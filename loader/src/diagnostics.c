// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/diagnostics.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Change: lines go to the platform log instead of a FILE on the SD card.
#include "diagnostics.h"
#include "platform.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_bool enabled = true;

void diagnosticsSetEnabled(bool value) { atomic_store_explicit(&enabled, value, memory_order_release); }
bool diagnosticsEnabled(void) { return atomic_load_explicit(&enabled, memory_order_acquire); }

void diagnosticsTraceV(const char *format, va_list args) {
    if (!diagnosticsEnabled()) return;
    char line[1024];
    vsnprintf(line, sizeof(line), format, args);
    platformLogLine(line);
}

void diagnosticsTrace(const char *format, ...) {
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
