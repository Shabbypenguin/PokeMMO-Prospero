// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_sdl.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: linuxSdlSetDisplayAcquire (the loading screen hands over the display); link box and on-screen keyboard.
#pragma once
#include "linux_dl.h"
#include <stdbool.h>
#include <stdio.h>

// This project's own implementations of the shared libraries the client's Java layer loads by name (LWJGL binds them with
// dlsym). They are "virtual": no ELF file exists for them, the functions below are the library.
//   libSDL3   window, OpenGL context (EGL on the Switch's window), events, time, gamepads (subset)
//   libGLX    the same entry points under the name LWJGL tries first
//   libEGL    eglGetProcAddress and the GL entry points of Mesa's EGL
//   libopenal OpenAL 1.1 on a software mixer (linux_audio.c) feeding the console's audio output
extern const LinuxVirtualLibrary linuxSdlLibrary, linuxEglLibrary, linuxGlxLibrary, linuxOpenAlLibrary;

void linuxSdlStopRequest(void);  // the next SDL_PollEvent reports SDL_EVENT_QUIT (applet exit)
// The picture should get this size (the console was docked or undocked). Any thread may ask; the thread that renders does it between two frames.
void linuxSdlRequestSize(unsigned width, unsigned height);
// Called on the game's thread before its window surface is made (PS5: the loading screen owns the only surface until then).
void linuxSdlSetDisplayAcquire(void (*acquire)(void));
bool linuxSdlReset(void);  // tears down the EGL surface and context created for the window
// For the file chooser, when the game calls it on the thread that renders: whether this thread owns the GL context, and one frame of the
// chooser (input sampled, picture swapped) so that it can run its own loop.
bool linuxSdlThreadHasContext(void);
bool linuxSdlPresentFrame(void);

typedef struct {
    unsigned windows, contexts, frames, calls, unknown_lookups;
    bool gl_ready;
    unsigned width, height;
} LinuxSdlStats;
LinuxSdlStats linuxSdlStats(void);
