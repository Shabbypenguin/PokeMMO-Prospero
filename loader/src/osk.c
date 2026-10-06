// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The on-screen keyboard. The PS5's system keyboard (libSceImeDialog) loads in a title but its functions cannot be looked
// up (loader-2), so the loader draws its own over the game and types into whichever field the game has focused: every key
// becomes the SDL key or text event a real keyboard would give. Nothing typed is kept here (passwords pass straight through).
//
// Controls while it is up: d-pad or left stick move, Cross types, Square deletes, Triangle is space, L1/R1 move the text
// cursor, L2 switches letters/symbols, R2 is shift (once, then caps lock, then off), the touchpad is Tab, Options is Enter,
// L3 moves the keyboard between the bottom and the top of the screen, Circle or R3 closes it.
#include "osk.h"
#include "diagnostics.h"
#include "linux_sdl_events.h"
#include "overlay.h"
#include "overlay_assets.h"
#include <stdatomic.h>
#include <string.h>

enum { KEY_TEXT, KEY_SHIFT, KEY_PAGE, KEY_SPACE, KEY_TAB, KEY_DELETE, KEY_ENTER };
typedef struct {
    const char *label;  // also the text typed, for KEY_TEXT
    int kind;
    float units;  // width; a row is 10 units
} Key;
#define T(c) {c, KEY_TEXT, 1}
#define ROW_END {NULL, 0, 0}
static const Key letters[][11] = {
    {T("1"), T("2"), T("3"), T("4"), T("5"), T("6"), T("7"), T("8"), T("9"), T("0"), ROW_END},
    {T("q"), T("w"), T("e"), T("r"), T("t"), T("y"), T("u"), T("i"), T("o"), T("p"), ROW_END},
    {T("a"), T("s"), T("d"), T("f"), T("g"), T("h"), T("j"), T("k"), T("l"), T("@"), ROW_END},
    {T("z"), T("x"), T("c"), T("v"), T("b"), T("n"), T("m"), T(","), T("."), T("-"), ROW_END},
    {{"Shift", KEY_SHIFT, 1.5f}, {"?123", KEY_PAGE, 1.5f}, {"Space", KEY_SPACE, 3}, {"Tab", KEY_TAB, 1.25f}, {"Del", KEY_DELETE, 1.25f}, {"Enter", KEY_ENTER, 1.5f}, ROW_END},
};
static const Key capitals[][11] = {
    {T("1"), T("2"), T("3"), T("4"), T("5"), T("6"), T("7"), T("8"), T("9"), T("0"), ROW_END},
    {T("Q"), T("W"), T("E"), T("R"), T("T"), T("Y"), T("U"), T("I"), T("O"), T("P"), ROW_END},
    {T("A"), T("S"), T("D"), T("F"), T("G"), T("H"), T("J"), T("K"), T("L"), T("@"), ROW_END},
    {T("Z"), T("X"), T("C"), T("V"), T("B"), T("N"), T("M"), T(","), T("."), T("-"), ROW_END},
    {{"Shift", KEY_SHIFT, 1.5f}, {"?123", KEY_PAGE, 1.5f}, {"Space", KEY_SPACE, 3}, {"Tab", KEY_TAB, 1.25f}, {"Del", KEY_DELETE, 1.25f}, {"Enter", KEY_ENTER, 1.5f}, ROW_END},
};
static const Key symbols[][11] = {
    {T("1"), T("2"), T("3"), T("4"), T("5"), T("6"), T("7"), T("8"), T("9"), T("0"), ROW_END},
    {T("!"), T("#"), T("$"), T("%"), T("^"), T("&"), T("*"), T("("), T(")"), T("_"), ROW_END},
    {T("~"), T("`"), T("="), T("+"), T("["), T("]"), T("{"), T("}"), T("\\"), T("|"), ROW_END},
    {T(";"), T(":"), T("'"), T("\""), T("<"), T(">"), T("/"), T("?"), ROW_END},
    {{"Shift", KEY_SHIFT, 1.5f}, {"ABC", KEY_PAGE, 1.5f}, {"Space", KEY_SPACE, 3}, {"Tab", KEY_TAB, 1.25f}, {"Del", KEY_DELETE, 1.25f}, {"Enter", KEY_ENTER, 1.5f}, ROW_END},
};
#undef T
#define ROWS 5

// SDL 3 numbers for the keys that are not text.
enum {
    SC_RETURN = 40, SC_BACKSPACE = 42, SC_TAB = 43, SC_RIGHT = 79, SC_LEFT = 80,
    KC_RETURN = 13, KC_BACKSPACE = 8, KC_TAB = 9, KC_RIGHT = 0x4000004F, KC_LEFT = 0x40000050
};
// Controller bits of LinuxInputSnapshot.buttons (SDL gamepad buttons).
enum { B_CROSS = 0, B_CIRCLE = 1, B_SQUARE = 2, B_TRIANGLE = 3, B_TOUCHPAD = 4, B_OPTIONS = 6, B_L3 = 7, B_L1 = 9, B_R1 = 10, B_UP = 11, B_DOWN = 12, B_LEFT = 13, B_RIGHT = 14 };

static atomic_bool visible;
static atomic_flag guard = ATOMIC_FLAG_INIT;
static struct {
    int row, column;
    bool symbols, top;
    int shift;  // 0 off, 1 next letter, 2 caps lock
    bool fresh;  // just opened: what is held now does not count
    uint32_t before;
    bool l2_before, r2_before;
    uint64_t repeat_at[16];  // per held input: when it repeats next
} k;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }

static const Key (*page(void))[11] { return k.symbols ? symbols : k.shift ? capitals : letters; }
static int rowLength(int row) {
    int n = 0;
    while (page()[row][n].label) ++n;
    return n;
}
static float rowUnits(int row) {
    float units = 0;
    for (int i = 0; i < rowLength(row); ++i) units += page()[row][i].units;
    return units;
}
static float keyCentre(int row, int column) {  // in units from the left edge of a 10-unit row
    float x = (10.0f - rowUnits(row)) / 2;
    for (int i = 0; i < column; ++i) x += page()[row][i].units;
    return x + page()[row][column].units / 2;
}
static void moveVertical(int direction) {
    float centre = keyCentre(k.row, k.column);
    k.row = (k.row + direction + ROWS) % ROWS;
    int best = 0;
    float best_distance = 1e9f;
    for (int i = 0; i < rowLength(k.row); ++i) {
        float d = keyCentre(k.row, i) - centre;
        if (d < 0) d = -d;
        if (d < best_distance) best_distance = d, best = i;
    }
    k.column = best;
}
static void key(uint32_t scancode, uint32_t keycode, uint64_t now) {
    linuxSdlEventsPushKey(scancode, keycode, true, now);
    linuxSdlEventsPushKey(scancode, keycode, false, now);
}
static void typeText(const char *text, uint64_t now) { linuxSdlEventsTextChanged("", text, now); }
static void press(uint64_t now) {
    const Key *pressed = &page()[k.row][k.column];
    switch (pressed->kind) {
        case KEY_TEXT:
            typeText(pressed->label, now);
            if (k.shift == 1) k.shift = 0;
            break;
        case KEY_SHIFT: k.shift = (k.shift + 1) % 3; break;
        case KEY_PAGE: k.symbols = !k.symbols; break;
        case KEY_SPACE: typeText(" ", now); break;
        case KEY_TAB: key(SC_TAB, KC_TAB, now); break;
        case KEY_DELETE: key(SC_BACKSPACE, KC_BACKSPACE, now); break;
        case KEY_ENTER: key(SC_RETURN, KC_RETURN, now); break;
    }
    if (k.column >= rowLength(k.row)) k.column = rowLength(k.row) - 1;  // the page changed under the highlight
}

void oskShow(bool show) {
    lock();
    if (show && !atomic_load(&visible)) {
        k.fresh = true;
        k.shift = 0;
        k.symbols = false;
    }
    atomic_store(&visible, show);
    unlock();
    diagnosticsTrace("osk.%s", show ? "show" : "hide");
}
bool oskVisible(void) { return atomic_load(&visible); }

// Held inputs repeat: first after 400 ms, then every 70 ms.
static bool fire(int slot, bool held, bool was_held, uint64_t now) {
    if (!held) return false;
    if (!was_held) {
        k.repeat_at[slot] = now + 400000000ull;
        return true;
    }
    if (now >= k.repeat_at[slot]) {
        k.repeat_at[slot] = now + 70000000ull;
        return true;
    }
    return false;
}
void oskFeed(const LinuxInputSnapshot *s) {
    if (!atomic_load(&visible) || !s->gamepad) return;
    lock();
    uint64_t now = s->timestamp_ns;
    // The left stick works as the d-pad.
    uint32_t held = s->buttons;
    if (s->axes[1] < -16000) held |= 1u << B_UP;
    if (s->axes[1] > 16000) held |= 1u << B_DOWN;
    if (s->axes[0] < -16000) held |= 1u << B_LEFT;
    if (s->axes[0] > 16000) held |= 1u << B_RIGHT;
    bool l2 = s->axes[4] > 16000, r2 = s->axes[5] > 16000;
    if (k.fresh) {
        k.before = held;
        k.l2_before = l2;
        k.r2_before = r2;
        k.fresh = false;
        unlock();
        return;
    }
    uint32_t was = k.before, pressed = held & ~was;
#define HELD(bit) (((held >> (bit)) & 1u) != 0)
#define WAS(bit) (((was >> (bit)) & 1u) != 0)
    if (fire(0, HELD(B_UP), WAS(B_UP), now)) moveVertical(-1);
    if (fire(1, HELD(B_DOWN), WAS(B_DOWN), now)) moveVertical(1);
    if (fire(2, HELD(B_LEFT), WAS(B_LEFT), now)) k.column = (k.column + rowLength(k.row) - 1) % rowLength(k.row);
    if (fire(3, HELD(B_RIGHT), WAS(B_RIGHT), now)) k.column = (k.column + 1) % rowLength(k.row);
    if (fire(4, HELD(B_CROSS), WAS(B_CROSS), now)) press(now);
    if (fire(5, HELD(B_SQUARE), WAS(B_SQUARE), now)) key(SC_BACKSPACE, KC_BACKSPACE, now);
    if (fire(6, HELD(B_TRIANGLE), WAS(B_TRIANGLE), now)) typeText(" ", now);
    if (fire(7, HELD(B_L1), WAS(B_L1), now)) key(SC_LEFT, KC_LEFT, now);
    if (fire(8, HELD(B_R1), WAS(B_R1), now)) key(SC_RIGHT, KC_RIGHT, now);
    if (pressed & (1u << B_TOUCHPAD)) key(SC_TAB, KC_TAB, now);
    if (pressed & (1u << B_OPTIONS)) key(SC_RETURN, KC_RETURN, now);
    if (pressed & (1u << B_L3)) k.top = !k.top;
    if (l2 && !k.l2_before) k.symbols = !k.symbols;
    if (r2 && !k.r2_before) k.shift = (k.shift + 1) % 3;
    if (k.column >= rowLength(k.row)) k.column = rowLength(k.row) - 1;
#undef HELD
#undef WAS
    k.before = held;
    k.l2_before = l2;
    k.r2_before = r2;
    bool close = (pressed & (1u << B_CIRCLE)) != 0;
    unlock();
    if (close) oskShow(false);
}

void oskDraw(void) {
    if (!atomic_load(&visible)) return;
    lock();
    const float unit = 112, gap = 8, key_h = 70, pad = 24, legend_h = 76;
    const float panel_w = 10 * unit + 2 * pad, panel_h = ROWS * key_h + legend_h + 2 * pad;
    const float panel_x = (OVERLAY_WIDTH - panel_w) / 2, panel_y = k.top ? 24 : OVERLAY_HEIGHT - panel_h - 24;
    overlayRect(panel_x, panel_y, panel_w, panel_h, 0x081228EEu);
    overlayFrame(panel_x, panel_y, panel_w, panel_h, 2, 0x5AA0E8FFu);
    for (int row = 0; row < ROWS; ++row) {
        float x = panel_x + pad + (10.0f - rowUnits(row)) / 2 * unit, y = panel_y + pad + (float)row * key_h;
        for (int column = 0; column < rowLength(row); ++column) {
            const Key *item = &page()[row][column];
            float w = item->units * unit;
            bool selected = row == k.row && column == k.column;
            bool lit = (item->kind == KEY_SHIFT && k.shift) || (item->kind == KEY_PAGE && k.symbols);
            overlayRect(x + gap / 2, y + gap / 2, w - gap, key_h - gap, selected ? 0x5AA0E8FFu : lit ? 0x2E4F86FFu : 0x16264AFFu);
            const char *label = item->kind == KEY_SHIFT && k.shift == 2 ? "CAPS" : item->label;
            float size = item->kind == KEY_TEXT ? 40 : 30;
            overlayTextCentered(x + w / 2, y + (key_h - size) / 2, label, size, selected ? 0x050B18FFu : 0xFFFFFFFFu);
            x += w;
        }
    }
    const float legend_y = panel_y + pad + ROWS * key_h + 10;
    overlayTextCentered(OVERLAY_WIDTH / 2, legend_y, "\x01 Type     \x03 Delete     \x04 Space     \x02 Close", 26, 0xA8B8D8FFu);
    overlayTextCentered(OVERLAY_WIDTH / 2, legend_y + 34, "L1/R1 Cursor   L2 Symbols   R2 Shift   Touchpad Tab   Options Enter   L3 Move", 26, 0xA8B8D8FFu);
    unlock();
}
