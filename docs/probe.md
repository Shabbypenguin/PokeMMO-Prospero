# Hardware probe

`probe/` builds a small native title (`PPSA27165`, "PokeMMO Prospero Probe") that checks, on a real console, every
platform behavior the loader design depends on. Run it once per firmware/toolchain change and attach the log to
any bug report.

## Build and run

From the build environment (see `pokemmo-ps5-buildenv`):

```bash
../pokemmo-ps5-buildenv/ps5env make probe PROBE_LOG_HOST=<your PC's LAN IP>   # PROBE_LOG_HOST is optional
../pokemmo-ps5-buildenv/ps5env make package-probe                               # zip with the installer
```

1. Install it: unzip `dist/pokemmo-prospero-probe-installer.zip` on your PC and run the installer for your OS
   (`install.bat`, `install.command`, `install.sh`; see [installer/README.md](../installer/README.md)). Say yes to
   the ROM question to also test the ROM path (`fs.app0roms`). From a checkout you can instead run
   `../pokemmo-ps5-buildenv/ps5env make deploy-probe PS5_HOST=<console IP>`.
2. On your PC: `python3 tools/udplog.py --out probe.log` (UDP port 18194).
3. Launch **PokeMMO Prospero Probe** from the home screen.
4. The log streams to your PC: broadcast, plus unicast to `PROBE_LOG_HOST` if set. The probe also writes it to
   `/download0/probe.log`, but that is inside the title's storage image and is **not** reachable over FTP, so the
   UDP log is the one to keep.

The automatic checks take about a minute. If the title closes, the last `BEGIN` line in the log names the check
that took it down, and **just launch it again**: the probe remembers which group crashed (in its `/download0`
storage), marks those checks FAIL and skips them, so each relaunch gets further. The memory resets with every new
probe version.

After `DONE`, the probe becomes a **controller tester**: press every button once (each lights a square along the
bottom of the screen and is logged), move both sticks, touch and click the touchpad, then press **Triangle** to open
the system keyboard and type a few letters. Close the title with the PS button when done. For the USB check, plug a
USB drive in before launching (ROMs at its top level are read too).

**Reinstalling:** ShadowMountPlus runs the title from its own copy, so after uploading a new probe, uninstall the
title from the home screen and wait for ShadowMountPlus to add it again. Check the first log line
(`PokeMMO-Prospero probe-N (build ...)`) to confirm the new build is the one running.

## Screen

One tile per check, eight per row, left to right, top to bottom. Green pass, red fail, blue informational, grey not
run.

| Row | Tiles |
|-----|-------|
| 1 | gl.context, gl.compat, gl.glsl110, gl.glsl120, gl.glsl130, gl.clientarr, gl.vbo-novao, gl.blend |
| 2 | gl.immediate, tls.fs28, thread.stack, thread.getattr, vm.reserve, vm.fixed, vm.commit, vm.direct |
| 3 | vm.directfixed, vm.vrange, vm.vrangedirect, fs.download0, fs.app0roms, fs.dataroms, fs.usb, net.dns |
| 4 | net.tcp, net.getaddrinfo, exec.rwx, exec.mprotect, exec.jit, input.pad, input.ime |

Below the tiles, one small square per controller button lights while that button is held.

## What each check decides

| Check | Question | If it fails |
|-------|----------|-------------|
| gl.context, gl.compat | Does ps5-opengl give the client's request (GL 2.1, no profile mask) a compatibility context? | graphics plan changes |
| gl.glsl110/120/130 | Do the client's shader dialects compile? | shader translation needed |
| gl.clientarr, gl.vbo-novao, gl.blend, gl.immediate | Legacy draw paths and alpha blending | report upstream to ps5-opengl |
| tls.fs28 | Is `%fs:0x28` (read by the client's 129 stack-canary checks) stable per thread? | rewrite those reads at load time |
| thread.stack, thread.getattr | Thread stack size control; main-thread stack bounds (GraalVM needs them) | different stack strategy |
| vm.reserve, vm.fixed | Large PROT_NONE reservations and fixed mappings inside them (GraalVM heap layout) | heap reservation strategy |
| vm.commit | How much anonymous (flexible) memory can be committed — informational | — |
| vm.direct, vm.directfixed | Direct memory budget, and mapping it at a fixed address inside an mmap reservation | Java heap won't fit |
| vm.vrange, vm.vrangedirect | The console's own reservation call (`sceKernelReserveVirtualRange`) for large ranges, with direct memory committed at fixed addresses inside and `mprotect` on it | heap layout needs another approach |
| fs.download0 | Writable title storage | storage plan changes |
| fs.app0roms | Do ROMs uploaded to `/data/homebrew/<ID>/roms/` show up, readable, at `/app0/roms`? (blue = none uploaded) | ROM location changes |
| fs.dataroms | Can a title read ROMs straight from `/data` (`/data/homebrew/PPSA27165/roms`, `/data/pokemmo-prospero/roms`)? Red = sandbox denies it | `.ffpfsc`/fpkg installs need another ROM route |
| fs.usb | Can a title see and read USB drives (`/mnt/usb0`–`7`)? Blue = none visible | in-app "import from USB" not possible |
| net.dns, net.tcp | Resolving and reaching PokeMMO's servers with the console resolver (`sceNetResolver`) | networking needs elevation |
| net.getaddrinfo | Does POSIX `getaddrinfo` work in a title? It comes from a WebKit-only module and crashed probe-2 | the loader resolves names through `sceNetResolver` |
| exec.rwx, exec.mprotect, exec.jit | Which executable-memory route works (libffi closures) | fall back to static trampolines |
| input.pad | Controller input in a title; the log maps every button bit, stick axis and touchpad report | input layer design |
| input.ime | Does the system keyboard (IME dialog) open from a title and return typed text? | text entry needs another route |
