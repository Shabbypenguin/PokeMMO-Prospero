// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// The loading screen (loading_screen.c): the logo, one bar and a line of status; the details behind it while Triangle is
// held. The platform decides what to show (main_ps5.c), this only draws it, between overlayBegin and overlayEnd.
#pragma once
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
    const char *log_path;
    const LoadingStep *steps;
    unsigned step_count;
    bool details;  // Triangle held
    unsigned frame;
} LoadingView;
void loadingScreenDraw(const LoadingView *view);
