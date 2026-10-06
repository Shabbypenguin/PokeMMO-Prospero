// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_sdl_cursor.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: none.
#pragma once
#include "linux_sdl_events.h"

// A mouse cursor driven by the controller, for a console without a usable touch screen (a Switch Lite whose screen was replaced, TV mode
// on a sofa). A click of the left stick (L3) turns it on or off. While it is on, the left stick moves it, ZR is the left click and ZL the
// right click, and the game sees an idle controller. The cursor is drawn over the game.
void linuxSdlCursorUpdate(LinuxInputSnapshot *snapshot, unsigned width, unsigned height);  // between the sampling and linuxSdlEventsUpdate
void linuxSdlCursorDraw(unsigned width, unsigned height);                                  // on the rendering thread, before the buffers are swapped
