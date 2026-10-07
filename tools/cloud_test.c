// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The Google Drive backup (loader/src/cloud.c) against tools/cloud_server.py, on a PC (make cloud-test): sign in, back up a ROM
// folder and a profile's settings, then sign in again as a fresh install would and restore both into empty folders.
// `cloud_test WORK_FOLDER`; PROSPERO_CLOUD_BASE must point at the server.
#include "cloud.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        printf("     cloud says: %s\n", cloudStatus()->error);
        ++failures;
    }
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char state[400], roms[400], config[400], state2[400], roms2[400], users2[400];
    snprintf(state, sizeof(state), "%s/state", argv[1]);
    snprintf(roms, sizeof(roms), "%s/roms", argv[1]);
    snprintf(config, sizeof(config), "%s/users/428814950/config", argv[1]);
    snprintf(state2, sizeof(state2), "%s/state2", argv[1]);
    snprintf(roms2, sizeof(roms2), "%s/roms2", argv[1]);
    snprintf(users2, sizeof(users2), "%s/users2", argv[1]);
    mkdir(roms2, 0755);
    mkdir(users2, 0755);

    cloudInit(state);
    check(!cloudSignedIn(), "not signed in at first");
    _Atomic bool cancel = false;
    check(cloudSignIn(&cancel), "sign-in (code shown, approved on the second poll)");
    printf("     code %s at %s\n", cloudStatus()->user_code, cloudStatus()->verify_url);
    check(cloudSignedIn(), "the sign-in is kept");
    check(cloudSyncRoms(roms, true, false), "ROMs backed up");
    check(cloudSyncRoms(roms, true, false), "a second backup sends nothing new");
    check(cloudBackupProfile("428814950", config), "settings backed up");
    check(cloudBackupProfile("428814950", config), "unchanged settings are not sent again");

    // A fresh install: new state, empty folders, signed in again.
    cloudInit(state2);
    check(!cloudSignedIn(), "a fresh install is not signed in");
    check(cloudSignIn(&cancel), "sign-in again");
    check(cloudSyncRoms(roms2, false, true), "ROMs restored");
    unsigned restored = 0;
    check(cloudRestoreProfiles(users2, &restored) && restored == 1, "one profile's settings restored");
    printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures != 0;
}
