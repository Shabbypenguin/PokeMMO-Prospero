// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// loader-36: backup to the player's own Google Drive (cloud.c), so that ROMs and every profile's settings survive installing the
// title again (which empties its storage). One Google account per console, signed in with Google's "TV and limited input" flow:
// the screen shows a short code, the player approves on a phone. The title only ever sees files it made itself (scope
// drive.file), in a folder "PokeMMO Prospero" in that Drive:
//   PokeMMO Prospero/roms/<file>                 the ROMs, as uploaded (never deleted from Drive by the title)
//   PokeMMO Prospero/profile-<profile id>.tar    one console profile's settings folder (with PokeMMO's remembered login)
// The refresh token is kept in <state folder>/google-token (the title storage): installing the title again means signing in again.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CLOUD_IDLE, CLOUD_CODE, CLOUD_BUSY, CLOUD_DONE, CLOUD_FAILED };
typedef struct {
    _Atomic int phase;          // what the screen shows
    char user_code[32];         // CLOUD_CODE: the code to enter
    char verify_url[128];       // ... at this address
    _Atomic uint64_t done, total;  // bytes of the transfer in progress
    char activity[300];         // "Backing up Pokemon Black.nds", "Restoring settings"
    char error[200];            // CLOUD_FAILED: why
} CloudStatus;

void cloudInit(const char *state_folder);
CloudStatus *cloudStatus(void);
bool cloudSignedIn(void);
bool cloudDeclined(void);  // the player chose not to use it (asked again after the title is installed again)
void cloudDecline(void);
void cloudSignOut(void);

// Shows a code (CLOUD_CODE) and waits for the player's approval. Blocks; `cancel` ends the wait.
bool cloudSignIn(_Atomic bool *cancel);
// ROMs: the files Drive has and the folder lacks are downloaded (`download`), the folder's ROMs Drive lacks are uploaded
// (`upload`). Same name and size counts as the same file.
bool cloudSyncRoms(const char *rom_folder, bool upload, bool download);
// One profile's settings folder, uploaded when it changed since the last upload from this console.
bool cloudBackupProfile(const char *profile, const char *config_folder);
// Every profile in Drive whose settings folder (users_folder/<id>/config) is empty or missing here is unpacked there.
// `restored` counts them.
bool cloudRestoreProfiles(const char *users_folder, unsigned *restored);
