# PokeMMO-Prospero

An unofficial effort to run the official, unmodified PokeMMO Linux client natively on a jailbroken PS5.

> **Status: early. PokeMMO does not run on the PS5 yet.** The loader (ported from PokeMMO-NX) runs the client on a
> Linux PC up to its graphics setup; its first PS5 build (milestone 1) is waiting for a console run. Graphics, input
> and audio for the client come next. Progress and open questions: [docs/plan.md](docs/plan.md), [docs/loader.md](docs/loader.md).

## How it works

This is a port of the approach [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) by Petit_Prince uses on
the Nintendo Switch. A small C loader maps PokeMMO's own Linux executable (a GraalVM native image) and supplies the
Linux libc, SDL3, OpenAL, EGL/GL and GTK pieces it expects. Here those sit on the PS5's FreeBSD-derived OS, with
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) (Mesa) providing OpenGL. See [CREDITS.md](CREDITS.md) for
exactly what comes from where.

The PokeMMO client is **never included** in this repository or its builds: you download it from PokeMMO
(`make fetch-client`) and supply ROMs you own. This project is not affiliated with or endorsed by the PokeMMO team.

## Repositories

| Repository | Contents |
|------------|----------|
| **PokeMMO-Prospero** (this one) | the port: probe, loader (coming), title build script, client analysis tools, docs |
| **pokemmo-ps5-buildenv** | the pinned toolchain image and the guide for building, setting up a console and maintaining the project |

Start with the buildenv repository's README; it builds this one.

## Quick start

```bash
git clone https://github.com/Shabbypenguin/pokemmo-ps5-buildenv.git
git clone https://github.com/Shabbypenguin/PokeMMO-Prospero.git   # side by side
cd PokeMMO-Prospero
../pokemmo-ps5-buildenv/ps5env build                # once: build the toolchain image
../pokemmo-ps5-buildenv/ps5env make probe           # build the hardware probe title
```

Then follow [docs/probe.md](docs/probe.md). Players get a single zip with the title and the installer
(`make package-probe`; the game title will ship the same way).

## Layout

| Path | What |
|------|------|
| `probe/` | hardware probe title: checks the platform behaviors the loader depends on |
| `installer/` | Windows/macOS/Linux installer: uploads a title over FTP and optionally your ROMs ([README](installer/README.md)) |
| `scripts/build-title.sh` | assembles a native PS5 title (FSELF + `libc.prx` + `sce_sys`) that links ps5-opengl |
| `tools/fetch_client.py` | downloads the PokeMMO client into `private/` (ignored by git) |
| `tools/analyze_client.py` | checks a client release against what the loader supports; run on every PokeMMO update |
| `tools/client-baseline.txt` | imports and runtime libraries of the last supported client revision (32920) |
| `tools/udplog.py` | receives the UDP log from the probe (and later the loader) |
| `docs/` | [plan](docs/plan.md), [probe](docs/probe.md) |

## Transparency

- Parts of this project were written with AI assistance (Anthropic's Claude) and reviewed by the maintainer. Commits
  with AI involvement carry a `Co-Authored-By` trailer.
- Nothing here counts as working on hardware until a log or result in the docs says so. Tested firmware and console
  setups are recorded in [docs/plan.md](docs/plan.md#hardware-status) as they are confirmed.
- No PokeMMO, Sony or Nintendo files are, or will be, committed here.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)); required because the titles link ps5-opengl and are built with the GPL
native-app boilerplate. Code adapted from PokeMMO-NX keeps its MIT copyright notice
([LICENSES/PokeMMO-NX-MIT.txt](LICENSES/PokeMMO-NX-MIT.txt)).
