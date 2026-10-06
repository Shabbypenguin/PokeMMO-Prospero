# Plan

## Phase 1 — client analysis (done)

Client revision 32920, `bin/linux/x64/PokeMMO` (re-run `make analyze` on every release):

| Fact | Value | Consequence |
|------|-------|-------------|
| Format | GraalVM native image, PIE, 100 MiB | no JVM; one ELF to load |
| NEEDED | libc.so.6, libz.so.1 | everything else is dlopen()ed |
| Imports | 254 | same set as the ARM64 build PokeMMO-NX shims, plus adler32/crc32 (zlib) |
| Relocations | RELATIVE, 64, GLOB_DAT, JUMP_SLOT | trivial |
| PT_TLS | none | no Linux TLS block needed |
| Raw `syscall` | 0 | every kernel call goes through libc → shimmable |
| Segment registers | `%fs:0x28` ×129 (stack canary) | needs a stable per-thread value; probe measures it |
| C++ runtime | linked statically | no Debian libstdc++ (the ARM64 build needs it) |
| Runtime libraries | SDL3, OpenAL, libGL/libEGL/libGLX, GTK (file chooser), lwjgl natives built in | all virtual in the loader |
| GLSL | legacy (no `#version`, `gl_FragColor`, `texture2D`), one `#version 130` | needs a compatibility context |
| Code generation | only libffi closure trampolines (LWJGL callbacks) | small exec pool, or prebuilt trampolines |

## Phase 1b — hardware probe (done)

`probe/` answers on firmware 12.40: compatibility context + legacy GLSL + blending on ps5-opengl,
`%fs:0x28` stability, thread stack control, address-space reservations, flexible vs direct memory (and direct memory
mapped at a fixed address inside a reservation, how GraalVM commits its heap), `/download0`,
network, and which executable-memory route works. probe-2 and probe-3 settled graphics, memory, executable memory and
network (see Hardware status). Directory listing and audio output are verified by the loader's first builds
instead of another probe run (see Phase 2).

## Phase 2 — loader core

Port PokeMMO-NX's loader from Horizon/ARM64 to PS5/x86-64:

- ELF loader: x86-64 relocations, PIE base, `.init_array`.
- `%fs:0x28`: if the probe shows it unstable, rewrite the 129 canary reads at load time into RIP-relative loads of a
  per-process guard (in memory only; the client on disk stays unmodified).
- libc shim on FreeBSD: files (virtual root under `/download0`), threads (`pthread_getattr_np` → `pthread_attr_get_np`),
  sync, mmap with Linux flag/errno translation, sockets/epoll (→ kqueue), time, signals.
- Memory (probe-3): anonymous `mmap` can't reserve address space, so the mmap shim serves GraalVM's large
  `PROT_NONE` reservations from `sceKernelReserveVirtualRange` (works up to 16 GiB) and commits **direct** memory at
  fixed addresses inside them; small mappings stay anonymous. The client gets `-XX:ReservedAddressSpaceSize` below
  16 GiB so its reservation fits.
- Directory listing: `opendir` is refused in titles (probe-3) but the client calls `opendir`/`readdir64`. The shim
  lists with `getdents` (`sceKernelGetdents`), checked on the loader's first run and logged. Fallback if that is
  refused too: the shim keeps an index of what it creates in `/download0`, and the installer writes a ROM list.
- Name resolution: `getaddrinfo` crashes in titles, so the shim implements it on `sceNetResolver`.
- libffi closures: JIT shared memory and RWX pages are refused; closures come from a pool of pages written while RW
  and flipped to RX with `mprotect` (the route probe-3 proved), like PokeMMO-NX's own RW/RX transitions.
- Page size is 16 KiB. The client's segments are 64 KiB-aligned and its RELRO ends on a 16 KiB boundary, so they
  map without changes.
- Launch arguments as on the Switch: `-XX:MaxHeapSize=640m -XX:MaxNewSize=128m`.
- Milestone: client reaches its main loop, logs over UDP.

## Phase 3 — graphics

Virtual libEGL/libGL backed by ps5-opengl, EGL window at 1080p. Milestone: login screen renders.

## Phase 4 — input, audio, text

The client has its own controller support (through SDL3's gamepad API) and a controller-friendly "Android" UI theme,
so the loader presents the PS5 controller as an SDL3 gamepad rather than inventing a mapping. PokeMMO-NX shows what
works on a console (see its README and `linux_sdl*.c`):

| Need | PokeMMO-NX (Switch) | PS5 plan |
|------|---------------------|----------|
| Buttons, sticks | virtual SDL3 gamepad; face buttons by printed label (A confirms, B cancels) | same, Cross confirms / Circle cancels (configurable) |
| Select / Start | Minus → SDL Back, Plus → SDL Start | Create (Share), PS and mic are taken by the system and never reach a title, so **touchpad click → SDL Back** (select) and Options → SDL Start |
| Gamepad name | kept short ("Switch Controller"): a long name makes the Android theme's settings pages loop and lag | same rule, e.g. "PS5 Controller" |
| Keyboard (login, chat) | R3 toggles the console's inline keyboard; game input is held at rest while it is up | R3 toggles our own on-screen keyboard drawn with GL (the system IME dialog module doesn't load in a title: probe-3, `0x80020002`); typed text goes to the game as SDL text events. USB keyboards: not planned (untested) |
| Mouse | L3 toggles a stick-driven cursor (ZR/ZL click) | same on L3 with R2/L2 click; while the cursor is on, touchpad movement also moves it (its click stays Back) |
| File chooser | drawn over the game, shows the game folders and SD card | same, over `/download0`, `/app0` and any readable ROM route |
| Audio | OpenAL shim on the Switch's audio out | OpenAL shim on AudioOut, 48 kHz |

Not in PokeMMO-NX despite expectations: an updater. Its releases bundle a fixed client revision (32920 in 1.0.0); the
on-console updater in Phase 5 is our own.

## Phase 5 — launcher and on-console client updates

The title fetches PokeMMO's client itself, so releases never contain PokeMMO files and players don't need a PC
to update. Updates are **on by default**, with a button to skip.

1. **Check.** `HEAD https://dl.pokemmo.com/download/PokeMMO-Client.zip` and compare the `ETag` (plus
   `Content-Length`/`Last-Modified`) with the installed one. Observed 2026-10-05: `accept-ranges: bytes`,
   `etag: "6a822758-102e8cfe"` (nginx-style: hex mtime – hex size; `0x102e8cfe` = 271486206 bytes = revision 32920,
   mtime = `Last-Modified` 2026-08-16), served through Cloudflare (`max-age=14400`, so a new release can take up to
   4 h to show).
2. **Show versions.** Range requests are supported, so read the zip's central directory (the last ~64 KiB), then
   `revision.txt` alone, to show "32920 → 329xx" without downloading 271 MB.
3. **Splash and prompt.** The splash always shows the launcher build (`PROSPERO_VERSION`: release tag or commit,
   generated by `scripts/build-title.sh`) and the installed client revision (`revision.txt` of the current slot), plus
   the latest revision once the check returns. On a newer one: "Update available — ✕ Download / ○ Skip",
   auto-continuing with the default after a few seconds.
   First launch with no client: download is required.
4. **Download** over HTTPS with certificate verification (console `sceHttp`, proven in sandboxed titles, or
   libcurl), resumable with range requests, progress bar.
5. **Extract only what the PS5 needs** (`bin/linux/x64/PokeMMO`, `data/`, `config/`, `revision.txt`; ~320 MB of
   Windows/macOS binaries skipped), checking each entry's CRC.
6. **Verify, then switch.** x86-64 ELF check, then the loader confirms it can resolve every import. Two slots
   (A/B) in `/download0`; "current" moves only after verification. A version that fails to load falls back to the
   previous one, with the reason on screen.

The client has its own updater (revision 32920 strings: `-updater_feeds:`, `-updater_sigs:`, `--update`,
`updater.tmp`, "Newly downloaded updater.tmp failed SHA256 check", "Error attempting auto restart"). It downloads a
signed updater and then needs to start a new process (`ProcessBuilder` → `posix_spawn`/`vfork`/`execve`). Neither
console can start a second process from a title, and PokeMMO-NX refuses those calls (unimplemented imports are bound
to traps that return an error), so on the Switch the built-in updater cannot finish; PokeMMO-NX instead ships a fixed
client revision. Our loader will do the same refusal, log the attempt, and show "update downloaded: restart the
title", while the pre-launch updater above keeps the client current, so in normal use the client finds itself up to
date and never tries. Unknown until the client runs: whether
data-only updates apply in-process without a restart.

## Phase 6 — packaging

Release zip = title folder + installer (`installer/`), icon/backgrounds, ROM instructions.

### Install layout

| What | On the console (FTP) | Seen by the title as | Written by |
|------|----------------------|----------------------|------------|
| Title | `/data/homebrew/PPSA27166/` | `/app0/` (read-only) | installer |
| ROMs | `/data/homebrew/PPSA27166/roms/` | `/app0/roms/` (read-only) | installer (lists what is already there, offers to add more) |
| Client slots, caches, config, logs | inside the title storage image | `/download0/` | the title |

`/download0` is a storage image, not a folder FTP can browse, so logs go out over UDP. Probe check `fs.app0roms` confirms the ROM
path on real hardware.

A single-file install (`.ffpfsc` image, or a future fpkg) is read-only, so ROMs can't live inside it. The other routes
are probed: `fs.dataroms` (read a `/data` folder directly) and `fs.usb` (in-app import from a USB drive into
`/download0`). The folder install stays the default until one of them is proven. `downloadDataSize` must cover two client slots + caches (~1.5–2 GB).

## Open risks

1. ps5-opengl is validated on 6.02 only; SDK 1.0.1 is CI-built, not console-validated.
2. Anonymous mmap draws on flexible memory, which ps5-opengl notes holds only a few hundred MiB. The 640 MiB Java heap will
   needs direct memory behind the mmap shim (confirmed by probe-3; route in Phase 2). The app heap caps native `malloc` at
   128 MiB by default; native allocations (Mesa, SDL shim, client natives) must fit, so the title may need a bigger one.
3. Title sandbox: writable storage is `/download0` only (size set in `param.json`) and it is not reachable over FTP;
   ROMs go next to the title instead (see the install layout above; to be confirmed by `fs.app0roms`).
4. Firmware updates can break the jailbreak chain independently of this project.

## Hardware status

| Firmware | Console setup | Probe result | Date |
|----------|---------------|--------------|------|
| 12.40 | kstuff-lite 1.07+, ShadowMountPlus 1.7beta3 | probe-2: graphics, canary, threads, direct memory, storage pass; crashed in `getaddrinfo` (see below) | 2026-10-05 |
| 12.40 | same | probe-3: 20 pass, 7 fail, 4 info; crashed once in `getaddrinfo`, finished on relaunch (crash-resume) | 2026-10-05 |

### probe-2 findings (12.40)

- **Graphics: everything passes.** ps5-opengl returns `4.6 (Compatibility Profile) Mesa 26.2.0` for the client's
  GL 2.1 request; legacy GLSL 110/120/130, client-side arrays, VBOs without a VAO, blending and immediate mode all
  render correctly.
- **`%fs:0x28` is 0 on every thread and never changes**, so the client's 129 canary checks work without patching.
- **Threads:** 8 MiB stacks and `pthread_attr_get_np` (for `pthread_getattr_np`) work; main thread stack 2 MiB.
- **Memory:** anonymous `mmap` is limited to the flexible budget (418 MiB at start, 256 MiB committable) and even a
  1 GiB `PROT_NONE` reservation fails (ENOMEM). Direct memory is large (11.7 GiB pool, 2 GiB mapped in the test).
  The loader therefore backs GraalVM's heap with direct memory; probe-3 tests reserving the range with
  `sceKernelReserveVirtualRange` and committing direct memory at fixed addresses inside it.
- **Storage:** `/download0` works. `/data` is not visible inside the title (ENOENT) at least while
  ShadowMountPlus's sandbox mounts are not applied; `/app0/roms` returned EPERM (probe-3 logs the sandbox view).
- **Network:** UDP works (the log arrived). `getaddrinfo` crashed the title three times: in titles it comes from
  `libScePosixForWebKit`. probe-3 resolves with the console's `sceNetResolver`; the loader will do the same.

### probe-3 findings (12.40)

- **Heap route found.** `sceKernelReserveVirtualRange` reserves 1, 4 and 16 GiB (32 GiB: `0x8002000c`). Direct
  memory maps at fixed addresses inside the range (chunks at +0, +1 and +3 GiB), anonymous `MAP_FIXED` works inside
  it, `mprotect` to NONE and back keeps the contents, and `munmap` releases it. A plain `mmap` reservation still
  fails, so `vm.reserve`, `vm.fixed` and `vm.directfixed` stay red; they are superseded by `vm.vrange*`.
- **Executable memory:** RWX pages map but fault when executed, JIT shared memory is refused (`0x80020001`), and
  RW→RX `mprotect` works. libffi closures use the `mprotect` route.
- **Network:** `sceNetResolver` resolves pokemmo.com and TCP to port 443 connects in 21 ms. `getaddrinfo` crashed
  the title again (expected; the check stays in so a firmware change that fixes it shows up).
- **Files:** `opendir` fails with EPERM on every path, even `/download0`, which the title can write to, while
  `stat("/app0/roms")` succeeds (`040777`). The ROM uploaded with the installer was therefore not seen. The libc
  directory call is what's blocked, not access; probe-4 lists with `getdents` and opens ROMs by name.
  `/data`, `/mnt` and `/user` don't exist in the title's view (ENOENT), so ShadowMountPlus's sandbox mounts were not
  applied, and the USB drive was not visible.
- **Input:** every pad button, both sticks and the touchpad report (see the button table in `probe/probe.c`). The
  IME dialog module fails to load (`0x80020002`) at both paths tried.
- **ShadowMountPlus keeps a copy.** Reinstalling over FTP left the old probe running from SMP's virtual drive until
  the title was uninstalled from the home screen and SMP re-added it. Documented in the build environment's
  console setup.
