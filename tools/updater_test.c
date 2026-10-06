// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Runs the client updater on a PC against tools/range_server.py: check, download, apply. `make updater-test`.
#include "updater.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: updater_test URL STAGING GAME [KNOWN_ETAG]\n");
        return 2;
    }
    char error[256] = "";
    UpdaterRemote remote;
    int checked = updaterCheck(argv[1], argc > 4 ? argv[4] : NULL, &remote, error, sizeof(error));
    printf("check=%d etag=%s length=%lld revision=%s files=%u download=%llu error=%s\n", checked, remote.etag, (long long)remote.length,
           remote.revision, remote.count, (unsigned long long)remote.download_bytes, error);
    if (checked != 0) return checked == 1 ? 0 : 1;
    UpdaterProgress progress;
    int downloaded = updaterDownload(argv[1], &remote, argv[2], &progress, error, sizeof(error));
    printf("download=%d done=%llu of %llu error=%s\n", downloaded, (unsigned long long)progress.done, (unsigned long long)progress.total, error);
    if (downloaded) return 1;
    int applied = updaterApply(&remote, argv[2], argv[3], error, sizeof(error));
    printf("apply=%d error=%s\n", applied, error);
    updaterFree(&remote);
    return applied ? 1 : 0;
}
