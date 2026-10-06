// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/diagnostics.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
#pragma once
#include <stdarg.h>
#include <stdbool.h>

// The diagnostics log. Every line goes to the platform log (PS5: UDP + /app0/prospero.log; PC: stderr or $PROSPERO_LOG).
void diagnosticsSetEnabled(bool enabled);
bool diagnosticsEnabled(void);
void diagnosticsTrace(const char *format, ...) __attribute__((format(printf, 1, 2)));  // one line; the newline is added
void diagnosticsTraceV(const char *format, va_list args);
