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

The screen shows one tile per step (green pass, red fail, blue information, yellow running):

| Step | What it answers |
|------|-----------------|
| fs.list.app0, fs.list.roms, fs.list.download0 | Does `getdents` list folders where `opendir` was refused? |
| fs.romread | Can every ROM be opened and its header read? |
| fs.app0write | Is the title folder writable? Blue = read-only. If it passed, check over FTP whether `/data/homebrew/PPSA27166/prospero-write-test.bin` appeared |
| fs.download0size | How much the title storage really holds (stops at 2 GiB) |
| sys.modules | Can system modules (SSL, HTTP, audio, the system keyboard) be loaded at run time, and from which path? |
| net.https | A HEAD request to dl.pokemmo.com through the system's HTTPS library |
| audio.tone | Half a second of a quiet tone through the system audio output |
| client.install | The developer client copied into the title storage |
| client.map, client.start, client.end | The client mapped, started, and how it ended |

Nothing in the checks can end the title: faults are caught, and slow calls get a time limit. A fault in the
client itself is logged with its address (`FATAL signal ...`) and that thread is parked, so the screen and
the log stay up.
