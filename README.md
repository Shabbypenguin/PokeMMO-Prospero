# PokeMMO-Prospero

An unofficial effort to run the official, unmodified PokeMMO Linux client natively on a jailbroken PS5.

> **Status: pre-beta, playable on a console.** Login, the overworld and battles work, with graphics, audio, the
> controller, an on-screen keyboard and the touchpad as a mouse. The title downloads and updates the PokeMMO client
> itself and takes ROMs over its own web page or FTP server. Each PS5 profile has its own PokeMMO login. An optional
> [Google Drive backup](#google-drive-backup) keeps ROMs and logins across reinstalls (waiting on Google's review of the
> app before it can be used). Tested on firmware 12.40 with kstuff-lite and ShadowMountPlus. Progress and open questions: [docs/plan.md](docs/plan.md), [docs/loader.md](docs/loader.md).

This project is not affiliated with or endorsed by the PokeMMO team or Sony. The PokeMMO client is **never included**:
the console downloads it from PokeMMO. You supply ROMs you dumped from cartridges you own.

## Getting started

### What you need

| What | Notes |
|------|-------|
| A jailbroken PS5 | Tested: firmware 12.40, kstuff-lite. |
| A way to install homebrew | **ShadowMountPlus**, or a homebrew store that installs `.ffpfsc` images. |
| Internet on the PS5 | The first start downloads the PokeMMO client (about 93 MB); later starts check for updates. |
| A PokeMMO account | Register from the title's login screen (it opens the PS5 web browser) or at pokemmo.com. |
| Your ROMs | See [ROMs](#roms). |

### 1. Install the title

Install `PPSA98001.ffpfsc` with ShadowMountPlus or your homebrew store. Everything the title keeps (client, ROMs,
settings, log) lives in its own storage on the console.

It shows up as **PokeMMO Prospero** (`PPSA98001`; earlier test builds were named "PokeMMO Prospero (dev)").

### 2. First start

1. Start the title. A loading screen with a progress bar appears.
2. On a fresh install it asks **"Restore your ROMs and settings from Google Drive?"** Press **Circle** if you've never
   used the backup; press **Cross** to sign in and get everything back (see [Google Drive backup](#google-drive-backup)).
3. It downloads the PokeMMO client. Keep the title open until the bar finishes.
4. Without Pokémon Black or White in the ROM folder, the **ROM screen** stays up and tells you how to upload them
   (next step). Once Black or White is there, the game starts.
5. Log in. The title sets the game up for a TV the first time: fullscreen 1920×1080, 60 FPS, interface scale 1.25 and
   the controller layout below. Change anything you like in the game's settings; your changes are kept.

### 3. Upload your ROMs

The ROM screen lists what was found, what is missing and what is not usable, and shows how to upload. Pick any one way:

| Way | How |
|-----|-----|
| **Web page** (easiest) | On a phone or computer on the same network, scan the QR code or open `http://<console IP>:8080` and pick the files. |
| **FTP** | Connect an FTP app to the address and port shown on screen (2121, or up to 2125 if 2121 is taken). It opens straight in the ROM folder. |

The screen shows each file arriving and checks the folder again afterwards; press **Cross** to check again yourself.
The upload page and FTP server only run while the ROM screen is up, and stop when the game starts.

**Adding games later.** When optional games are missing, the loading screen says how many were found and counts down
5 seconds just before the game starts: press **Square** then to open the ROM screen, upload, and press **Cross** to start
the game. Without a press the game starts as usual.

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
| Square (during the 5-second "games found" countdown) | ROM screen, to add optional games; Cross then starts the game |
| Cross / Circle | Answer the update question (download / skip); it downloads by itself after a countdown |
| Cross / Circle | Fresh install: answer "Restore from Google Drive?" (sign in / not now) |

### ROM screen

| Button | Does |
|--------|------|
| Cross | Check the folder again (Black/White missing) or start the game (adding optional games) |
| Square | Sign in to Google Drive for the backup (only shown while not signed in) |
| Circle | On the Google sign-in screen: cancel |

### Other

- **Links** in the game (register, forgot password) open in the PS5 web browser. The PS button brings the game back.
- **Exit** in the game's menu closes the title and goes back to the home screen.
- USB/Bluetooth keyboards and mice are not supported yet.

## Your data

| Install | Client, settings | ROMs | Log |
|---------|------------------|------|-----|
| Image | the title's storage | the title's storage | `http://<console IP>:8080/log` while the upload page runs; `/log-previous` is the start before |
| Folder (developers) | the title's storage | `/data/homebrew/PPSA98001/roms` | `/data/homebrew/PPSA98001/prospero.log` (FTP); `prospero-previous.log` is the start before |

**Each PS5 profile has its own PokeMMO login and settings** (the loading screen says "Playing as <profile>"); the client
and the ROMs are shared by everyone. To switch player, close the title, switch PS5 profile and start it again.

The title's storage survives reboots, but **installing the title again (including a launcher update) erases it**: the
client is downloaded again, ROMs must be uploaded again and every profile's settings go back to the defaults, unless you
use the [Google Drive backup](#google-drive-backup). Your PokeMMO account and characters are on PokeMMO's servers and are
not affected.

Client updates are installed next to the working client and only replace it once the new one has shown a picture;
if an update fails to start, the title goes back to the previous client by itself.

### Google Drive backup

Because installing the title again erases its storage, the title can keep a copy of your ROMs and every profile's
settings in **your own** Google Drive, and put them back after a reinstall or a launcher update. It's optional.

> **Not usable yet:** Google is reviewing the app. Until it's approved, signing in fails with an access error. This
> section describes how it works once it's live.

**Turning it on**

1. On the ROM screen, press **Square** ("Back up your ROMs and settings to Google Drive"). On a fresh install you can also
   answer yes to "Restore your ROMs and settings from Google Drive?".
2. The TV shows an address and a code. On your phone or computer, open `google.com/device` (or scan the QR code), sign
   in to Google and enter the code.
3. Allow PokeMMO Prospero to use its own files in your Drive. The TV notices by itself and carries on.

Sign in once per console: one Google account holds the backup for every PS5 profile on it.

**What it keeps, and when**

| What | When it goes up |
|------|-----------------|
| Your ROMs (`.nds`, `.gba`), each once | A few seconds after uploads to the ROM screen stop, and right after you sign in |
| The playing profile's PokeMMO settings, including the remembered login | Every minute while they change, and when you choose Exit in the game |

Everything goes into a folder called **PokeMMO Prospero** in that Drive: `roms/` and one `profile-<number>.tar` per
PS5 profile. It takes about as much Drive space as your ROMs (a few hundred MB for all five games). The client isn't
backed up: it downloads again from PokeMMO by itself.

**Restoring**

After installing the title again, the first start asks "Restore your ROMs and settings from Google Drive?". Press
**Cross** and sign in: the ROMs and every profile's settings come back before the game starts, with a progress bar.
Settings a profile already has on the console are never overwritten.

**Privacy and removal**

- The title uses Google's most limited Drive permission: it only sees files it created. It can't see the rest of your
  Drive. The data goes only between your PS5 and your Drive; the project has no servers and collects nothing.
- Anyone with access to that Google account can read the backed-up settings and remembered logins of every profile
  on the console.
- To remove it: delete the **PokeMMO Prospero** folder in Drive, and remove the app's access at
  [myaccount.google.com/permissions](https://myaccount.google.com/permissions). Deleting the title removes the sign-in
  from the console.
- Full details: [privacy page](https://shabbypenguin.github.io/PokeMMO-Prospero/privacy.html).

## Troubleshooting

| Problem | Try |
|---------|-----|
| Red bar: "PokeMMO is not installed yet" | Check the console's internet connection, close the title (PS button) and start it again. |
| ROM screen keeps coming back | Black or White isn't there or isn't readable. The screen says what each file is; unpack zipped files first. |
| Web page or FTP won't connect | Use the address shown on screen, from the same network. Uploads (and the FTP server) only work before the game starts. |
| Google sign-in says access is blocked, or "not a tester" | The app is still waiting for Google's review; the backup can't be used until it's approved. |
| "Google hasn't verified this app" on the phone | Tap **Advanced**, then **Go to PokeMMO Prospero**. It's only the app's own folder in your Drive. |
| ROM screen says "Google Drive: … (tried again later)" | The last backup failed (network, or Drive full). It tries again by itself; the reason is in the log on lines starting `cloud:`. |
| The code on the TV expired | Press Circle, then Square on the ROM screen for a new code. Codes last about 15 minutes. |
| "PokeMMO stopped while starting" or the game crashed | Start the title again. If it keeps happening, hold Triangle (it starts the upload page) and save `http://<console IP>:8080/log-previous` (the crashed start's log), then open an issue with it. |

## For developers

Builds use the pinned toolchain in the **pokemmo-ps5-buildenv** repository; start with its README.

```bash
git clone https://github.com/Shabbypenguin/pokemmo-ps5-buildenv.git
git clone https://github.com/Shabbypenguin/PokeMMO-Prospero.git   # side by side
cd PokeMMO-Prospero
../pokemmo-ps5-buildenv/ps5env build                  # once: build the toolchain image
../pokemmo-ps5-buildenv/ps5env make loader            # folder title (dist/pokemmo-prospero-loader-PPSA98001.zip)
../pokemmo-ps5-buildenv/ps5env make image-loader      # .ffpfsc image
```

A folder install (developers): unzip the folder title into `/data/homebrew/` on the console with an FTP payload and
let your title mounter pick it up. Its ROMs go to `/data/homebrew/PPSA98001/roms/`.

Google Drive backup: the project's OAuth client ("TVs and Limited Input devices", scope `drive.file`) is in
`loader/src/cloud.c`. `make cloud-test` runs a full backup and restore against `tools/cloud_server.py`, a stand-in for
Google's sign-in and Drive endpoints, on a PC.

Releases: set the release number in `VERSION` (it's what the loading screen shows), push, then Actions → **release** →
Run workflow with the same number, e.g. `v0.1.1-beta`. It builds in CI and publishes a
GitHub Release with `PPSA98001.ffpfsc` and its SHA-256 sum. Pushing a `v*` tag does the same.

How it works: this ports the approach [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) by Petit_Prince
uses on the Nintendo Switch. A small C loader maps PokeMMO's own Linux executable (a GraalVM native image) and
supplies the Linux libc, SDL3, OpenAL, EGL/GL and GTK pieces it expects, on top of the PS5's FreeBSD-derived OS, with
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) (Mesa) providing OpenGL. Details:
[docs/loader.md](docs/loader.md); what comes from where: [CREDITS.md](CREDITS.md).

| Path | What |
|------|------|
| `loader/` | the loader: Linux runtime, SDL/GL/audio adapters, loading screen, updater, ROM check, upload servers |
| `assets/` | branding and the default game settings |
| `scripts/build-title.sh` | assembles a native PS5 title (FSELF + `libc.prx` + `sce_sys`) that links ps5-opengl |
| `tools/` | client download and analysis, branding generator, UDP log receiver, test servers |
| `site/` | the project's web page and privacy page ([GitHub Pages](https://shabbypenguin.github.io/PokeMMO-Prospero/)) |
| `docs/` | [plan](docs/plan.md), [loader](docs/loader.md), [to do](docs/todo.md); the retired hardware probe is at branch [`probe-final`](https://github.com/Shabbypenguin/PokeMMO-Prospero/tree/probe-final) |

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
