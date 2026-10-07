// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Getting ROMs onto the console without anything else installed (upload_server.c), while the ROM screen is up:
//   - a web page (port 8080): open it in a browser on a PC or phone and pick the files;
//   - a small FTP server (port 2121, or the next free port up to 2125 when something else holds it): for FTP apps and the
//     installer.
// Both only ever write into the ROM folder. When the game starts uploads end; the web page stays up for the log (loader-28).
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define UPLOAD_HTTP_PORT 8080
#define UPLOAD_FTP_PORT 2121

typedef struct {
    _Atomic bool http_running, ftp_running;
    _Atomic unsigned ftp_port;              // the port the title's FTP server got (2121, or the next free one)
    _Atomic unsigned received;              // files completed
    _Atomic uint64_t current_done, current_total;
    char current[128];                      // the file arriving now ("" when none)
} UploadStatus;

// The port of an FTP server already running on the console (a payload such as ftpsrv), 0 when none answers.
unsigned uploadDetectFtp(void);
// `folder`: where files go (the title's roms/); `ftp_path`: the path FTP clients see for it (/data/homebrew/<ID>/roms).
// `changed` is called after each file arrives (the ROM list is read again). Starts what is not running yet.
void uploadServersStart(const char *folder, const char *ftp_path, bool with_ftp, void (*changed)(void));
void uploadServersStop(void);
// loader-28: before the game starts: uploads end (one arriving is finished first), the FTP server stops and the web page keeps
// running (started if it was not) with only the log and the settings to download.
void uploadServersDownloadsOnly(const char *folder);
// Files the web page offers for download (/log, /settings); NULL or "" for none.
void uploadSetDownloads(const char *log_path, const char *settings_path);
UploadStatus *uploadStatus(void);
