// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/elf_executable.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: mapped through the platform layer (reserve, commit, stage in place, protect) instead of Horizon code memory.
#pragma once
#include "elf_layout.h"
#include "elf_imports.h"
#include "elf_image.h"

typedef struct {
    char name[ELF_EXPORT_NAME_MAX];
    uintptr_t address;
    unsigned type;
} ElfExecutableExport;  // runtime address of an exported symbol

typedef struct {
    ElfLayout layout;
    void *base;                 // runtime address of layout.minimum_vaddr
    void *reservation;          // what the platform reserved (base lies inside it)
    size_t reservation_bytes;
    uintptr_t main_address;
    uint32_t relocated, unresolved, unsupported, resolved, weak_null;
    bool verified;
    ElfExecutableExport exports[ELF_EXPORT_MAX];
    unsigned export_count, export_overflow;
    // Shared libraries: runtime addresses of the dynamic symbol table and initializers, and DT_NEEDED names.
    uintptr_t symtab, strtab, init, init_array;
    uint32_t symbol_count, strtab_bytes;
    unsigned init_array_count;
    char needed[ELF_NEEDED_MAX][ELF_NEEDED_NAME_MAX];
    unsigned needed_count;
} ElfExecutable;

// Runtime address of an exported symbol of the mapped image, or 0 when it has none.
uintptr_t elfExecutableFindExport(const ElfExecutable *mapped, const char *name);

// Mapping does not call entry points, constructors or unresolved imports. On failure, close the returned object.
bool elfExecutableOpenResolved(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context);
// A shared library: no main(); tls_module is its module number for __tls_get_addr (0 when it has no PT_TLS segment).
bool elfExecutableOpenLibrary(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, size_t tls_module);
bool elfExecutableClose(ElfExecutable *mapped);
