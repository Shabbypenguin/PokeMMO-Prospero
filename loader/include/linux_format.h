// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_format.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: x86-64 va_list.
#pragma once
#include "linux_abi.h"
#include <stdarg.h>
#define LINUX_FORMAT_MAX_OUTPUT (1024u * 1024u)
// System V x86-64 va_list (the guest's and the native one are the same); Linux LP64 integer formats.
int linuxFormatSnprintf(char *, size_t, const char *, ...);
int linuxFormatVsnprintf(char *, size_t, const char *, va_list);
int linuxFormatFprintf(void *, const char *, ...);
int linuxFormatVfprintf(void *, const char *, va_list);
