# PokeMMO-Prospero

An unofficial effort to run the official, unmodified PokeMMO Linux client natively on a jailbroken PS5.

> **Status: pre-beta, playable on a console.** Login, the overworld and battles work, with graphics, audio, the
> controller, an on-screen keyboard and the touchpad as a mouse. The title downloads and updates the PokeMMO client
> itself and takes ROMs over its own web page or FTP server. Tested on firmware 12.40 with kstuff-lite and
> ShadowMountPlus. Progress and open questions: [docs/plan.md](docs/plan.md), [docs/loader.md](docs/loader.md).

This project is not affiliated with or endorsed by the PokeMMO team or Sony. The PokeMMO client is **never included**:
the console downloads it from PokeMMO. You supply ROMs you dumped from cartridges you own.

## Getting started

### What you need

| What | Notes |
|------|-------|
| A jailbroken PS5 | Tested: firmware 12.40, kstuff-lite. |
| A way to install homebrew | **ShadowMountPlus** (or a homebrew store that installs `.ffpfsc` images) for the image; or a folder-title loader plus an FTP payload (e.g. ftpsrv) for the installer. |
| Internet on the PS5 | The first start downloads the PokeMMO client (about 93 MB); later starts check for updates. |
| A PokeMMO account | Register from the title's login screen (it opens the PS5 web browser) or at pokemmo.com. |
| Your ROMs | See [ROMs](#roms). |

### 1. Install the title

**Image (recommended).** Install `PPSA98001.ffpfsc` with ShadowMountPlus or your homebrew
store. Everything the title keeps (client, ROMs, settings, log) lives in its own storage on the console.

**Folder install (developers).** Unzip `pokemmo-prospero-loader-installer.zip` on your computer, start the FTP payload
on the PS5, and run `install.bat` (Windows), `install.command` (macOS) or `./install.sh` (Linux). It asks for the
console's IP address and FTP details and can upload your ROMs in the same go. See [installer/README.md](installer/README.md).

Either way the title shows up as **PokeMMO Prospero** (`PPSA98001`; earlier test builds were named "PokeMMO Prospero (dev)").

### 2. First start

1. Start the title. A loading screen with a progress bar appears.
2. It downloads the PokeMMO client. Keep the title open until the bar finishes.
3. Without Pokémon Black or White in the ROM folder, the **ROM screen** stays up and tells you how to upload them
   (next step). Once Black or White is there, the game starts.
4. Log in. The title sets the game up for a TV the first time: fullscreen 1920×1080, 60 FPS, interface scale 1.25 and
   the controller layout below. Change anything you like in the game's settings; your changes are kept.

### 3. Upload your ROMs

The ROM screen lists what was found, what is missing and what is not usable, and shows how to upload. Pick any one way:

| Way | How |
|-----|-----|
| **Web page** (easiest) | On a phone or computer on the same network, scan the QR code or open `http://<console IP>:8080` and pick the files. |
| **FTP** | Connect an FTP app to the address, port and folder shown on screen (port 2121, or up to 2125 if 2121 is taken; folder `/data/homebrew/PPSA98001/roms`). |
| **Installer** | Run the installer and answer yes to uploading ROMs. With an image install it talks to the title's own FTP server and only uploads ROMs. |

The screen shows each file arriving and checks the folder again afterwards; press **Cross** to check again yourself.
The upload page and FTP server only run while the ROM screen is up, and stop when the game starts. To add games
later, hold **Square** on the loading screen.

## ROMs

PokeMMO uses these games. Upload the unpacked `.nds` / `.gba` files (not `.zip` / `.7z`); file names don't matter, the
title reads each file's header.

| Game | File | Needed? |
|------|------|---------|
| Pokémon Black **or** White | `.nds` | **Required.** The game cannot start without it. |
| Pokémon FireRed | `.gba` | Optional: adds Kanto |
| Pokémon Emerald | `.gba` | Optional: adds Hoenn |
| Pokémon Platinum | `.nds` | Optional: adds Sinnoh |
| Pokémon HeartGold **or** SoulSilver | `.nds` | Optional: adds Johto |

Not used by PokeMMO (the ROM screen says so): Black 2 / White 2, LeafGreen, Ruby / Sapphire, Diamond / Pearl.
PokeMMO's own setup guide has the current list. This project doesn't provide or link to ROMs.

## Controls

### In the game (default layout)

| Button | Does |
|--------|------|
| D-pad or left stick | Move, menus |
| Cross / Circle | A (confirm, talk) / B (cancel) |
| Square | X |
| Triangle | Not bound (assign it in the game's controller settings if you like) |
| L1 / R1 | Previous / next |
| Options | Game menu |
| Touchpad press | Bag |
| R3 (click the right stick) | On-screen keyboard on/off; select the text field first |
| L3 (click the left stick) | Stick cursor on/off: the left stick moves a mouse cursor, R2 / L2 left / right click |

The layout is the game's own controller setting, so you can rebind it in the game.

### Touchpad as a mouse

| Gesture | Does |
|---------|------|
| Slide a finger | Moves the cursor, like a laptop trackpad (faster swipes go further) |
| Tap | Left click |
| Two-finger tap | Right click |
| Tap, then touch again and slide | Drag |
| Press down | Still the bag button; never a click |

The cursor hides a few seconds after you stop touching the pad.

### On-screen keyboard (R3)

| Button | Does |
|--------|------|
| D-pad / left stick, Cross | Move, type the highlighted key |
| Square / Triangle | Delete / space (hold to repeat) |
| L1 / R1 | Move the text cursor |
| L2 / R2 | Symbols / shift (once, caps lock, off) |
| Touchpad press / Options | Tab (next field) / Enter |
| L3 | Move the keyboard to the top or bottom of the screen |
| Circle | Close it |

### Loading screen

| Button | Does |
|--------|------|
| Hold Triangle | Startup details: each check's result and where to get the full log |
| Hold Square | ROM screen: what is uploaded and how to add more |
| Cross / Circle | Answer the update question (download / skip); it downloads by itself after a countdown |

### Other

- **Links** in the game (register, forgot password) open in the PS5 web browser. The PS button brings the game back.
- **Exit** in the game's menu closes the title and goes back to the home screen.
- USB/Bluetooth keyboards and mice are not supported yet.

## Your data

| Install | Client, settings | ROMs | Log |
|---------|------------------|------|-----|
| Image | the title's storage | the title's storage | `http://<console IP>:8080/log` while the upload page runs; `/log-previous` is the start before |
| Folder | the title's storage | `/data/homebrew/PPSA98001/roms` | `/data/homebrew/PPSA98001/prospero.log` (FTP); `prospero-previous.log` is the start before |

The title's storage survives reboots and title updates, but **deleting or reinstalling the title erases it**: the
client is downloaded again, ROMs must be uploaded again and the game's settings go back to the defaults. Your
PokeMMO account and characters are on PokeMMO's servers and are not affected. In an image install, save
`http://<console IP>:8080/settings` first if you want a copy of your settings.

Client updates are installed next to the working client and only replace it once the new one has shown a picture;
if an update fails to start, the title goes back to the previous client by itself.

## Troubleshooting

| Problem | Try |
|---------|-----|
| Red bar: "PokeMMO is not installed yet" | Check the console's internet connection, close the title (PS button) and start it again. |
| ROM screen keeps coming back | Black or White isn't there or isn't readable. The screen says what each file is; unpack zipped files first. |
| Web page or FTP won't connect | Use the address shown on screen, from the same network. Uploads (and the FTP server) only work before the game starts. |
| "PokeMMO stopped while starting" or the game crashed | Start the title again. If it keeps happening, hold Square and save `http://<console IP>:8080/log-previous` (the crashed start's log), then open an issue with it. |

## For developers

Builds use the pinned toolchain in the **pokemmo-ps5-buildenv** repository; start with its README.

```bash
git clone https://github.com/Shabbypenguin/pokemmo-ps5-buildenv.git
git clone https://github.com/Shabbypenguin/PokeMMO-Prospero.git   # side by side
cd PokeMMO-Prospero
../pokemmo-ps5-buildenv/ps5env build                  # once: build the toolchain image
../pokemmo-ps5-buildenv/ps5env make package-loader    # folder title + installer zip
../pokemmo-ps5-buildenv/ps5env make image-loader      # .ffpfsc image
```

Releases: Actions → **release** → Run workflow, with a version such as `v0.1.0-beta`. It builds in CI and publishes a
GitHub Release with `PPSA98001.ffpfsc`, the installer zip and their SHA-256 sums. Pushing a `v*` tag does the same.

How it works: this ports the approach [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) by Petit_Prince
uses on the Nintendo Switch. A small C loader maps PokeMMO's own Linux executable (a GraalVM native image) and
supplies the Linux libc, SDL3, OpenAL, EGL/GL and GTK pieces it expects, on top of the PS5's FreeBSD-derived OS, with
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) (Mesa) providing OpenGL. Details:
[docs/loader.md](docs/loader.md); what comes from where: [CREDITS.md](CREDITS.md).

| Path | What |
|------|------|
| `loader/` | the loader: Linux runtime, SDL/GL/audio adapters, loading screen, updater, ROM check, upload servers |
| `installer/` | Windows/macOS/Linux installer: uploads a title over FTP and optionally your ROMs ([README](installer/README.md)) |
| `assets/` | branding and the default game settings |
| `scripts/build-title.sh` | assembles a native PS5 title (FSELF + `libc.prx` + `sce_sys`) that links ps5-opengl |
| `tools/` | client download and analysis, branding generator, UDP log receiver, test servers |
| `docs/` | [plan](docs/plan.md), [loader](docs/loader.md); the retired hardware probe is at branch [`probe-final`](https://github.com/Shabbypenguin/PokeMMO-Prospero/tree/probe-final) |

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
