// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Runs the ROM upload servers (web page on 8080, FTP on 2121) on a PC over a folder, for testing with a browser, curl or an
// FTP client. `upload_test FOLDER [SECONDS]`.
#include "upload_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void changed(void) { printf("changed: %u files received\n", uploadStatus()->received), fflush(stdout); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    printf("existing FTP server: port %u\n", uploadDetectFtp());
    uploadServersStart(argv[1], "/data/homebrew/PPSA27166/roms", true, changed);
    printf("http=%d ftp=%d\n", uploadStatus()->http_running, uploadStatus()->ftp_running);
    fflush(stdout);
    sleep(argc > 2 ? (unsigned)atoi(argv[2]) : 60);
    uploadServersStop();
    printf("stopped\n");
    return 0;
}
