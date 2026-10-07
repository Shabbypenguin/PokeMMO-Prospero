#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PokeMMO-Prospero contributors
# Upload approach (hidden temporary name, rename, eboot.bin and param.json last) follows
# ps5-native-app-boilerplate's tools/deploy.sh (BlackBearReloaded, GPL-3.0-or-later).
"""Install a PokeMMO-Prospero title on a PS5 over FTP, and optionally upload your ROMs.

Run it with no arguments and it asks for everything. Needs only Python 3.8+ (no extra packages) and an FTP
server running on the console (for example ftpsrv, port 2121).

  title     -> /data/homebrew/<TITLE_ID>/          (picked up by your title mounter, e.g. ShadowMountPlus)
  ROMs      -> /data/homebrew/<TITLE_ID>/roms/     (the title reads them from /app0/roms)

The PokeMMO client itself is not uploaded: the title downloads it from PokeMMO on the console.
"""

import argparse
import ftplib
import getpass
import io
import json
import os
import posixpath
import re
import socket
import sys
import tempfile
import time
import zipfile
from pathlib import Path

HOMEBREW = "/data/homebrew"
ROM_SUFFIXES = {".nds", ".gba"}
ROM_SCAN_DEPTH = 2
CRITICAL_LAST = ("eboot.bin", "sce_sys/param.json")  # published last so a half-upload never looks complete
BLOCK = 256 * 1024


# ---- small terminal helpers ------------------------------------------------------------------
def say(text=""):
    print(text, flush=True)


def ask(prompt, default=None):
    suffix = f" [{default}]" if default not in (None, "") else ""
    try:
        answer = input(f"{prompt}{suffix}: ").strip()
    except EOFError:
        answer = ""
    return answer or (default if default is not None else "")


def ask_yes_no(prompt, default=False):
    hint = "Y/n" if default else "y/N"
    while True:
        answer = ask(f"{prompt} ({hint})").lower()
        if not answer:
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        say("  Please answer y or n.")


def clean_path(text):
    """Accept paths pasted or drag-and-dropped into a terminal (quotes, escaped spaces, ~)."""
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        text = text[1:-1]
    if os.name != "nt":
        text = text.replace("\\ ", " ")
    return Path(os.path.expanduser(text))


def human(size):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024


# ---- saved settings (host, port, user; never the password) ------------------------------------
def config_path():
    base = os.environ.get("APPDATA") if os.name == "nt" else os.environ.get("XDG_CONFIG_HOME", "~/.config")
    return Path(os.path.expanduser(base or "~")) / "pokemmo-prospero" / "installer.json"


def load_config():
    try:
        return json.loads(config_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_config(values):
    path = config_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(values, indent=2) + "\n", encoding="utf-8")
    except OSError as error:
        say(f"  (could not save settings to {path}: {error})")


# ---- the title package -------------------------------------------------------------------------
class Package:
    def __init__(self, root):
        self.root = root
        param = json.loads((root / "sce_sys" / "param.json").read_text(encoding="utf-8"))
        self.title_id = param.get("titleId", "")
        if not re.fullmatch(r"[A-Z0-9]{9}", self.title_id):
            raise ValueError(f"param.json has an invalid titleId: {self.title_id!r}")
        names = param.get("localizedParameters", {})
        self.name = names.get(names.get("defaultLanguage", "en-US"), {}).get("titleName", self.title_id)
        self.version = param.get("contentVersion", "?")
        for required in ("eboot.bin", "sce_module/libc.prx", "sce_sys/param.json"):
            if not (root / required).is_file():
                raise ValueError(f"package is missing {required}")

    def files(self):
        everything = sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*") if p.is_file())
        return [f for f in everything if f not in CRITICAL_LAST] + list(CRITICAL_LAST)


def title_dir_in(directory):
    """A title folder is a directory named after its title id (PPSA##### or another 9 characters), or one containing sce_sys/param.json."""
    if (directory / "sce_sys" / "param.json").is_file():
        return directory
    candidates = [d for d in directory.iterdir() if d.is_dir() and (d / "sce_sys" / "param.json").is_file()]
    return candidates[0] if len(candidates) == 1 else None


def open_package(path, scratch):
    if path.is_dir():
        found = title_dir_in(path)
        if not found:
            raise ValueError(f"no title folder (with sce_sys/param.json) in {path}")
        return Package(found)
    try:
        archive = zipfile.ZipFile(path)
    except zipfile.BadZipFile as error:
        raise ValueError(f"{path.name} is not a valid zip ({error}); the download may be incomplete") from error
    with archive:
        bad = archive.testzip()
        if bad:
            raise ValueError(f"{path.name} is damaged ({bad} fails its checksum); download it again")
        for member in archive.namelist():
            target = (scratch / member).resolve()
            if not str(target).startswith(str(scratch.resolve())):
                raise ValueError(f"unsafe path in archive: {member}")
        archive.extractall(scratch)
    found = title_dir_in(scratch)
    if not found:
        raise ValueError(f"{path.name} does not contain a title folder")
    return Package(found)


def find_default_package():
    """Next to this script, or in ../dist when run from a checkout: the newest title zip or folder."""
    here = Path(__file__).resolve().parent
    candidates = []
    for directory in (here, here.parent, here.parent / "dist"):
        if directory.is_dir():
            candidates += [p for p in directory.glob("*.zip") if "pokemmo-prospero" in p.name.lower() or re.match(r"[A-Z0-9]{9}", p.name)]
            candidates += [p for p in directory.iterdir() if p.is_dir() and re.fullmatch(r"[A-Z0-9]{9}", p.name)]
    candidates = sorted(set(candidates), key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates


# ---- FTP ---------------------------------------------------------------------------------------
def code_of(error):
    return str(error)[:3]


def exists_dir(ftp, path):
    current = ftp.pwd()
    try:
        ftp.cwd(path)
        return True
    except ftplib.error_perm:
        return False
    finally:
        ftp.cwd(current)


def ensure_dir(ftp, path):
    built = ""
    for part in [p for p in path.split("/") if p]:
        built += "/" + part
        if not exists_dir(ftp, built):
            ftp.mkd(built)


def remote_size(ftp, path):
    """Size of a remote file, or None if it doesn't exist or the server can't tell."""
    try:
        ftp.voidcmd("TYPE I")
        size = ftp.size(path)
        return int(size) if size is not None else None
    except (ftplib.error_perm, ftplib.error_reply, ValueError):
        return None


def remove_quietly(ftp, path):
    try:
        ftp.delete(path)
    except ftplib.error_perm:
        pass


def upload(ftp, local, remote, label):
    with local.open("rb") as source:
        upload_stream(ftp, source, local.stat().st_size, remote, label)


def upload_stream(ftp, source, total, remote, label):
    """Upload a readable binary stream under a hidden temporary name, then rename it into place."""
    directory, name = posixpath.split(remote)
    temporary = posixpath.join(directory, f".{name}.upload")
    ensure_dir(ftp, directory)
    remove_quietly(ftp, temporary)
    sent = 0
    started = time.monotonic()
    last_draw = 0.0

    def progress(block):
        nonlocal sent, last_draw
        sent += len(block)
        now = time.monotonic()
        if now - last_draw > 0.2 or sent == total:
            last_draw = now
            rate = sent / max(now - started, 1e-6)
            percent = 100 * sent // total if total else 100
            sys.stdout.write(f"\r  {label}  {percent:3d}%  {human(sent)} / {human(total)}  {human(rate)}/s   ")
            sys.stdout.flush()

    ftp.storbinary(f"STOR {temporary}", source, blocksize=BLOCK, callback=progress)
    if total == 0:
        progress(b"")
    remove_quietly(ftp, remote)
    ftp.rename(temporary, remote)
    sys.stdout.write("\n")


def sync_files(ftp, pairs, always=()):
    """Upload (local, remote, label) pairs, skipping files whose remote size already matches."""
    uploaded = skipped = 0
    for local, remote, label in pairs:
        if label not in always and remote_size(ftp, remote) == local.stat().st_size:
            say(f"  {label}  unchanged, skipped")
            skipped += 1
            continue
        upload(ftp, local, remote, label)
        uploaded += 1
    return uploaded, skipped


def connect(host, port, user, password):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=20)
    ftp.login(user, password)
    ftp.set_pasv(True)
    return ftp


# ---- ROMs --------------------------------------------------------------------------------------
def remote_roms(ftp, remote_dir):
    """ROMs already on the console as {lower-case name: (name, size or None)}; empty if the folder doesn't exist."""
    if not exists_dir(ftp, remote_dir):
        return {}
    found = {}
    try:
        for name, facts in ftp.mlsd(remote_dir, facts=["type", "size"]):
            if facts.get("type") == "file" and Path(name).suffix.lower() in ROM_SUFFIXES:
                size = facts.get("size")
                found[name.lower()] = (name, int(size) if size and size.isdigit() else None)
        return found
    except ftplib.error_perm:  # server without MLSD: plain names, then ask for each size
        pass
    for entry in ftp.nlst(remote_dir):
        name = posixpath.basename(entry)
        if Path(name).suffix.lower() in ROM_SUFFIXES:
            found[name.lower()] = (name, remote_size(ftp, f"{remote_dir}/{name}"))
    return found


def show_remote_roms(existing, remote_dir):
    if not existing:
        say(f"No ROMs on the console yet ({remote_dir}/).")
        return
    say(f"ROMs already on the console ({remote_dir}/):")
    for name, size in sorted(existing.values(), key=lambda item: item[0].lower()):
        say(f"  {name}" + (f"  ({human(size)})" if size is not None else ""))


def scan_roms(folder):
    found = []
    base_depth = len(folder.parts)
    for path in sorted(folder.rglob("*")):
        if len(path.parts) - base_depth > ROM_SCAN_DEPTH:
            continue
        if path.is_file() and path.suffix.lower() in ROM_SUFFIXES:
            found.append(path)
    names = {}
    for path in found:  # the console sees one flat roms/ folder: refuse duplicate names
        names.setdefault(path.name.lower(), []).append(path)
    clashes = [paths for paths in names.values() if len(paths) > 1]
    return found, clashes


def rom_step(ftp, package, rom_folder, assume_yes, interactive):
    remote_dir = f"{HOMEBREW}/{package.title_id}/roms"
    say()
    existing = remote_roms(ftp, remote_dir)
    show_remote_roms(existing, remote_dir)
    if rom_folder is None and interactive:
        say()
        say("PokeMMO needs ROMs of the original games, dumped from cartridges you own")
        say("(.nds for Black/White, HeartGold/SoulSilver, Platinum; .gba for FireRed/LeafGreen, Emerald).")
        question = "Upload more ROMs?" if existing else "Upload ROMs to the console now?"
        if not ask_yes_no(question, default=False):
            say("Skipping ROMs. Run this installer again any time to add them.")
            return
        while True:
            rom_folder = clean_path(ask("Folder that contains your ROMs (you can drag it into this window)"))
            if rom_folder.is_dir():
                break
            say(f"  Not a folder: {rom_folder}")
    if rom_folder is None:
        return
    if not rom_folder.is_dir():
        raise ValueError(f"ROM folder not found: {rom_folder}")

    roms, clashes = scan_roms(rom_folder)
    if clashes:
        for paths in clashes:
            say("  Same file name in more than one place: " + ", ".join(str(p) for p in paths))
        raise ValueError("rename the duplicates so every ROM has a unique file name")
    if not roms:
        say(f"No .nds or .gba files found in {rom_folder} (searched {ROM_SCAN_DEPTH} folders deep). Skipping ROMs.")
        return
    say(f"Found {len(roms)} ROM(s):")
    for path in roms:
        size = path.stat().st_size
        _, remote = existing.get(path.name.lower(), (None, None))
        note = "already on the console" if remote == size else "replaces the console's copy" if path.name.lower() in existing else "new"
        say(f"  {path.name}  ({human(size)}, {note})")
    if interactive and not assume_yes and not ask_yes_no(f"Upload these to {remote_dir}/?", default=True):
        say("Skipping ROMs.")
        return
    uploaded, skipped = sync_files(ftp, [(p, f"{remote_dir}/{existing.get(p.name.lower(), (p.name,))[0]}", f"roms/{p.name}") for p in roms])
    say(f"ROMs done: {uploaded} uploaded, {skipped} already there.")


def write_rom_index(ftp, remote_dir):
    """roms/.prospero-index: the ROM names, one per line. A PS5 title cannot list folders with the C library, so the loader
    falls back to this file when its own listing is refused."""
    names = sorted(name for name, _ in remote_roms(ftp, remote_dir).values())
    if not names:
        return
    data = ("\n".join(names) + "\n").encode()
    import io
    upload_stream(ftp, io.BytesIO(data), len(data), f"{remote_dir}/.prospero-index", "roms/.prospero-index")


# ---- developer builds: the client from a PokeMMO-Client.zip -------------------------------------------
# Release builds download the client on the console. A developer build (milestone 1 of the loader) has no downloader yet,
# so the Linux part of the official zip is uploaded into the title folder (client/), with a manifest the loader copies from.
# Never put the client in a release package: it is PokeMMO's, and the project does not redistribute it.
def client_entries(archive):
    keep = []
    for info in archive.infolist():
        name = info.filename
        if info.is_dir() or ".." in name.split("/"):
            continue
        if name.startswith("bin/") and not name.startswith("bin/linux/x64/"):
            continue  # Windows, macOS and ARM binaries
        if name.lower().endswith((".exe", ".sh", ".bat", ".command")) or name.startswith(("log/", "roms/")):
            continue
        keep.append(info)
    return keep


def client_step(ftp, package, client_zip):
    import io
    with zipfile.ZipFile(client_zip) as archive:
        entries = client_entries(archive)
        if not any(e.filename == "bin/linux/x64/PokeMMO" for e in entries) or not any(e.filename == "revision.txt" for e in entries):
            raise ValueError(f"{client_zip.name} is not a PokeMMO client zip (no bin/linux/x64/PokeMMO or revision.txt)")
        revision = archive.read("revision.txt").decode(errors="replace").strip()
        total = sum(e.file_size for e in entries)
        remote_root = f"{HOMEBREW}/{package.title_id}/client"
        say()
        say(f"Developer client: revision {revision}, {len(entries)} files, {human(total)} -> {remote_root}/")
        uploaded = skipped = 0
        for entry in entries:
            if entry.filename == "revision.txt":
                continue  # last: the loader copies the client again only when the revision changes
            remote = f"{remote_root}/{entry.filename}"
            if remote_size(ftp, remote) == entry.file_size:
                skipped += 1
                continue
            with archive.open(entry) as source:
                upload_stream(ftp, source, entry.file_size, remote, f"client/{entry.filename}")
            uploaded += 1
        manifest = "".join(f"{e.file_size} {e.filename}\n" for e in entries if e.filename != "revision.txt").encode()
        upload_stream(ftp, io.BytesIO(manifest), len(manifest), f"{remote_root}/manifest.txt", "client/manifest.txt")
        data = archive.read("revision.txt")
        upload_stream(ftp, io.BytesIO(data), len(data), f"{remote_root}/revision.txt", "client/revision.txt")
        say(f"Client done: {uploaded} uploaded, {skipped} unchanged.")


# ---- main --------------------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", type=Path, help="title zip or folder (default: newest one next to this script)")
    parser.add_argument("--host", help="console IP address")
    parser.add_argument("--port", type=int, help="FTP port (default 2121)")
    parser.add_argument("--user", help="FTP user (default anonymous)")
    parser.add_argument("--password", help="FTP password (default: none / prompt)")
    rom_group = parser.add_mutually_exclusive_group()
    rom_group.add_argument("--roms", type=Path, metavar="FOLDER", help="upload ROMs from this folder")
    rom_group.add_argument("--no-roms", action="store_true", help="don't ask about ROMs")
    parser.add_argument("--yes", action="store_true", help="no questions: use arguments and saved settings")
    parser.add_argument("--client", type=Path, metavar="ZIP",
                        help="developer builds only: upload the Linux part of this PokeMMO-Client.zip into the title folder")
    parser.add_argument("--redownload-client", action="store_true",
                        help="make the console forget its installed client and download it again on the next start (settings stay)")
    args = parser.parse_args()
    interactive = not args.yes and sys.stdin.isatty()

    say("PokeMMO-Prospero installer")
    say("Unofficial; not affiliated with PokeMMO. The title downloads the PokeMMO client itself.")
    say()

    with tempfile.TemporaryDirectory(prefix="pokemmo-prospero-") as scratch:
        # 1. Which title
        package_path = args.package
        if package_path is None:
            candidates = find_default_package()
            if not candidates:
                raise ValueError("no title package found next to the installer; pass --package <zip or folder>")
            package_path = candidates[0]
            if interactive and len(candidates) > 1:
                say("Title packages found:")
                for index, candidate in enumerate(candidates, 1):
                    say(f"  {index}. {candidate.name}")
                choice = ask("Which one", "1")
                if not choice.isdigit() or not 1 <= int(choice) <= len(candidates):
                    raise ValueError("invalid choice")
                package_path = candidates[int(choice) - 1]
        package = open_package(clean_path(str(package_path)), Path(scratch))
        files = package.files()
        size = sum((package.root / f).stat().st_size for f in files)
        say(f"Title: {package.name} ({package.title_id}, version {package.version}), {len(files)} files, {human(size)}")

        # 2. Where to
        saved = load_config()
        host = args.host or saved.get("host", "")
        port = args.port or saved.get("port", 2121)
        user = args.user or saved.get("user", "anonymous")
        password = args.password
        if interactive:
            say()
            say("Console FTP server (start your FTP payload on the PS5 first):")
            host = ask("  PS5 IP address", host)
            port_text = ask("  FTP port", str(port))
            if not port_text.isdigit():
                raise ValueError(f"invalid port: {port_text}")
            port = int(port_text)
            user = ask("  FTP user", user)
            if password is None:
                password = getpass.getpass("  FTP password (Enter for none): ")
        if not host:
            raise ValueError("no console address (use --host)")
        password = password or ""

        say()
        say(f"Connecting to {host}:{port} ...")
        try:
            ftp = connect(host, port, user, password)
        except (OSError, ftplib.Error) as error:
            raise ConnectionError(
                f"could not connect to ftp://{host}:{port} ({error}).\n"
                "  Check that the FTP payload is running on the PS5, the IP and port are right,\n"
                "  and this computer is on the same network.") from error
        save_config({"host": host, "port": port, "user": user})  # only settings that actually connected

        with ftp:
            remote_root = f"{HOMEBREW}/{package.title_id}"
            if "PokeMMO-Prospero ROM upload" in (ftp.getwelcome() or ""):
                # The title's own upload server (its ROM screen is open): it only takes ROMs.
                say("Connected to the PokeMMO Prospero ROM screen: only ROMs can be uploaded this way.")
                rom_step(ftp, package, clean_path(str(args.roms)) if args.roms else None, args.yes, interactive)
                say()
                say("Done. Press Cross on the console to check the ROMs again.")
                return 0
            if not exists_dir(ftp, "/data"):
                raise ValueError("the FTP server doesn't show /data: is this the PS5's FTP server with full access?")
            ensure_dir(ftp, HOMEBREW)
            say(f"Installing to {remote_root}/  (close the title on the PS5 first if it is running)")
            pairs = [(package.root / f, f"{remote_root}/{f}", f) for f in files]
            uploaded, skipped = sync_files(ftp, pairs, always=set(CRITICAL_LAST))
            listing = ftp.nlst(remote_root)
            if not any(name.rsplit("/", 1)[-1] == "eboot.bin" for name in listing):
                raise RuntimeError("upload finished but eboot.bin is not on the console")
            say(f"Title done: {uploaded} uploaded, {skipped} unchanged.")

            # 3. Developer builds: the client
            if args.client:
                client_step(ftp, package, clean_path(str(args.client)))

            if args.redownload_client:
                marker = b"redownload\n"
                upload_stream(ftp, io.BytesIO(marker), len(marker), f"{remote_root}/redownload-client", "redownload-client")
                say("The console will download the PokeMMO client again on its next start.")

            # 4. ROMs
            if not args.no_roms:
                rom_step(ftp, package, clean_path(str(args.roms)) if args.roms else None, args.yes, interactive)
            write_rom_index(ftp, f"{remote_root}/roms")

    say()
    say(f"All done. Launch \"{package.name}\" from the PS5 home screen")
    say("(if it doesn't appear, refresh or restart your title mounter, e.g. ShadowMountPlus).")
    return 0


def run():
    try:
        code = main()
    except KeyboardInterrupt:
        say("\nCancelled.")
        code = 130
    except (ValueError, ConnectionError, RuntimeError, OSError, ftplib.Error, zipfile.BadZipFile, socket.timeout) as error:
        say(f"\nError: {error}")
        code = 1
    if os.environ.get("POKEMMO_PROSPERO_PAUSE") == "1":  # set by the double-click launchers so the window stays open
        try:
            input("\nPress Enter to close this window.")
        except EOFError:
            pass
    return code


if __name__ == "__main__":
    sys.exit(run())
