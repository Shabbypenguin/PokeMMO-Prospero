// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/game.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the platform passes the folders and arguments; no Switch applet handling.
#pragma once
#include <stdbool.h>

typedef struct {
    const char *root;         // native folder the client sees as "/" (its install is /game, temporary files /tmp, home /home/ps5)
    const char *client_path;  // native path of bin/linux/x64/PokeMMO inside root (the loader maps the file)
    struct {
        const char *guest, *native;
    } mounts[4];              // optional extra folders, e.g. {"/game/roms", "/app0/roms"}
    unsigned mount_count;
    const char *const *arguments;  // the client's argv after the program name (JVM options), NULL-terminated
    unsigned timeout_seconds;      // 0: wait for ever
} GameConfig;

// Runs the official PokeMMO client (its `main`) inside the adapters and waits for it. True when main returned 0 or exit(0) was called.
bool gameRun(const GameConfig *config);
const char *gameFailure(void);  // a short sentence, valid after gameRun returned false
