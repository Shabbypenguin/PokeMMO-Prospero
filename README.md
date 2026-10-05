# pokemmo-ps5

Run the official, unmodified PokeMMO Linux x64 client natively on a jailbroken PS5.

Same approach as [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) on the Switch: a C loader maps the client's
ELF and provides the Linux libc, SDL3, OpenAL and GL it expects, here on top of the PS5's FreeBSD-derived OS, with
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) (Mesa, OpenGL 4.6 incl. compatibility contexts) for graphics.
The client is never redistributed: the build fetches it from PokeMMO, you supply your own ROMs.

**Status:** Phase 1 (client analysis) done, toolchain image done, hardware probe ready to run. See [docs/plan.md](docs/plan.md).

## Console requirements

The title format is the one [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
produces and ps5-opengl validated:

- kstuff-lite (fake-signed FSELF support) on a jailbroken console
- ShadowMountPlus (mounts/registers folder titles from `/data/homebrew`)
- an FTP server (`ftpsrv`, port 2121) for deployment

The boilerplate is hardware-verified on firmware 6.02 and 12.70; 12.40 is untested until the probe runs.

## Build host

Any x86-64 Linux box with Docker (the homelab server is fine).

```bash
scripts/ps5env build                                # once: build the pinned toolchain image (~10 min)
scripts/ps5env make probe PROBE_LOG_HOST=192.168.68.50   # your PC's IP, optional
scripts/ps5env make deploy-probe PS5_HOST=<console-ip>   # or unzip dist/*.zip into /data/homebrew/
```

`scripts/ps5env` runs `sudo docker` by default; set `DOCKER=docker` if your user is in the docker group.

## Running the probe

1. On your PC: `python3 tools/udplog.py --out probe.log`
2. Launch **PokeMMO PS5 Probe** from the home screen.
3. Read the log on your PC. It is also written to `/download0/probe.log` inside the title sandbox
   (`/user/download/PPSA27165/` over FTP).

The screen shows one tile per check (green pass, red fail, blue info, grey not run), left to right, top to bottom:

| Row | Tiles |
|-----|-------|
| 1 | gl.context, gl.compat, gl.glsl110, gl.glsl120, gl.glsl130, gl.clientarr, gl.vbo-novao |
| 2 | gl.blend, gl.immediate, tls.fs28, thread.stack, thread.getattr, vm.reserve, vm.fixed |
| 3 | vm.commit, vm.direct, vm.directfixed, fs.download0, net.dns, net.tcp, exec.rwx |
| 4 | exec.mprotect, exec.jit |

The exec checks run last; if the title closes, the log's last `BEGIN` line names the culprit.
Send me the full log either way.

## Layout

| Path | What |
|------|------|
| `docker/Dockerfile` | toolchain image: payload SDK v0.42, boilerplate @4f531c4b, ps5-opengl SDK 1.0.1 (all pinned) |
| `scripts/ps5env` | build the image / run commands in it with the repo at `/work` |
| `scripts/build-title.sh` | assemble a native title that links ps5-opengl |
| `probe/` | hardware probe title |
| `tools/analyze_client.py` | check a new PokeMMO release against what the loader supports |
| `tools/client-baseline.txt` | imports and runtime libraries of the last analyzed revision (32920) |
| `tools/udplog.py` | UDP log receiver |

## License

GPL-3.0-or-later (it links ps5-opengl and uses the GPL boilerplate). Code adapted from PokeMMO-NX keeps its MIT notice.
PokeMMO is © the PokeMMO team; this project is unofficial and ships none of their files.
