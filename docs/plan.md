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

## Phase 1b — hardware probe (next: needs the console)

`probe/` answers on firmware 12.40: compatibility context + legacy GLSL + blending on ps5-opengl,
`%fs:0x28` stability, thread stack control, address-space reservations, flexible vs direct memory (and direct memory
mapped at a fixed address inside a reservation, how GraalVM commits its heap), `/download0`,
network, and which executable-memory route works.

## Phase 2 — loader core

Port PokeMMO-NX's loader from Horizon/ARM64 to PS5/x86-64:

- ELF loader: x86-64 relocations, PIE base, `.init_array`.
- `%fs:0x28`: if the probe shows it unstable, rewrite the 129 canary reads at load time into RIP-relative loads of a
  per-process guard (in memory only; the client on disk stays unmodified).
- libc shim on FreeBSD: files (virtual root under `/download0`), threads (`pthread_getattr_np` → `pthread_attr_get_np`),
  sync, mmap with Linux flag/errno translation, sockets/epoll (→ kqueue), time, signals.
- libffi closures: JIT shared memory if available, else a static trampoline table in the loader's own code.
- Launch arguments as on the Switch: `-XX:MaxHeapSize=640m -XX:MaxNewSize=128m`.
- Milestone: client reaches its main loop, logs over UDP.

## Phase 3 — graphics

Virtual libEGL/libGL backed by ps5-opengl, EGL window at 1080p. Milestone: login screen renders.

## Phase 4 — input, audio, text

SDL3 shim on scePad (controller → mouse/keys mapping like NX), OpenAL shim on AudioOut, IME dialog for login/chat.

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
3. **Prompt.** "Update available — ✕ Download / ○ Skip", auto-continuing with the default after a few seconds.
   First launch with no client: download is required.
4. **Download** over HTTPS with certificate verification (console `sceHttp`, proven in sandboxed titles, or
   libcurl), resumable with range requests, progress bar.
5. **Extract only what the PS5 needs** (`bin/linux/x64/PokeMMO`, `data/`, `config/`, `revision.txt`; ~320 MB of
   Windows/macOS binaries skipped), checking each entry's CRC.
6. **Verify, then switch.** x86-64 ELF check, then the loader confirms it can resolve every import. Two slots
   (A/B) in `/download0`; "current" moves only after verification. A version that fails to load falls back to the
   previous one, with the reason on screen.

Open: whether the client's own updater (data feeds) runs on Linux and touches its binary; the loader must
intercept any attempt to replace or re-exec itself.

## Phase 6 — packaging

Release zip = title folder + installer (`installer/`), icon/backgrounds, ROM instructions.

### Install layout

| What | On the console (FTP) | Seen by the title as | Written by |
|------|----------------------|----------------------|------------|
| Title | `/data/homebrew/PPSA27166/` | `/app0/` (read-only) | installer |
| ROMs | `/data/homebrew/PPSA27166/roms/` | `/app0/roms/` (read-only) | installer |
| Client slots, caches, config, logs | inside the title storage image | `/download0/` | the title |

`/download0` is a storage image, not a folder FTP can browse, so logs go out over UDP. Probe check `fs.app0roms` confirms the ROM
path on real hardware. `downloadDataSize` must cover two client slots + caches (~1.5–2 GB).

## Open risks

1. ps5-opengl is validated on 6.02 only; SDK 1.0.1 is CI-built, not console-validated.
2. Anonymous mmap draws on flexible memory, which ps5-opengl notes holds only a few hundred MiB. The 640 MiB Java heap will
   likely need direct memory behind the mmap shim (probe: vm.commit vs vm.direct). The app heap caps native `malloc` at
   128 MiB by default; native allocations (Mesa, SDL shim, client natives) must fit, so the title may need a bigger one.
3. Title sandbox: writable storage is `/download0` only (size set in `param.json`) and it is not reachable over FTP;
   ROMs go next to the title instead (see the install layout above; to be confirmed by `fs.app0roms`).
4. Firmware updates can break the jailbreak chain independently of this project.

## Hardware status

| Firmware | Console setup | Probe result | Date |
|----------|---------------|--------------|------|
| 12.40 | kstuff-lite | not run yet | — |
