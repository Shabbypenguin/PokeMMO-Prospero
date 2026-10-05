# Credits

This project stands on other people's work. This file says exactly what is used from whom, and is kept current:
when code from another project enters this repository, the entry below is updated in the same commit.

## PokeMMO-NX — the Switch port this project is modeled on

- Project: <https://github.com/Petit-Prince-dev/PokeMMO-NX> by **Petit_Prince**, MIT license
  (full text: [LICENSES/PokeMMO-NX-MIT.txt](LICENSES/PokeMMO-NX-MIT.txt)).
- Reference revision: `5c03eb0cd213013d37a5e092b5cbcb6bf17fa8c1`.

PokeMMO-NX proved the approach this port uses: load the **official, unmodified** PokeMMO Linux client with a small
C loader that supplies the Linux libc, SDL3, OpenAL, EGL/GL and GTK it expects, and never redistribute the client.
Without it this port would not have been attempted.

What this repository takes from it today:

| Taken | Where it lives here | Form |
|-------|---------------------|------|
| The overall design (ELF loader + libc shim + virtual libraries) | [docs/plan.md](docs/plan.md) | design, no code |
| Which runtime libraries the client opens and must be provided virtually | [docs/plan.md](docs/plan.md), `tools/client-baseline.txt` | knowledge, re-derived from the client with `tools/analyze_client.py` |
| The client's OpenGL context request (desktop GL 2.1, no profile mask) | `probe/probe.c` (`glOpen`) | behavior reproduced, code written from scratch |
| The libffi closure interception idea (LWJGL callbacks need executable trampolines) | [docs/plan.md](docs/plan.md), `probe/probe.c` (exec checks) | design, no code |
| Downloading the client at build time instead of shipping it | `tools/fetch_client.py` | adapted from `tools/fetch_client.py` |
| JVM launch options `-XX:MaxHeapSize=640m -XX:MaxNewSize=128m` | [docs/plan.md](docs/plan.md) (Phase 2) | values |

What will be taken in Phase 2 (planned, not yet in this repository): the portable parts of PokeMMO-NX's loader — the
ELF loader (`elf_*.c`), the Linux ABI/libc layer (`linux_abi.c`, `linux_stdio.c`, `linux_files.c`, …) and the SDL3,
OpenAL and GTK shims — adapted from Horizon/ARM64 to the PS5/x86-64. Every adapted file will keep PokeMMO-NX's
copyright notice and say so in its header, as the MIT license requires.

## PS5 platform projects

| Project | Author | License | Used for |
|---------|--------|---------|----------|
| [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) | BlackBearReloaded | GPL-3.0-or-later | OpenGL 4.6 / EGL (Mesa) for the titles; its native-app glue (`app_heap.c`, `runtime_shims.c`, linker script, AGC link stubs, `param.json` base) is copied in at build time by `scripts/build-title.sh` |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | BlackBearReloaded | GPL-3.0-or-later | title build: FSELF writer, clean-room `libc.prx`, packaging, FTP deploy |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | John Törnblom and contributors | GPL-3.0 (see upstream) | compiler target, headers and system-library stubs |
| [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) | EchoStretch | no license file upstream | console side: runs fake-signed titles (not part of this repository) |

The exact pinned versions live in the build environment repository (`pokemmo-ps5-buildenv`, `docker/Dockerfile`).

## Considered, not used

- [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) (mpereiraesaa) — evaluated as a graphics backend; ps5-opengl
  fit the client's OpenGL directly, so it is not used.

## PokeMMO

PokeMMO is © the PokeMMO team. This project is unofficial, is not affiliated with or endorsed by them, and contains
none of their files. You download the client from PokeMMO yourself and supply ROMs you own.
