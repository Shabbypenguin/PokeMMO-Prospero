// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The link box. The game opens some links in the browser (forgotten password, forgotten username, news): a PS5 title has no
// browser to give them to, so the address is shown with a QR code to open it on a phone. QR codes by Project Nayuki's
// qrcodegen (MIT, loader/src/qrcodegen.c).
#include "link_box.h"
#include "diagnostics.h"
#include "overlay.h"
#include "overlay_assets.h"
#include "qrcodegen.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define QR_VERSION_MAX 15  // up to about 400 characters at medium error correction
static atomic_bool visible;
static atomic_flag guard = ATOMIC_FLAG_INIT;
static char address[512];
static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
static bool have_qr, fresh;
static uint32_t before;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }

void linkBoxShow(const char *url) {
    uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
    lock();
    snprintf(address, sizeof(address), "%s", url ? url : "");
    have_qr = address[0] && qrcodegen_encodeText(address, scratch, qr, qrcodegen_Ecc_MEDIUM, 1, QR_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    fresh = true;
    atomic_store(&visible, true);
    unlock();
    diagnosticsTrace("link.show qr=%d url=%.200s", have_qr, address);
}
bool linkBoxVisible(void) { return atomic_load(&visible); }
void linkBoxFeed(const LinuxInputSnapshot *s) {
    if (!atomic_load(&visible) || !s->gamepad) return;
    lock();
    uint32_t pressed = s->buttons & ~before;
    if (fresh) pressed = 0;  // the press that opened the link does not close it
    fresh = false;
    before = s->buttons;
    unlock();
    if (pressed & 3u) atomic_store(&visible, false);  // Cross or Circle
}

// The address in lines of at most `max_width`, broken anywhere (addresses have few spaces).
static void drawAddress(float centre_x, float y, float size, float max_width) {
    char line[160];
    const char *rest = address;
    for (int row = 0; row < 3 && *rest; ++row) {
        size_t take = 0, length = strlen(rest);
        while (take < length && take < sizeof(line) - 1) {
            memcpy(line, rest, take + 1);
            line[take + 1] = 0;
            if (overlayTextWidth(line, size) > max_width) break;
            ++take;
        }
        memcpy(line, rest, take);
        line[take] = 0;
        if (row == 2 && take < length) memcpy(line + (take > 3 ? take - 3 : 0), "...", 4);
        overlayTextCentered(centre_x, y + (float)row * size * 1.2f, line, size, 0xC8D6F0FFu);
        rest += take;
    }
}
void linkBoxDraw(void) {
    if (!atomic_load(&visible)) return;
    lock();
    const float panel_w = 980, panel_h = 900, panel_x = (OVERLAY_WIDTH - panel_w) / 2, panel_y = (OVERLAY_HEIGHT - panel_h) / 2;
    const float centre = OVERLAY_WIDTH / 2;
    overlayRect(0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 0x000000A0u);
    overlayRect(panel_x, panel_y, panel_w, panel_h, 0x081228FFu);
    overlayFrame(panel_x, panel_y, panel_w, panel_h, 2, 0x5AA0E8FFu);
    overlayTextCentered(centre, panel_y + 40, "Open this link on your phone", 46, 0xFFFFFFFFu);
    float box = 500, box_x = centre - box / 2, box_y = panel_y + 120;
    overlayRect(box_x, box_y, box, box, 0xFFFFFFFFu);
    if (have_qr) {
        int size = qrcodegen_getSize(qr);
        float module = box / (float)(size + 8);  // the four modules of quiet zone a QR code needs on each side
        float origin_x = box_x + (box - module * (float)size) / 2, origin_y = box_y + (box - module * (float)size) / 2;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size;) {
                if (!qrcodegen_getModule(qr, x, y)) {
                    ++x;
                    continue;
                }
                int run = x;
                while (run < size && qrcodegen_getModule(qr, run, y)) ++run;
                overlayRect(origin_x + (float)x * module, origin_y + (float)y * module, (float)(run - x) * module + 0.5f, module + 0.5f, 0x000000FFu);
                x = run;
            }
    } else
        overlayTextCentered(centre, box_y + box / 2 - 20, "(too long for a QR code)", 32, 0x000000FFu);
    drawAddress(centre, box_y + box + 40, 30, panel_w - 80);
    overlayTextCentered(centre, panel_y + panel_h - 70, "\x01 / \x02  Close", 34, 0xA8B8D8FFu);
    unlock();
}
