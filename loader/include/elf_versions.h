// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/elf_versions.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: none.
#pragma once
#include <elf.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct {
    const char *version, *library;
} ElfSymbolVersion;
// Returned names borrow dynstr storage. Caller frees the array.
bool elfReadVersions(FILE *file, uint64_t file_bytes, const Elf64_Shdr *sections, unsigned section_count, unsigned dynsym_index, const char *strings,
                     size_t string_bytes, size_t symbol_count, ElfSymbolVersion **out);
