// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/elf_imports.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: none.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name, *version, *library;
    unsigned type, binding;
} ElfImport;
typedef bool (*ElfImportResolver)(void *context, const ElfImport *symbol, uintptr_t *address);
