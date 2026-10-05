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

## Phase 5 — packaging

Title with icon/backgrounds, ROM/setup instructions, a release script that downloads the client on the build host.

## Open risks

1. ps5-opengl is validated on 6.02 only; SDK 1.0.1 is CI-built, not console-validated.
2. Anonymous mmap draws on flexible memory, which ps5-opengl notes holds only a few hundred MiB. The 640 MiB Java heap will
   likely need direct memory behind the mmap shim (probe: vm.commit vs vm.direct). The app heap caps native `malloc` at
   128 MiB by default; native allocations (Mesa, SDL shim, client natives) must fit, so the title may need a bigger one.
3. Title sandbox: writable storage is `/download0` only (size set in `param.json`); ROMs are copied there over FTP.
4. Firmware updates can break the jailbreak chain independently of this project.
