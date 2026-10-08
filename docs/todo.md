# To do

Planned, not started. Newest first.

## Launcher update check

Players can keep running an old `.ffpfsc` for a long time without knowing a newer one exists.

- At start (while the client update check runs), ask GitHub for the newest release:
  `GET https://api.github.com/repos/Shabbypenguin/PokeMMO-Prospero/releases/latest` and compare its `tag_name` with the
  title's own `v` + `PROSPERO_RELEASE` (the `VERSION` file).
- Newer one out: a note on the loading screen ("PokeMMO Prospero v0.2.0-beta is out: install it with your homebrew
  store"), shown for a few seconds like the ROM countdown, never blocking the game. Maybe a QR code to the release page.
- Pre-releases: `releases/latest` skips them, so decide whether betas compare against `/releases` (first entry) instead.
- Cheap and quiet on failure: one request with a short timeout, no retries; GitHub's unauthenticated limit
  (60 requests an hour per IP) is plenty for one check per start.
- Remember that a reinstall erases the title's storage (and the Drive backup restores ROMs and settings), so the note
  can say updating is safe once the backup is on.

## Other open items

- Google Drive backup: in beta testing (the app is published); finish Google's branding check (the home page domain) and test sign-in,
  backup and restore on a console.
- USB/Bluetooth keyboard and mouse: a test build that opens `sceKeyboard`/the mouse library and logs what it gets,
  then route them into the game.
