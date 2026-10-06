# Installer

Copies a PokeMMO-Prospero title to your PS5 over FTP and, if you want, your ROMs too. Works on Windows, macOS and Linux.

## Before you start

- The PS5 has a homebrew environment that runs folder titles from `/data/homebrew` (see the buildenv repo's
  `docs/console-setup.md`), and an **FTP server payload is running** on it (e.g. ftpsrv, port 2121).
- Your computer is on the same network as the PS5.
- **Python 3.8 or newer.** Windows: get it from <https://www.python.org/downloads/> and tick
  "Add python.exe to PATH". macOS: the first run of `python3` offers to install it. Linux: usually already there.

## Run it

Unzip the release, then:

| OS | Do this |
|----|---------|
| Windows | double-click `install.bat` |
| macOS | double-click `install.command` (first time: right-click → Open, because it is unsigned) |
| Linux | `./install.sh` in a terminal |

It asks for:

1. **The title** to install, if there is more than one package next to it.
2. **PS5 IP address, FTP port, user and password.** The address, port and user are remembered for next time; the
   password never is. ftpsrv's defaults are port 2121, user `anonymous`, no password.
3. **Whether to upload ROMs.** It first lists the ROMs already on the console, then asks whether to upload (more).
   Point it at the folder that holds them (you can drag the folder into the window); files already there are skipped.
   It uploads the `.nds` and `.gba` files it finds, up to two folders deep.

Running it again is safe and quick: unchanged files are skipped, and the title's `eboot.bin` and `param.json` are
always replaced last, so an interrupted install never looks complete.

## Where things go

| What | Path on the PS5 |
|------|-----------------|
| Title | `/data/homebrew/<TITLE_ID>/` |
| ROMs | `/data/homebrew/<TITLE_ID>/roms/` |

The PokeMMO client is **not** uploaded: the title downloads it from PokeMMO itself and keeps it updated.

## ROMs

PokeMMO needs ROMs of the original games, and you must dump them from cartridges you own. This project doesn't
provide or link to ROMs. Pokémon Black or White (`.nds`) is the base game. FireRed/LeafGreen and Emerald (`.gba`)
and HeartGold/SoulSilver and Platinum (`.nds`) add regions; see PokeMMO's own setup guide for the current list.

## Without questions (scripts, CI)

```bash
python3 pokemmo_prospero_install.py --yes --package pokemmo-prospero-probe-PPSA27165.zip \
    --host 192.168.1.50 --port 2121 --user anonymous --password "" --roms ~/roms
```

`--no-roms` skips the ROM step. `python3 pokemmo_prospero_install.py --help` lists every option.
