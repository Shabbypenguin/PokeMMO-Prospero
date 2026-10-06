# Credits

This project stands on other people's work. This file says exactly what is used from whom, and is kept current:
when code from another project enters this repository, the entry below is updated in the same commit.

## PokeMMO-NX — the Switch port this project is modeled on

- Project: <https://github.com/Petit-Prince-dev/PokeMMO-NX> by **Petit_Prince**, MIT license
  (full text: [LICENSES/PokeMMO-NX-MIT.txt](LICENSES/PokeMMO-NX-MIT.txt)).
- Reference revisions: `5c03eb0` (1.0.0) for the loader design, `cac8d83` (1.1.2) for the input design.

PokeMMO-NX proved the approach this port uses: load the **official, unmodified** PokeMMO Linux client with a small
C loader that supplies the Linux libc, SDL3, OpenAL, EGL/GL and GTK it expects, and never redistribute the client.
Without it this port would not have been attempted.

What this repository takes from it today:

| Taken | Where it lives here | Form |
|-------|---------------------|------|
| The overall design (ELF loader + libc shim + virtual libraries) | [docs/plan.md](docs/plan.md), [docs/loader.md](docs/loader.md) | design and code (below) |
| Which runtime libraries the client opens and must be provided virtually | [docs/plan.md](docs/plan.md), `tools/client-baseline.txt` | knowledge, re-derived from the client with `tools/analyze_client.py` |
| The client's OpenGL context request (desktop GL 2.1, no profile mask) | `probe/probe.c` (`glOpen`) | behavior reproduced, code written from scratch |
| The libffi closure interception idea (LWJGL callbacks need executable trampolines) | [docs/plan.md](docs/plan.md), `probe/probe.c` (exec checks) | design, no code |
| Downloading the client at build time instead of shipping it | `tools/fetch_client.py` | adapted from `tools/fetch_client.py` |
| JVM launch options `-XX:MaxHeapSize=640m -XX:MaxNewSize=128m` | [docs/plan.md](docs/plan.md) (Phase 2) | values |
| Input design: controller as an SDL3 gamepad with face buttons by printed label, a short gamepad name (long ones make the client's Android theme lag), R3 for the keyboard, L3 for a stick cursor | [docs/plan.md](docs/plan.md) (Phase 4) | design, no code |

Since milestone 1 of the loader, PokeMMO-NX's code is in this repository too (`loader/`). Each of these files
keeps PokeMMO-NX's copyright notice in its header and lists what changed (details in [docs/loader.md](docs/loader.md)):

| File(s) here | From PokeMMO-NX | Changes |
|--------------|-----------------|---------|
| `loader/src/elf_layout.c`, `elf_versions.c`, `elf_image.c` and headers | same names | x86-64 relocations, in-place staging |
| `loader/src/elf_executable.c` | `elf_executable.c` | mapping through the platform layer |
| `loader/src/linux_abi.c`, `linux_runtime.c`, `linux_files.c`, `linux_stdio.c`, `linux_format.c`, `linux_libc_extra.c`, `linux_process.c` | same names | x86-64 layouts and numbers, platform calls, mounts, `getdents` listing |
| `loader/src/linux_net.c`, `linux_net_translate.c`, `linux_net_native.c`, `linux_vfd.c`, `linux_zlib.c` | same names | BSD sockets of the platform, packed `epoll_event` |
| `loader/src/linux_dl.c`, `linux_trap.c`, `linux_virtual_stubs.c`, `linux_jit.c`, `linux_threads.c`, `linux_vm.c`, `diagnostics.c`, `game.c` | same names | see each header |
| `loader/src/linux_stubs_x86_64.c` | `linux_trap_stubs.s`, `linux_virtual_stub_table.s` | rewritten for x86-64 |
| `loader/src/linux_sync.c`, `linux_semaphore.c`, `linux_tls.c` | interfaces only | implementations written for this port |
| `loader/src/linux_sdl.c`, `linux_sdl_events.c`, `linux_sdl_keys.c`, `linux_sdl_io.c`, `linux_sdl_cursor.c`, `linux_audio.c`, `linux_al.c`, `linux_gtk.c`, `linux_file_picker.c` | same names | ps5-opengl surface, platform calls |
| `loader/src/linux_sdl_input.c`, `linux_audio_out.c` | same names | PS5 controller; the keyboard is the loader's own (`osk.c`); audio through `sceAudioOut` |

## PS5 platform projects

| Project | Author | License | Used for |
|---------|--------|---------|----------|
| [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) | BlackBearReloaded | GPL-3.0-or-later | OpenGL 4.6 / EGL (Mesa) for the titles; its native-app glue (`app_heap.c`, `runtime_shims.c`, linker script, AGC link stubs, `param.json` base) is copied in at build time by `scripts/build-title.sh` |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | BlackBearReloaded | GPL-3.0-or-later | title build: FSELF writer, clean-room `libc.prx`, packaging, FTP deploy; the installer's upload order (temporary name, rename, `eboot.bin`/`param.json` last) follows its `tools/deploy.sh` |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | John Törnblom and contributors | GPL-3.0 (see upstream) | compiler target, headers and system-library stubs |
| [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) | EchoStretch | no license file upstream | console side: runs fake-signed titles (not part of this repository) |

The exact pinned versions live in the build environment repository (`pokemmo-ps5-buildenv`, `docker/Dockerfile`).

## Libraries

| Library | Author | License | Used for |
|---------|--------|---------|----------|
| [QR Code generator](https://github.com/nayuki/QR-Code-generator) (C version, `loader/src/qrcodegen.c`, `loader/include/qrcodegen.h`, commit `3c6d0b3`) | Project Nayuki | MIT (in the files) | the link box's QR codes |

## Fonts

- The file chooser's bitmap font (`loader/src/picker_font.c`) is rendered from [DejaVu Sans Mono](https://dejavu-fonts.github.io/)
  by `tools/make_picker_font.py` (DejaVu fonts license, derived from Bitstream Vera: free to embed and redistribute).
- The overlay font (loading screen, keyboard, link box; `loader/src/overlay_assets.c`) is rendered from
  [DejaVu Sans](https://dejavu-fonts.github.io/) by `tools/make_branding.py` (same license).

## Artwork

- The PokeMMO-Prospero logo (`assets/branding/logo-source.png`) and the icon, home screen background and loading screen
  logo made from it by `tools/make_branding.py` are this project's own (Shabby). They replace the ps5-native-app-boilerplate's
  sample artwork and sound in the title.

## Considered, not used

- [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) (mpereiraesaa) — evaluated as a graphics backend; ps5-opengl
  fit the client's OpenGL directly, so it is not used.

## PokeMMO

PokeMMO is © the PokeMMO team. This project is unofficial, is not affiliated with or endorsed by them, and contains
none of their files. You download the client from PokeMMO yourself and supply ROMs you own.
