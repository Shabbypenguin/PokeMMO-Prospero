// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The loading screen (see loading_screen.h). Virtual 1920x1080 coordinates (overlay.h), the logo's colours.
#include "loading_screen.h"
#include "overlay.h"
#include "overlay_assets.h"
#include "qrcodegen.h"
#include <stdio.h>
#include <string.h>

static const uint32_t BACKGROUND = 0x081228FFu, TEXT = 0xFFFFFFFFu, SOFT = 0xC8D6F0FFu, DIM = 0x8A9BBFFFu, FAINT = 0x5F7096FFu, BLUE = 0x5AA0E8FFu,
                      TRACK = 0x1B2B4FFFu, RED = 0xE05555FFu, AMBER = 0xF0C060FFu;

static uint32_t stepColor(int state, unsigned frame) {
    switch (state) {
        case LOADING_PASS: return 0x26BF4DFFu;
        case LOADING_FAIL: return 0xD93333FFu;
        case LOADING_INFO: return 0x4073D9FFu;
        case LOADING_RUNNING: return (frame / 20) % 2 ? 0xF2CC1AFFu : 0xBF9919FFu;
        default: return 0x595961FFu;
    }
}
// Up to three lines, broken between words, centred.
static void drawWrapped(float y, const char *text, float size, float max_width, uint32_t rgba) {
    char line[256];
    const char *rest = text;
    for (int row = 0; row < 3 && *rest; ++row) {
        size_t fit = 0, last_space = 0, length = strlen(rest);
        while (fit < length && fit < sizeof(line) - 1) {
            memcpy(line, rest, fit + 1);
            line[fit + 1] = 0;
            if (overlayTextWidth(line, size) > max_width) break;
            if (rest[fit] == ' ') last_space = fit;
            ++fit;
        }
        size_t take = fit == length || !last_space ? fit : last_space;
        memcpy(line, rest, take);
        line[take] = 0;
        overlayTextCentered(OVERLAY_WIDTH / 2, y + (float)row * size * 1.25f, line, size, rgba);
        rest += take;
        while (*rest == ' ') ++rest;
    }
}
static void drawDetails(const LoadingView *view) {
    overlayClear(0x050B18FFu);
    overlayTextCentered(OVERLAY_WIDTH / 2, 90, "Startup details", 56, TEXT);
    const unsigned columns = 3;
    const float tile_w = 520, tile_h = 120, gap = 30, left = (OVERLAY_WIDTH - columns * tile_w - (columns - 1) * gap) / 2, top = 220;
    for (unsigned i = 0; i < view->step_count; ++i) {
        float x = left + (float)(i % columns) * (tile_w + gap), y = top + (float)(i / columns) * (tile_h + gap);
        int state = view->steps[i].state;
        overlayRect(x, y, tile_w, tile_h, 0x111D36FFu);
        overlayRect(x, y, 16, tile_h, stepColor(state, view->frame));
        overlayText(x + 40, y + 20, view->steps[i].name, 40, TEXT);
        const char *word = state == LOADING_PASS      ? "OK"
                           : state == LOADING_FAIL    ? "Failed"
                           : state == LOADING_INFO    ? "Note"
                           : state == LOADING_RUNNING ? "Running"
                                                      : "Not run";
        overlayText(x + 40, y + 68, word, 32, DIM);
    }
    char line[200];
    snprintf(line, sizeof(line), "Full log: %s (FTP)", view->log_path ? view->log_path : "prospero.log");
    overlayTextCentered(OVERLAY_WIDTH / 2, 900, line, 34, DIM);
    overlayTextCentered(OVERLAY_WIDTH / 2, 960, "Release \x04 to go back", 34, DIM);
}
void loadingScreenDraw(const LoadingView *view) {
    if (view->details) {
        drawDetails(view);
        return;
    }
    overlayClear(BACKGROUND);
    overlayLogo(OVERLAY_WIDTH / 2 - 210, 150, 420);
    overlayTextCentered(OVERLAY_WIDTH / 2, 610, "PokeMMO Prospero", 64, TEXT);
    const float bar_x = 560, bar_y = 740, bar_w = 800, bar_h = 12;
    float fraction = view->fraction < 0 ? 0 : view->fraction > 1 ? 1 : view->fraction;
    overlayRect(bar_x, bar_y, bar_w, bar_h, TRACK);
    if (view->problem) {
        overlayRect(bar_x, bar_y, bar_w * (fraction > 0.05f ? fraction : 0.05f), bar_h, RED);
        overlayTextCentered(OVERLAY_WIDTH / 2, 790, view->problem, 44, TEXT);
        if (view->advice) drawWrapped(860, view->advice, 32, 1100, 0xA8B8D8FFu);
    } else if (view->question) {
        overlayRect(bar_x, bar_y, bar_w * fraction, bar_h, BLUE);
        overlayTextCentered(OVERLAY_WIDTH / 2, 790, view->question, 40, TEXT);
        if (view->choices) overlayTextCentered(OVERLAY_WIDTH / 2, 855, view->choices, 34, SOFT);
    } else {
        overlayRect(bar_x, bar_y, bar_w * fraction, bar_h, BLUE);
        const char *status = view->status ? view->status : "";
        char line[128];
        snprintf(line, sizeof(line), "%s%.*s", status, (int)((view->frame / 30) % 4), "...");
        overlayText(OVERLAY_WIDTH / 2 - overlayTextWidth(status, 40) / 2, 790, line, 40, SOFT);  // the dots do not move the words
        if (view->detail && view->detail[0]) overlayTextCentered(OVERLAY_WIDTH / 2, 850, view->detail, 32, DIM);
        if (view->warning) overlayTextCentered(OVERLAY_WIDTH / 2, 910, view->warning, 30, AMBER);
    }
    overlayText(48, 1020, "Hold \x04 for details", 28, view->problem ? SOFT : FAINT);
    if (view->version) overlayText(OVERLAY_WIDTH - 48 - overlayTextWidth(view->version, 26), 990, view->version, 26, FAINT);
    if (view->revision && view->revision[0]) {
        char line[96];
        snprintf(line, sizeof(line), "PokeMMO revision %s", view->revision);
        overlayText(OVERLAY_WIDTH - 48 - overlayTextWidth(line, 26), 1025, line, 26, FAINT);
    }
}

// A QR code, white with its quiet zone, `size` pixels square; the code is remade only when the text changes.
static void drawQr(float x, float y, float size, const char *text) {
    static char encoded_text[256];
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    static bool have;
    if (strcmp(encoded_text, text)) {
        uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
        snprintf(encoded_text, sizeof(encoded_text), "%s", text);
        have = qrcodegen_encodeText(text, scratch, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
    }
    if (!have) return;
    int modules = qrcodegen_getSize(qr);
    float m = size / (float)(modules + 8);
    overlayRect(x, y, size, size, 0xFFFFFFFFu);
    for (int row = 0; row < modules; ++row)
        for (int column = 0; column < modules;) {
            if (!qrcodegen_getModule(qr, column, row)) {
                ++column;
                continue;
            }
            int end = column;
            while (end < modules && qrcodegen_getModule(qr, end, row)) ++end;
            overlayRect(x + (4.0f + (float)column) * m, y + (4.0f + (float)row) * m, (float)(end - column) * m + 0.5f, m + 0.5f, 0x000000FFu);
            column = end;
        }
}

// ---- the ROM screen ------------------------------------------------------------------------------------------------------------------
void romScreenDraw(const RomScan *scan, const RomUploadInfo *upload, bool blocking) {
    const uint32_t GREEN = 0x7EE08AFFu;
    overlayClear(0x050B18FFu);
    overlayTextCentered(OVERLAY_WIDTH / 2, 60, "Game ROMs", 56, TEXT);
    if (blocking)
        overlayTextCentered(OVERLAY_WIDTH / 2, 135, "PokeMMO needs Pokemon Black or White (DS) to play.", 34, AMBER);
    else
        overlayTextCentered(OVERLAY_WIDTH / 2, 135, "PokeMMO needs Black or White; the other games add their regions.", 30, SOFT);
    const float left = 240, width = 1440, row_h = 66, top = 205;
    overlayRect(left, top - 10, width, ROM_GAMES * row_h + 10, 0x0C1630FFu);
    for (int game = 0; game < ROM_GAMES; ++game) {
        float y = top + (float)game * row_h;
        int index = scan->found[game];
        overlayText(left + 30, y + 10, romGameName(game), 36, TEXT);
        overlayText(left + 520, y + 16, romGameRequired(game) ? "Required" : "Optional", 28, DIM);
        if (index >= 0)
            overlayTextFit(left + 720, y + 16, scan->files[index].note, 28, width - 750, GREEN);
        else
            overlayText(left + 720, y + 16, "Missing", 28, romGameRequired(game) ? RED : AMBER);
    }
    float y = top + ROM_GAMES * row_h + 20;
    if (!scan->listed)
        overlayTextCentered(OVERLAY_WIDTH / 2, y, "The ROM folder could not be read.", 30, RED);
    else {
        unsigned others = 0;
        for (unsigned i = 0; i < scan->count; ++i) others += scan->files[i].game < 0;
        if (others) overlayText(left, y, "Other files in the ROM folder:", 28, DIM);
        unsigned shown = 0;
        for (unsigned i = 0; i < scan->count && shown < 2; ++i) {
            if (scan->files[i].game >= 0) continue;
            char line[300];
            snprintf(line, sizeof(line), "%s  -  %s", scan->files[i].file, scan->files[i].note);
            overlayTextFit(left + 30, y + 34 + 30 * (float)shown++, line, 26, width - 30, SOFT);
        }
        if (others > shown || scan->more) {
            char line[80];
            snprintf(line, sizeof(line), "... and %u more", others - shown + scan->more);
            overlayText(left + 30, y + 34 + 30 * (float)shown, line, 26, DIM);
        }
    }
    const float box_y = 700;
    overlayRect(left, box_y, width, 260, 0x111D36FFu);
    overlayText(left + 30, box_y + 14, "Upload ROM files (.nds, .gba) from a PC or phone on the same network:", 28, SOFT);
    char line[300];
    float row = box_y + 56;
    if (upload->web) {
        overlayText(left + 30, row, "In a browser, open", 30, DIM);
        snprintf(line, sizeof(line), "http://%s:8080", upload->address);
        overlayText(left + 330, row - 4, line, 38, BLUE);
        drawQr(left + width - 240, box_y + 10, 240, line);  // scan with a phone instead of typing the address
        row += 50;
    }
    if (upload->ftp_port) {
        overlayText(left + 30, row, "Or with an FTP app", 30, DIM);
        snprintf(line, sizeof(line), "Address %s     Port %u", upload->address, upload->ftp_port);
        overlayText(left + 330, row + 2, line, 28, BLUE);
        snprintf(line, sizeof(line), "Folder %s", upload->folder);
        overlayTextFit(left + 330, row + 38, line, 28, width - 360 - (upload->web ? 260 : 0), BLUE);
        row += 80;
    }
    if (upload->receiving && upload->receiving[0])
        overlayTextFit(left + 30, row - 4, upload->receiving, 26, width - 60 - (upload->web ? 260 : 0), GREEN);
    overlayText(left + 30, box_y + 224, "Or run the installer on your PC and choose your ROM folder.", 24, DIM);
    if (blocking)
        overlayTextCentered(OVERLAY_WIDTH / 2, 995, "\x01 Check again          \x02 Start anyway", 34, TEXT);
    else
        overlayTextCentered(OVERLAY_WIDTH / 2, 995, "Release \x03 to go back", 30, DIM);
}
