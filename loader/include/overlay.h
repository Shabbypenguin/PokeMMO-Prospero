// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// What this project draws over a picture: the loading screen, the on-screen keyboard and the link box. OpenGL's fixed
// function pipeline (as the cursor and the file chooser), on whichever context is current; the game's state is saved
// and restored around it. Coordinates are a virtual 1920x1080 screen, colors are 0xRRGGBBAA.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OVERLAY_WIDTH 1920.0f
#define OVERLAY_HEIGHT 1080.0f

bool overlayBegin(unsigned width, unsigned height);  // false (and nothing to end) when OpenGL cannot be used
void overlayEnd(void);
void overlayClear(uint32_t rgba);  // the whole picture, for screens of our own
void overlayRect(float x, float y, float w, float h, uint32_t rgba);
void overlayFrame(float x, float y, float w, float h, float thickness, uint32_t rgba);  // outline inside the rectangle
// Text in DejaVu Sans; `size` is the line height in pixels, y the top of the line. Codes 1-4 are the controller's face
// buttons (overlay_assets.h). Returns the width drawn.
float overlayText(float x, float y, const char *text, float size, uint32_t rgba);
float overlayTextWidth(const char *text, float size);
float overlayTextCentered(float centre_x, float y, const char *text, float size, uint32_t rgba);
void overlayLogo(float x, float y, float height);  // the project logo, `height` tall, width kept in proportion
// Text cut to fit `max_width`, with "..." where it was cut.
void overlayTextFit(float x, float y, const char *text, float size, float max_width, uint32_t rgba);
