// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// The loading screen (loading_screen.c): the logo, one bar and a line of status; the details behind it while Triangle is
// held. The platform decides what to show (main_ps5.c), this only draws it, between overlayBegin and overlayEnd.
#pragma once
#include "roms.h"
#include <stdbool.h>

enum { LOADING_NOT_RUN = 0, LOADING_PASS, LOADING_FAIL, LOADING_INFO, LOADING_RUNNING };
typedef struct {
    const char *name;
    int state;
} LoadingStep;
typedef struct {
    float fraction;       // 0..1
    const char *status;   // "Installing PokeMMO" (animated dots are added)
    const char *detail;   // second line, may be empty
    const char *problem;  // set when starting cannot go on: the bar turns red
    const char *advice;   // what to do about the problem
    const char *warning;  // a note that does not stop anything (no ROMs)
    const char *question; // a choice for the player (an update), shown instead of the status
    const char *choices;  // its buttons
    const char *version;  // bottom right
    const char *revision;
    const char *log_where;  // where the full log can be had, as a phrase ("/data/.../prospero.log (FTP)", "http://.../log")
    const LoadingStep *steps;
    unsigned step_count;
    bool details;  // Triangle held
    bool no_input;  // loader-32: the client is starting and the controller is not read: no "Hold Triangle" hint
    unsigned frame;
} LoadingView;
void loadingScreenDraw(const LoadingView *view);
// The ROM screen: what is in the ROM folder against what PokeMMO uses, and where to upload. REQUIRED: the game cannot start as
// things are (Cross checks again). OPTIONAL: the player chose to add games (Cross starts the game).
// Where to upload: the web page (when running), an FTP server (the console's own payload, or the title's), the folder.
typedef struct {
    const char *address;  // the console's IP
    bool web;             // the title's upload page runs (port 8080)
    unsigned ftp_port;    // an FTP server to use, 0 when none
    const char *folder;   // the ROM folder as FTP shows it
    const char *receiving;  // "Receiving ...", or NULL
} RomUploadInfo;
enum { ROM_SCREEN_NONE, ROM_SCREEN_REQUIRED, ROM_SCREEN_OPTIONAL };
void romScreenDraw(const RomScan *scan, const RomUploadInfo *upload, int mode);
