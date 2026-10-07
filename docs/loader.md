# The loader

`loader/` runs the official, unmodified PokeMMO Linux x86-64 client on a PS5. The design and most of the
code come from [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) (Petit_Prince, MIT): an ELF
loader maps the client, and a Linux ABI layer answers the C library calls the client and its native libraries
make. Every file taken from PokeMMO-NX keeps its notice in the header and says what changed (see also
[CREDITS.md](../CREDITS.md)).

## Layout

| Folder | What | Origin |
|--------|------|--------|
| `loader/src/elf_*.c` | ELF layout, staging, relocations, symbol versions, mapping | PokeMMO-NX, adapted to x86-64 and in-place staging |
| `loader/src/linux_*.c` | the Linux ABI: files, stdio, printf, threads, sync, memory, sockets, epoll/eventfd, zlib, dlopen, traps | PokeMMO-NX, adapted; `linux_sync`, `linux_semaphore`, `linux_tls` and `linux_vm`'s backend rewritten |
| `loader/src/linux_stubs_x86_64.c` | trap/virtual stubs and `__tls_get_addr` entry (assembly) | rewritten for x86-64 |
| `loader/src/game.c` | maps the client and runs its `main` on a 16 MiB thread | PokeMMO-NX, simplified |
| `loader/include/platform.h` | the only system interface the adapters use | new |
| `loader/platform/ps5/` | the PS5 title: platform layer, self-checks, screen | new |
| `loader/platform/host/` | the same loader for a Linux PC | new |

### What changed from the Switch

| Topic | Switch (PokeMMO-NX) | PS5 (here) |
|-------|---------------------|------------|
| CPU, client binary | ARM64, `bin/linux/arm64/PokeMMO` | x86-64, `bin/linux/x64/PokeMMO` |
| Struct layouts | glibc AArch64 | glibc x86-64 (`stat` 144 bytes, packed `epoll_event`, 40-byte mutex, 56-byte thread attr, 4-byte mutex/cond attrs) |
| Thread-local storage of loaded libraries | TLS descriptors on TPIDR_EL0 | general-dynamic `__tls_get_addr`, per-thread blocks created on first use (`%fs` belongs to the PS5's libc) |
| Executable memory | Horizon code memory | written, then `mprotect` to read+execute (RWX pages fault on the PS5) |
| Large memory | Horizon heap | address space from `sceKernelReserveVirtualRange`, direct memory committed in 64 KiB blocks |
| Directory listing | newlib `opendir` | `open` + `getdents` (titles get EPERM from `opendir`), installer index file as fallback |
| DNS | Horizon `getaddrinfo` | `sceNetResolver` (`getaddrinfo` crashes PS5 titles) |
| Page size | 4 KiB | 16 KiB system pages; the guest still sees 4 KiB pages |

## Running it on a PC

```bash
make fetch-client      # once: the official client into private/ (never committed)
make host-run          # builds build/host/prospero-host and runs the client in it
```

Expected today: the client starts (logback, config, LWJGL, SDL3, libgdx), then stops at
"Error attempting to set up hardware accelerated graphics", because no display exists yet.

## Running it on the console (milestone 1)

```bash
../pokemmo-ps5-buildenv/ps5env make package-loader PROBE_LOG_HOST=<your PC's IP>
```

Install with the installer from `dist/pokemmo-prospero-loader-installer.zip`, adding the client for this
developer build: `python3 pokemmo_prospero_install.py --client PokeMMO-Client.zip` (the installer uploads the
Linux part of the zip into the title folder as `client/`; release builds will download the client on the
console instead). Then run `python3 tools/udplog.py --out loader.log` and start **PokeMMO Prospero (dev)**.

Since loader-8 the screen shows the project logo, one progress bar and a line of status ("Checking files",
"Installing PokeMMO", "Starting PokeMMO"). It stays up until the client creates its OpenGL window, then hands the
display over. When starting cannot go on, the bar turns red with a plain-language reason. Hold **Triangle** to see
the steps behind the bar (green pass, red fail, blue note, yellow running):

| Step | What it answers |
|------|-----------------|
| fs.list.app0, fs.list.roms | Does `getdents` list folders where `opendir` was refused? |
| fs.romread | Can every ROM be opened and its header read? (Note = no ROMs uploaded) |
| sys.modules | Can system modules (SSL, HTTP, audio, the system keyboard) be loaded at run time, and from which path? |
| net.https | A HEAD request through the system's HTTPS library (Note until it is linked directly; the client does its own HTTPS) |
| client.install | The developer client copied into the title storage |
| client.map, client.start, client.end | The client mapped, started, and how it ended |

The storage probes of loader-1 (title folder writes, 1 GiB written to `/download0`) and loader-4's test tone answered
their questions and no longer run.

### In the game

| Input | Does |
|-------|------|
| R3 | On-screen keyboard on/off (select the text field first) |
| Keyboard up: d-pad / left stick, Cross | Move, type the highlighted key |
| Keyboard up: Square, Triangle | Delete, space (held: repeat) |
| Keyboard up: L1 / R1 | Move the text cursor |
| Keyboard up: L2 / R2 | Symbols page / shift (once, caps lock, off) |
| Keyboard up: touchpad, Options | Tab (next field), Enter |
| Keyboard up: L3, Circle | Move the keyboard to the top or bottom, close it |
| Exit in the game's menu | Closes the title and goes back to the home screen (loader-21) |
| A link in the game | Opens in the PS5 web browser (the PS button brings the game back); if the browser cannot start, a box shows the address and a QR code |

### Client slots (loader-18)

`/download0/root/slots/a` and `slots/b` each hold a complete client (a `.prospero-complete` marker names its revision);
`slots/state` says which is active. A new client (download or developer copy) goes into the other slot, which becomes
active "on trial". The trial ends when the game shows its first picture; the previous client is then deleted (loader-21), so
the second copy only exists while an update is on trial. If the game ends before that, or a start finds
a trial still open (the previous start crashed), the loader goes back to the previous slot, says so on the loading
screen, and does not offer that revision again until a newer one is published. The settings live in
`/download0/root/shared/config`, mounted over the game's `/game/config`, so both slots share them. The single client
folder of loader-17 and earlier (`/download0/root/game`) becomes slot a on the first start.

### ROMs (loader-16)

Before the game starts the loader reads the header of every file in the title's `roms/` folder (`loader/src/roms.c`):
the game code PokeMMO itself logs (`IRBO`, `BPRE`, ...) tells Black/White (required), FireRed, Emerald, Platinum and
HeartGold/SoulSilver (optional) from games PokeMMO does not use (Black 2/White 2, LeafGreen, Ruby/Sapphire, Diamond/Pearl)
and from compressed or unrelated files. Without Black or White a ROM screen stays up: what was found, what is missing, the
FTP details to upload with (address, port 2121 and folder, each on its own line) and the installer as the other way; Cross checks the folder again,
and there is no way round it (PokeMMO cannot get past its start without Black/White). With only optional games missing, the loading screen says how many were found and holding Square
shows the same screen.

### Uploading ROMs without an FTP payload (loader-19)

At start the loader checks whether an FTP server answers on the console (ports 2121, 1337, 21). While the ROM screen is up
it runs a web page on port 8080 (open `http://<console IP>:8080` in any browser on the network, or scan the QR code next to
it, and pick the files) and,
when no FTP server answered, a small FTP server on port 2121 that shows only the ROM folder
(`/data/homebrew/PPSA98001/roms`). Both write nothing but files in that folder, show progress on the ROM screen, re-read
the ROM list after each file, and stop before the game starts. The installer recognises the title's FTP server and only
uploads ROMs to it. `tools/upload_test.c` runs both servers on a PC.

### Title ID (loader-24)

The title is `PPSA98001`. Retail PS5 games use `PPSA` + five digits and are numbered far lower; PS5 homebrew settles in
`PPSA99xxx` (the [homebrew.page](https://homebrew.page/) catalog lists none in `PPSA98xxx`). A non-`PPSA` id
(`POKEMMOP5`) did not work on a console. Earlier builds used `PPSA27166`, which a retail game could also get, so a
`PPSA98001` install is a new title: its own storage, client download and ROM upload.

### Folder or image install (loader-23)

At start the loader checks whether the title folder (`/app0`) is writable. In a folder install it is, and nothing changes.
In an image install (`.ffpfsc`, mounted read-only) the ROMs live in `/download0/root/roms`, which only the title's own upload
page and FTP server can reach (the FTP server is started even when an FTP payload runs, on 2121 or the next free port up to
2125), the log is `/download0/root/prospero.log`, and the upload page links to the log and the game's settings for
download (`/log`, `/settings`). The settings backup copy in the title folder only exists in a folder install.

### Client updates (loader-15)

At every start the loader asks PokeMMO's download server whether the published client changed (one HEAD request; the
ETag of the installed revision is kept in `/download0/root/update-etag`). When it did, the zip's index and its
`revision.txt` are read with range requests; a newer revision is offered on the loading screen (Cross: download,
Circle: skip; download after six seconds). With no client installed it is downloaded straight away. Only what a PS5
runs is fetched (about 93 MB of the 271 MB zip, in two requests), unpacked as it arrives, every file checked against its
CRC-32, into the client slot that is not in use (below). A dropped connection resumes where it stopped. HTTPS is the system's own (`libSceHttp`, certificates checked).
`make updater-test` runs the same code on a PC against `tools/range_server.py` (`--drop-every` exercises the resume).
The installer's `--redownload-client` makes the console forget its client once and download it again.

The game's settings live in `config/main.properties` in the title storage, which FTP cannot see. The loader copies the
file to `/data/homebrew/PPSA98001/settings/main.properties` at start and within a minute of any change (loader-11).
Before the game starts, the loader applies `assets/settings/defaults.properties` (fullscreen 1920x1080, 60 fps, UI scale
1.25, the controller layout): in full on a new install, and once per `DEFAULTS_VERSION` on an existing one, so later changes in the game stay
(loader-12). Raise `DEFAULTS_VERSION` to push changed defaults to everyone once.

The keyboard (`loader/src/osk.c`) and the link box (`loader/src/link_box.c`) are the loader's own, drawn over the
game with `loader/src/overlay.c`; nothing typed is kept. `make overlay-preview` draws them (and the loading screen)
on a PC with Mesa into `build/overlay-preview/`, and checks what the keyboard types.

Nothing in the checks can end the title: faults are caught, and slow calls get a time limit. A fault in the
client itself is logged with its address (`FATAL signal ...`) and that thread is parked, so the screen and
the log stay up.
