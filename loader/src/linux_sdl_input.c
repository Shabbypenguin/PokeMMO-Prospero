// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_sdl_input.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the PS5 controller (through the platform layer) instead of the Switch's; no touch screen. The Switch's inline keyboard
// applet has no PS5 counterpart yet (the system keyboard module loads but its functions cannot be looked up): the keyboard is
// reported unavailable until the on-screen keyboard exists.
#include "linux_sdl_input.h"
#include "diagnostics.h"
#include "platform.h"
#include <stdatomic.h>

// SDL names the face buttons by position: Cross (south) confirms and Circle (east) cancels, which is what PokeMMO expects. Create,
// PS and the mic button never reach a title, so the touchpad click is SDL's Back (select) and Options is Start.
static uint32_t buttonBits(uint32_t held) {
    static const struct {
        uint32_t pad;
        int sdl;
    } map[] = {{PLATFORM_PAD_CROSS, 0},    {PLATFORM_PAD_CIRCLE, 1},  {PLATFORM_PAD_SQUARE, 2}, {PLATFORM_PAD_TRIANGLE, 3}, {PLATFORM_PAD_TOUCHPAD, 4},
               {PLATFORM_PAD_OPTIONS, 6},  {PLATFORM_PAD_L3, 7},      {PLATFORM_PAD_R3, 8},     {PLATFORM_PAD_L1, 9},        {PLATFORM_PAD_R1, 10},
               {PLATFORM_PAD_UP, 11},      {PLATFORM_PAD_DOWN, 12},   {PLATFORM_PAD_LEFT, 13},  {PLATFORM_PAD_RIGHT, 14}};
    uint32_t bits = 0;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (held & map[i].pad) bits |= 1u << map[i].sdl;
    return bits;
}
static int16_t stick(uint8_t value) {
    int centred = ((int)value - 128) * 258;  // 0..255 -> about -32767..32767; up is negative on both
    return (int16_t)(centred > 32767 ? 32767 : centred < -32767 ? -32767 : centred);
}
static int16_t trigger(uint8_t value) { return (int16_t)(value * 32767 / 255); }

bool linuxSdlInputSample(LinuxInputSnapshot *snapshot) {
    static atomic_bool logged;
    PlatformPad pad;
    bool read = platformPadRead(&pad);
    *snapshot = (LinuxInputSnapshot){0};
    snapshot->timestamp_ns = platformMonotonicNs();
    if (!atomic_exchange(&logged, true)) diagnosticsTrace("sdl.input pad_read=%d connected=%d", read, pad.connected);
    if (!read) return true;
    snapshot->gamepad = pad.connected;
    snapshot->buttons = buttonBits(pad.buttons);
    snapshot->axes[0] = stick(pad.lx);
    snapshot->axes[1] = stick(pad.ly);
    snapshot->axes[2] = stick(pad.rx);
    snapshot->axes[3] = stick(pad.ry);
    snapshot->axes[4] = trigger(pad.l2);
    snapshot->axes[5] = trigger(pad.r2);
    return true;
}

void linuxSdlInputKeyboardRequest(bool show) {
    if (show) diagnosticsTrace("sdl.keyboard=UNAVAILABLE (the on-screen keyboard is not written yet)");
}
void linuxSdlInputKeyboardPump(void) {}
bool linuxSdlInputKeyboardVisible(void) { return false; }
bool linuxSdlInputKeyboardAvailable(void) { return false; }
