// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/elf_image.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: TLS module numbers instead of TLS descriptors; in-place staging.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "elf_imports.h"

// Defined global/weak function and object symbols of the dynamic symbol table. The official
// client exports about twenty, with names up to about eighty bytes.
#define ELF_EXPORT_MAX 48u
#define ELF_EXPORT_NAME_MAX 128u
typedef struct {
    char name[ELF_EXPORT_NAME_MAX];
    uint64_t vaddr;
    unsigned type;
} ElfExport;

#define ELF_NEEDED_MAX 16u
#define ELF_NEEDED_NAME_MAX 64u
// Input options. A shared library has no main() and may bring its own TLS block (module number assigned by the caller).
typedef struct {
    bool library;
    size_t tls_module;          // libraries with a PT_TLS segment: their module number for __tls_get_addr (0: none)
    unsigned char *destination;  // optional: stage in place, at the final address (must be writable, span bytes from the lowest vaddr)
} ElfStageOptions;

typedef struct {
    unsigned char *memory;
    uint64_t bytes, minimum_vaddr, main_vaddr;
    uint32_t relocated, unresolved, unsupported, resolved, weak_null;
    ElfExport exports[ELF_EXPORT_MAX];
    unsigned export_count, export_overflow;  // overflow counts exports that did not fit or had a name too long
    // Libraries: the dynamic symbol table as mapped (virtual addresses), initializers and DT_NEEDED names.
    uint64_t symtab_vaddr, strtab_vaddr, init_vaddr, init_array_vaddr, init_array_bytes;
    uint32_t symbol_count, strtab_bytes;
    char needed[ELF_NEEDED_MAX][ELF_NEEDED_NAME_MAX];
    unsigned needed_count;
    uint32_t tls_relocations;
    bool owns_memory;
} ElfImage;

// Stage PT_LOAD segments in ordinary memory, fix internal pointers and relocate the image to the address it will be mapped at (target_address:
// the runtime address of the lowest PT_LOAD byte, not a bias). Memory remains non-executable. Only imports accepted by the resolver are bound.
bool elfStageImage(const char *path, ElfImage *image, uintptr_t target_address, ElfImportResolver resolver, void *context, const ElfStageOptions *options);
void elfReleaseImage(ElfImage *image);
