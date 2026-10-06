// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/elf_executable.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the Horizon code-memory mapping is replaced by the platform layer. The image is staged in place (the PS5 has no
// room for a staging copy of the 100 MiB client), then each system page gets the protection of the segments on it.
#include "elf_executable.h"
#include "diagnostics.h"
#include "elf_image.h"
#include "platform.h"
#include <elf.h>
#include <string.h>

bool elfExecutableClose(ElfExecutable *mapped) {
    bool ok = true;
    if (mapped->reservation) {
        int error = platformUnreserve(mapped->reservation, mapped->reservation_bytes);
        diagnosticsTrace("elf.mapping.release=%s base=%p bytes=%zu error=%d", error ? "FAIL" : "PASS", mapped->reservation, mapped->reservation_bytes,
                         error);
        ok = !error;
    }
    if (ok) memset(mapped, 0, sizeof(*mapped));
    return ok;
}

static size_t roundUp(size_t value, size_t unit) { return (value + unit - 1) / unit * unit; }

// Flags of the segments that cover [low, high) of the image (vaddr space), ORed; *covered is false when none does.
static uint32_t flagsIn(const ElfLayout *layout, uint64_t low, uint64_t high, bool *covered) {
    uint32_t flags = 0;
    *covered = false;
    for (unsigned i = 0; i < layout->count; ++i) {
        const ElfLoadSegment *segment = &layout->segments[i];
        if (segment->vaddr < high && low < segment->vaddr + segment->memory_bytes) {
            flags |= segment->flags;
            *covered = true;
        }
    }
    return flags;
}

static bool openImage(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, const ElfStageOptions *options) {
    bool library = options && options->library;
    memset(mapped, 0, sizeof(*mapped));
    ElfImage staged = {0};
    bool ok = false, main_executable = false;
    if (!elfReadLayout(path, &mapped->layout, library)) return false;
    const ElfLayout *layout = &mapped->layout;
    size_t page = platformPageSize();
    size_t span = roundUp(layout->span, PLATFORM_VM_BLOCK);
    size_t alignment = layout->alignment > PLATFORM_VM_BLOCK ? layout->alignment : PLATFORM_VM_BLOCK;
    mapped->reservation_bytes = span + (alignment > PLATFORM_VM_BLOCK ? alignment : 0);
    int error = platformReserve(mapped->reservation_bytes, &mapped->reservation);
    if (error) {
        diagnosticsTrace("elf.mapping.reserve=FAIL bytes=%zu error=%d", mapped->reservation_bytes, error);
        mapped->reservation = NULL;
        return false;
    }
    mapped->base = (void *)roundUp((uintptr_t)mapped->reservation, alignment);
    unsigned char *base = mapped->base;

    // Back the image. A block that holds code or read-only data becomes "code" memory (on the PS5 only flexible memory can be made
    // executable); a block that only holds writable data comes from the larger data pool.
    unsigned code_blocks = 0, data_blocks = 0;
    for (size_t offset = 0; offset < span;) {
        size_t run = offset;
        bool covered = false;
        uint32_t flags = flagsIn(layout, layout->minimum_vaddr + offset, layout->minimum_vaddr + offset + PLATFORM_VM_BLOCK, &covered);
        int kind = covered && !(flags & PF_W) ? PLATFORM_MEMORY_CODE : (flags & PF_X) ? PLATFORM_MEMORY_CODE : PLATFORM_MEMORY_DATA;
        // Extend the run while the next block has the same kind (fewer, larger commits).
        do {
            run += PLATFORM_VM_BLOCK;
            if (run >= span) break;
            uint32_t next = flagsIn(layout, layout->minimum_vaddr + run, layout->minimum_vaddr + run + PLATFORM_VM_BLOCK, &covered);
            int next_kind = covered && !(next & PF_W) ? PLATFORM_MEMORY_CODE : (next & PF_X) ? PLATFORM_MEMORY_CODE : PLATFORM_MEMORY_DATA;
            if (next_kind != kind) break;
        } while (true);
        error = platformCommit(base + offset, run - offset, kind);
        if (error) {
            diagnosticsTrace("elf.mapping.commit=FAIL address=%p bytes=%zu kind=%d error=%d", base + offset, run - offset, kind, error);
            goto done;
        }
        if (kind == PLATFORM_MEMORY_CODE)
            code_blocks += (unsigned)((run - offset) / PLATFORM_VM_BLOCK);
        else
            data_blocks += (unsigned)((run - offset) / PLATFORM_VM_BLOCK);
        offset = run;
    }

    uintptr_t target = (uintptr_t)base + layout->first_vaddr - layout->minimum_vaddr;
    ElfStageOptions in_place = options ? *options : (ElfStageOptions){0};
    in_place.destination = (unsigned char *)target;
    if (!elfStageImage(path, &staged, target, resolver, context, &in_place) || staged.unsupported) goto done;
    if (staged.minimum_vaddr != layout->first_vaddr) goto done;
    for (unsigned i = 0; i < layout->count; ++i) {
        const ElfLoadSegment *segment = &layout->segments[i];
        if ((segment->flags & PF_X) && staged.main_vaddr >= segment->vaddr && staged.main_vaddr - segment->vaddr < segment->file_bytes) main_executable = true;
    }
    if (!main_executable && !library) goto done;
    mapped->relocated = staged.relocated;
    mapped->unresolved = staged.unresolved;
    mapped->unsupported = staged.unsupported;
    mapped->resolved = staged.resolved;
    mapped->weak_null = staged.weak_null;
    uintptr_t bias = (uintptr_t)base - layout->minimum_vaddr;
    mapped->main_address = staged.main_vaddr ? bias + staged.main_vaddr : 0;
    if (library) {
        mapped->symtab = staged.symtab_vaddr ? bias + staged.symtab_vaddr : 0;
        mapped->strtab = staged.strtab_vaddr ? bias + staged.strtab_vaddr : 0;
        mapped->symbol_count = staged.symbol_count;
        mapped->strtab_bytes = staged.strtab_bytes;
        mapped->init = staged.init_vaddr ? bias + staged.init_vaddr : 0;
        mapped->init_array = staged.init_array_vaddr ? bias + staged.init_array_vaddr : 0;
        mapped->init_array_count = (unsigned)(staged.init_array_bytes / sizeof(uint64_t));
        memcpy(mapped->needed, staged.needed, sizeof(mapped->needed));
        mapped->needed_count = staged.needed_count;
    }
    mapped->export_count = staged.export_count;
    mapped->export_overflow = staged.export_overflow;
    for (unsigned i = 0; i < staged.export_count; ++i) {
        memcpy(mapped->exports[i].name, staged.exports[i].name, ELF_EXPORT_NAME_MAX);
        mapped->exports[i].address = bias + staged.exports[i].vaddr;
        mapped->exports[i].type = staged.exports[i].type;
    }

    // Final protections, one system page at a time: the union of the segments on the page. A page that would need to be
    // writable and executable at once cannot exist (the PS5 refuses to run such pages).
    for (size_t offset = 0; offset < span;) {
        bool covered = false;
        uint32_t flags = flagsIn(layout, layout->minimum_vaddr + offset, layout->minimum_vaddr + offset + page, &covered);
        if ((flags & PF_W) && (flags & PF_X)) {
            diagnosticsTrace("elf.mapping.page=WRITABLE_AND_EXECUTABLE offset=0x%zx", offset);
            goto done;
        }
        int protection = !covered ? PLATFORM_PROT_NONE
                                  : PLATFORM_PROT_READ | ((flags & PF_W) ? PLATFORM_PROT_WRITE : 0) | ((flags & PF_X) ? PLATFORM_PROT_EXEC : 0);
        size_t run = offset + page;
        while (run < span) {
            uint32_t next = flagsIn(layout, layout->minimum_vaddr + run, layout->minimum_vaddr + run + page, &covered);
            int next_protection =
                !covered ? PLATFORM_PROT_NONE : PLATFORM_PROT_READ | ((next & PF_W) ? PLATFORM_PROT_WRITE : 0) | ((next & PF_X) ? PLATFORM_PROT_EXEC : 0);
            if (next_protection != protection || ((next & PF_W) && (next & PF_X))) break;
            run += page;
        }
        if (protection != (PLATFORM_PROT_READ | PLATFORM_PROT_WRITE)) {
            error = platformProtect(base + offset, run - offset, protection);
            diagnosticsTrace("elf.mapping.protect=%s address=%p bytes=%zu protection=%d error=%d", error ? "FAIL" : "PASS", base + offset, run - offset,
                             protection, error);
            if (error) goto done;
        }
        offset = run;
    }
    mapped->verified = ok = true;
done:
    elfReleaseImage(&staged);
    diagnosticsTrace("elf.mapping=%s file=%s base=%p span=0x%llx code_blocks=%u data_blocks=%u relocated=%u unresolved=%u unsupported=%u main=0x%llx",
                     ok ? "PASS" : "FAIL", path, mapped->base, (unsigned long long)layout->span, code_blocks, data_blocks, mapped->relocated,
                     mapped->unresolved, mapped->unsupported, (unsigned long long)mapped->main_address);
    return ok;
}

bool elfExecutableOpenResolved(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context) {
    return openImage(path, mapped, resolver, context, NULL);
}
bool elfExecutableOpenLibrary(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, size_t tls_module) {
    ElfStageOptions options = {.library = true, .tls_module = tls_module};
    return openImage(path, mapped, resolver, context, &options);
}

uintptr_t elfExecutableFindExport(const ElfExecutable *mapped, const char *name) {
    for (unsigned i = 0; i < mapped->export_count; ++i)
        if (!strcmp(mapped->exports[i].name, name)) return mapped->exports[i].address;
    return 0;
}
