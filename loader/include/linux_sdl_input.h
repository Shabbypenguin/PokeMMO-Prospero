// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_sdl_input.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: PS5: linuxSdlInputStickToDpad (loader-28).
#pragma once
#include "linux_sdl_events.h"
#include <stdio.h>

// Reads the console's input (controller, touch screen) into a snapshot for linux_sdl_events.c. Switch only.
bool linuxSdlInputSample(LinuxInputSnapshot *snapshot);
// PS5 (loader-28): the left stick also presses the d-pad (PokeMMO does not move with a stick): the direction it leans most, from
// halfway out, held until it comes back to a third. The stick's own axes are left as they are.
void linuxSdlInputStickToDpad(LinuxInputSnapshot *snapshot);
// The console's software keyboard (swkbd): blocks until the player confirms or cancels. UTF-8 text in `out`; false when cancelled.
// The inline software keyboard: the applet draws it over the game, which keeps running. The game is told what is typed with
// key and text events (linuxSdlEventsTextChanged). Request it with linuxSdlInputKeyboardRequest; linuxSdlInputKeyboardPump
// (called every frame from the SDL event pump) moves it along without ever blocking.
void linuxSdlInputKeyboardRequest(bool show);
void linuxSdlInputKeyboardPump(void);
bool linuxSdlInputKeyboardVisible(void);
bool linuxSdlInputKeyboardAvailable(void);  // false when the inline keyboard cannot be started (the modal one is used instead)
