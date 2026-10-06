// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// The on-screen keyboard (osk.c): drawn by the loader, driven by the controller, types into the game's focused field.
#pragma once
#include "linux_sdl_events.h"
#include <stdbool.h>

void oskShow(bool show);
bool oskVisible(void);
void oskFeed(const LinuxInputSnapshot *snapshot);  // while visible: the controller belongs to the keyboard
void oskDraw(void);                                // between overlayBegin and overlayEnd
