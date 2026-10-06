// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_vm.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: new backend. Arenas, sparse page metadata, clipped reservations and the shared-file write-back (page hashing) follow
// PokeMMO-NX; the memory behind an arena is committed through the platform layer in 64 KiB blocks (PS5: direct memory, which is
// plentiful, instead of the few hundred MiB of flexible memory), and private file mappings (GraalVM maps its image heap that way)
// are filled from the file.
//
// Model. The guest sees 4 KiB pages. Each page of an arena has a state byte: owned by the guest or not (unmapped), its
// protection, and "fresh" (must read as zero the next time it is made accessible). A 64 KiB block of the arena is backed by
// memory as soon as one of its pages is accessible, and stays backed (so PROT_NONE keeps the contents, as on Linux) until every
// page of it is unmapped or fresh again. A system page (16 KiB on the PS5) gets the union of the protections of its guest pages.
#include "linux_vm.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include "linux_files.h"
#include "platform.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PAGE LINUX_VM_PAGE
#define CHUNK_PAGES (LINUX_VM_CHUNK / PAGE)
#define BLOCK PLATFORM_VM_BLOCK
#define BLOCK_PAGES (BLOCK / PAGE)
#define MAX_ARENAS 256u
enum { OWNED = 0x80, FRESH = 0x40, PROT_MASK = 7 };
// A missing chunk holds default pages: owned, fresh, no access (a new reservation). HOLE: a chunk the guest unmapped entirely.
static uint8_t hole_chunk[CHUNK_PAGES];
#define HOLE (hole_chunk)
#define DEFAULT_PAGE (OWNED | FRESH)

typedef struct {
    unsigned char *base;
    size_t bytes;   // what the guest was given
    size_t window;  // the start of it that is really reserved (smaller only for a clipped reservation)
    uint8_t **chunks;
    uint8_t *backed;  // one bit per block of the window
    size_t active, backed_blocks;
} Arena;
static Arena arenas[MAX_ARENAS];
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t backing_bytes;

static int failure(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}
static bool rounded(size_t bytes, size_t *result) {
    if (!bytes || bytes > SIZE_MAX - (PAGE - 1)) return false;
    *result = (bytes + PAGE - 1) & ~(size_t)(PAGE - 1);
    return true;
}
static int nativeProtection(int protection) {
    return (protection & LINUX_PROT_READ ? PLATFORM_PROT_READ : 0) | (protection & LINUX_PROT_WRITE ? PLATFORM_PROT_WRITE : 0) |
           (protection & LINUX_PROT_EXEC ? PLATFORM_PROT_EXEC : 0);
}
static int protectionError(int protection) {
    if (protection & ~(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) return LINUX_EINVAL;
    if (protection & LINUX_PROT_EXEC) return LINUX_ENOSYS;  // executable guest memory: only through the JIT pool (linux_jit.c)
    return 0;
}

// ---- page metadata -------------------------------------------------------------------------------------------------------------------
static size_t pageCount(const Arena *arena) { return arena->bytes / PAGE; }
static uint8_t pageRead(const Arena *arena, size_t index) {
    const uint8_t *chunk = arena->chunks[index / CHUNK_PAGES];
    return chunk ? chunk[index % CHUNK_PAGES] : DEFAULT_PAGE;
}
// Makes the chunks covering [first, end) private so their pages can be written. 0 or ENOMEM.
static int materialize(Arena *arena, size_t first, size_t end) {
    for (size_t chunk = first / CHUNK_PAGES; chunk <= (end - 1) / CHUNK_PAGES; ++chunk) {
        uint8_t *current = arena->chunks[chunk];
        if (current && current != HOLE) continue;
        uint8_t *fresh = malloc(CHUNK_PAGES);
        if (!fresh) return LINUX_ENOMEM;
        memset(fresh, current == HOLE ? 0 : DEFAULT_PAGE, CHUNK_PAGES);
        arena->chunks[chunk] = fresh;
    }
    return 0;
}
static uint8_t *pageRef(Arena *arena, size_t index) { return &arena->chunks[index / CHUNK_PAGES][index % CHUNK_PAGES]; }
// After a change, a chunk whose pages are all unmapped becomes HOLE again (saves memory for large unmaps).
static void compact(Arena *arena, size_t first, size_t end) {
    for (size_t chunk = first / CHUNK_PAGES; chunk <= (end - 1) / CHUNK_PAGES; ++chunk) {
        uint8_t *pages = arena->chunks[chunk];
        if (!pages || pages == HOLE) continue;
        size_t count = chunk == (pageCount(arena) - 1) / CHUNK_PAGES ? pageCount(arena) - chunk * CHUNK_PAGES : CHUNK_PAGES;
        bool all_holes = true, all_default = true;
        for (size_t i = 0; i < count && (all_holes || all_default); ++i) {
            if (pages[i]) all_holes = false;
            if (pages[i] != DEFAULT_PAGE) all_default = false;
        }
        if (all_holes || all_default) {
            free(pages);
            arena->chunks[chunk] = all_holes ? HOLE : NULL;
        }
    }
}
static bool backed(const Arena *arena, size_t block) { return block * BLOCK < arena->window && (arena->backed[block / 8] >> (block % 8)) & 1; }
static void setBacked(Arena *arena, size_t block, bool value) {
    if (value)
        arena->backed[block / 8] |= (uint8_t)(1u << (block % 8));
    else
        arena->backed[block / 8] &= (uint8_t)~(1u << (block % 8));
}

// ---- applying the page states to the system ------------------------------------------------------------------------------------------
// Brings every block touching [first, end) in line with its pages: backs blocks that hold an accessible page, zeroes fresh pages
// that become accessible, releases blocks with nothing left to keep, and sets the protection of each system page.
static int apply(Arena *arena, size_t first, size_t end) {
    size_t system_page = platformPageSize();
    size_t first_block = first / BLOCK_PAGES, end_block = (end + BLOCK_PAGES - 1) / BLOCK_PAGES;
    for (size_t block = first_block; block < end_block; ++block) {
        size_t low = block * BLOCK_PAGES, high = low + BLOCK_PAGES;
        if (high > pageCount(arena)) high = pageCount(arena);
        bool accessible = false, keep = false;
        for (size_t i = low; i < high; ++i) {
            uint8_t page = pageRead(arena, i);
            if ((page & OWNED) && (page & PROT_MASK)) accessible = true;
            if ((page & OWNED) && !(page & FRESH)) keep = true;
        }
        unsigned char *address = arena->base + block * BLOCK;
        if (accessible && block * BLOCK >= arena->window) return LINUX_ENOMEM;  // beyond what a clipped reservation really has
        if (accessible && !backed(arena, block)) {
            int error = platformCommit(address, BLOCK, PLATFORM_MEMORY_DATA);
            if (error) {
                diagnosticsTrace("vm.commit=FAIL address=%p error=%d backing_mib=%zu", address, error, backing_bytes >> 20);
                return LINUX_ENOMEM;
            }
            memset(address, 0, BLOCK);  // new memory is not guaranteed to be zero; every page of the block is now clean
            int materialized = materialize(arena, low, high);
            if (materialized) return materialized;
            for (size_t i = low; i < high; ++i) *pageRef(arena, i) &= (uint8_t)~FRESH;
            setBacked(arena, block, true);
            ++arena->backed_blocks;
            backing_bytes += BLOCK;
        } else if (!accessible && !keep && backed(arena, block)) {
            int error = platformDecommit(address, BLOCK);
            if (error) diagnosticsTrace("vm.decommit=FAIL address=%p error=%d", address, error);
            if (!error) {
                setBacked(arena, block, false);
                --arena->backed_blocks;
                backing_bytes -= BLOCK;
            }
            continue;
        }
        if (!backed(arena, block)) continue;
        // Fresh pages that are accessible must read as zero now.
        for (size_t i = low; i < high; ++i) {
            uint8_t page = pageRead(arena, i);
            if ((page & OWNED) && (page & FRESH) && (page & PROT_MASK)) {
                // The system page may be inaccessible right now: open it for writing first.
                size_t offset = i * PAGE;
                unsigned char *system_low = arena->base + offset / system_page * system_page;
                platformProtect(system_low, system_page, PLATFORM_PROT_READ | PLATFORM_PROT_WRITE);
                memset(arena->base + offset, 0, PAGE);
                int materialized = materialize(arena, i, i + 1);
                if (materialized) return materialized;
                *pageRef(arena, i) &= (uint8_t)~FRESH;
            }
        }
        // System page protections: the union of the guest pages on each system page.
        for (size_t offset = block * BLOCK; offset < block * BLOCK + BLOCK; offset += system_page) {
            int protection = 0;
            for (size_t i = offset / PAGE; i < (offset + system_page) / PAGE && i < pageCount(arena); ++i) {
                uint8_t page = pageRead(arena, i);
                if (page & OWNED) protection |= page & PROT_MASK;
            }
            int error = platformProtect(arena->base + offset, system_page, nativeProtection(protection));
            if (error) {
                diagnosticsTrace("vm.protect=FAIL address=%p protection=%d error=%d", arena->base + offset, protection, error);
                return LINUX_ENOMEM;
            }
        }
    }
    return 0;
}

// ---- arenas -------------------------------------------------------------------------------------------------------------------------
// The real address space of an arena goes first: another arena may sit in the unreserved rest of a clipped one.
static Arena *findArena(const void *address, size_t bytes) {
    uintptr_t low = (uintptr_t)address;
    for (int pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < MAX_ARENAS; ++i) {
            Arena *arena = &arenas[i];
            if (!arena->chunks) continue;
            uintptr_t start = (uintptr_t)arena->base, limit = start + (pass == 0 ? arena->window : arena->bytes);
            if (low >= start && low < limit && bytes <= limit - low) return arena;
        }
    return NULL;
}
static void releaseArena(Arena *arena) {
    for (size_t i = 0; i < (pageCount(arena) + CHUNK_PAGES - 1) / CHUNK_PAGES; ++i)
        if (arena->chunks[i] && arena->chunks[i] != HOLE) free(arena->chunks[i]);
    platformUnreserve(arena->base, arena->window);
    backing_bytes -= arena->backed_blocks * BLOCK;
    free(arena->chunks);
    free(arena->backed);
    memset(arena, 0, sizeof(*arena));
}
static bool allUnmapped(const Arena *arena) {
    for (size_t i = 0; i < (pageCount(arena) + CHUNK_PAGES - 1) / CHUNK_PAGES; ++i)
        if (arena->chunks[i] != HOLE) return false;
    return true;
}
static int createArena(size_t size, Arena **result) {
    Arena *arena = NULL;
    for (unsigned i = 0; i < MAX_ARENAS && !arena; ++i)
        if (!arenas[i].chunks) arena = &arenas[i];
    if (!arena) return LINUX_ENOMEM;
    size_t reserve = (size + BLOCK - 1) / BLOCK * BLOCK;
    void *base = NULL;
    int error = platformReserve(reserve, &base);
    while (error && reserve > LINUX_VM_MIN_WINDOW) {
        reserve = reserve / 2 < LINUX_VM_MIN_WINDOW ? LINUX_VM_MIN_WINDOW : reserve / 2;
        error = platformReserve(reserve, &base);
    }
    if (error) return LINUX_ENOMEM;
    size_t chunk_count = (size / PAGE + CHUNK_PAGES - 1) / CHUNK_PAGES;
    uint8_t **chunks = calloc(chunk_count, sizeof(*chunks));
    uint8_t *backed_bits = calloc((reserve / BLOCK + 7) / 8 + 1, 1);
    if (!chunks || !backed_bits) {
        free(chunks);
        free(backed_bits);
        platformUnreserve(base, reserve);
        return LINUX_ENOMEM;
    }
    *arena = (Arena){.base = base, .bytes = size, .window = reserve, .chunks = chunks, .backed = backed_bits, .active = size / PAGE};
    if (reserve < size) diagnosticsTrace("vm.reservation=CLIPPED requested_mib=%zu window_mib=%zu base=%p", size >> 20, reserve >> 20, base);
    *result = arena;
    return 0;
}

// Sets [first, end) to `state` (owned or not, fresh, protection) and applies it.
static int setPages(Arena *arena, size_t first, size_t end, uint8_t state) {
    int error = materialize(arena, first, end);
    if (error) return error;
    for (size_t i = first; i < end; ++i) {
        uint8_t *page = pageRef(arena, i);
        if ((*page & OWNED) && !(state & OWNED)) --arena->active;
        if (!(*page & OWNED) && (state & OWNED)) ++arena->active;
        *page = state;
    }
    error = apply(arena, first, end);
    compact(arena, first, end);
    return error;
}

static void *mapAnonymous(void *address, size_t bytes, int protection, int flags, int *result_error) {
    size_t size = 0;
    int error = protectionError(protection);
    if (!error && !rounded(bytes, &size)) error = LINUX_EINVAL;
    const int known = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED | LINUX_MAP_NORESERVE | LINUX_MAP_STACK;
    if (!error && ((flags & (LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS)) != (LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS) || (flags & ~known))) error = LINUX_ENOSYS;
    if (!error && size > LINUX_VM_MAX_RESERVATION) error = LINUX_ENOMEM;
    if (!error && (flags & LINUX_MAP_FIXED) && (!address || (uintptr_t)address % PAGE)) error = LINUX_EINVAL;
    if (error) {
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    pthread_mutex_lock(&vm_lock);
    Arena *arena = NULL;
    bool created = false;
    if (flags & LINUX_MAP_FIXED) {
        arena = findArena(address, size);
        if (!arena) error = LINUX_ENOSYS;  // only the guest's own reservations may be replaced
    } else {
        error = createArena(size, &arena);
        if (!error) {
            address = arena->base;
            created = true;
        }
    }
    if (!error) {
        size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / PAGE;
        // A new mapping: owned, fresh (its old contents are gone), with the requested protection. A new arena's pages already are
        // owned, fresh and inaccessible (no metadata yet), which is all a PROT_NONE reservation needs.
        if (!created || protection != LINUX_PROT_NONE) error = setPages(arena, first, first + size / PAGE, (uint8_t)(OWNED | FRESH | protection));
        if (error && created) releaseArena(arena);
    }
    pthread_mutex_unlock(&vm_lock);
    *result_error = error;
    return error ? LINUX_MAP_FAILED : address;
}

// ---- file mappings ------------------------------------------------------------------------------------------------------------------
// Shared writable file mappings are copies; the pages that changed since the last write-back are found by hashing, so no shadow copy
// of the file is needed. Written back by msync, munmap, a new mapping of the same file and at exit.
#define MAX_FILE_MAPPINGS 64u
typedef struct {
    bool used;
    unsigned char *address;
    size_t bytes, pages;
    int64_t offset;
    char path[LINUX_FILE_PATH_MAX];
    uint64_t *hashes;
} FileMapping;
static FileMapping file_mappings[MAX_FILE_MAPPINGS];
static uint64_t pageHash(const unsigned char *page) {
    uint64_t hash = 0xcbf29ce484222325ull, word;
    for (size_t i = 0; i < PAGE; i += 8) {
        memcpy(&word, page + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    return hash;
}
static int flushMapping(FileMapping *mapping, bool release) {
    int error = 0;
    size_t page = 0;
    while (page < mapping->pages && !error) {
        if (pageHash(mapping->address + page * PAGE) == mapping->hashes[page]) {
            ++page;
            continue;
        }
        size_t run = page;
        while (run < mapping->pages && pageHash(mapping->address + run * PAGE) != mapping->hashes[run]) ++run;
        size_t from = page * PAGE, to = run * PAGE;
        if (to > mapping->bytes) to = mapping->bytes;  // the tail of the last page is not part of the file
        if (to > from) error = linuxFilesWriteBack(mapping->path, mapping->offset + (int64_t)from, mapping->address + from, to - from);
        if (!error)
            for (size_t i = page; i < run; ++i) mapping->hashes[i] = pageHash(mapping->address + i * PAGE);
        page = run;
    }
    if (release) {
        free(mapping->hashes);
        memset(mapping, 0, sizeof(*mapping));
    }
    return error;
}
static int flushOverlapping(const void *address, size_t bytes, bool release) {
    int error = 0;
    for (unsigned i = 0; i < MAX_FILE_MAPPINGS; ++i) {
        FileMapping *mapping = &file_mappings[i];
        if (!mapping->used) continue;
        uintptr_t low = (uintptr_t)address, high = bytes > UINTPTR_MAX - low ? UINTPTR_MAX : low + bytes;
        uintptr_t start = (uintptr_t)mapping->address, end = start + mapping->pages * PAGE;
        if (end <= low || start >= high) continue;
        int result = flushMapping(mapping, release);
        if (result && !error) error = result;
    }
    return error;
}
int linuxVmFlushFileMappings(void) { return flushOverlapping((void *)0, (size_t)-1, false); }

// A file mapping is a copy of the file's bytes taken when it is made (later changes of the file are not seen). Private mappings
// (GraalVM's image heap, mapped at a fixed address of its heap reservation) may be written freely; shared writable ones are written
// back as above.
static void *mapFile(void *address, size_t bytes, int protection, int flags, int fd, int64_t offset, int *result_error) {
    size_t size = 0;
    int error = protectionError(protection);
    bool shared = (flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) == LINUX_MAP_SHARED;
    bool writable_shared = shared && (protection & LINUX_PROT_WRITE);
    if (!error && (flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) != LINUX_MAP_SHARED && (flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) != LINUX_MAP_PRIVATE)
        error = LINUX_EINVAL;
    if (!error && (!rounded(bytes, &size) || offset < 0 || (uint64_t)offset % PAGE)) error = LINUX_EINVAL;
    if (!error && (flags & ~(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_NORESERVE | LINUX_MAP_FIXED))) error = LINUX_ENOSYS;
    if (!error && writable_shared && (flags & LINUX_MAP_FIXED)) error = LINUX_ENOSYS;
    if (error) {
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    char path[LINUX_FILE_PATH_MAX];
    int descriptor_flags = 0;
    if (writable_shared) {
        if (!linuxFilesDescriptorInfo(fd, path, &descriptor_flags)) {
            *result_error = *linuxAbiErrnoLocation() ? *linuxAbiErrnoLocation() : LINUX_EBADF;
            return LINUX_MAP_FAILED;
        }
        if ((descriptor_flags & 3) != 2) {
            *result_error = LINUX_EACCES;  // a shared writable mapping needs a descriptor open for reading and writing
            return LINUX_MAP_FAILED;
        }
        flushOverlapping(NULL, (size_t)-1, false);  // other mappings of this file must reach the file before it is read again
    }
    void *memory = mapAnonymous(address, bytes, LINUX_PROT_READ | LINUX_PROT_WRITE,
                                LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | (flags & (LINUX_MAP_NORESERVE | LINUX_MAP_FIXED)), &error);
    if (error) {
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    size_t done = 0;
    while (done < bytes && !error) {
        size_t chunk = bytes - done < (4u << 20) ? bytes - done : (4u << 20);
        int64_t count = linuxAbiPread(fd, (unsigned char *)memory + done, chunk, offset + (int64_t)done);
        if (count < 0)
            error = *linuxAbiErrnoLocation() ? *linuxAbiErrnoLocation() : LINUX_EIO;
        else if (count == 0)
            break;  // past the end of the file: the rest stays zero, as on Linux
        else
            done += (size_t)count;
    }
    if (!error && protection != (LINUX_PROT_READ | LINUX_PROT_WRITE) && linuxAbiMprotect(memory, size, protection)) error = *linuxAbiErrnoLocation();
    FileMapping *tracked = NULL;
    if (!error && writable_shared) {
        for (unsigned i = 0; i < MAX_FILE_MAPPINGS && !tracked; ++i)
            if (!file_mappings[i].used) tracked = &file_mappings[i];
        uint64_t *hashes = tracked ? calloc(size / PAGE, sizeof(uint64_t)) : NULL;
        if (!tracked || !hashes) {
            free(hashes);
            error = LINUX_ENOMEM;
            tracked = NULL;
        } else {
            for (size_t i = 0; i < size / PAGE; ++i) hashes[i] = pageHash((unsigned char *)memory + i * PAGE);
            *tracked = (FileMapping){.used = true, .address = memory, .bytes = bytes, .pages = size / PAGE, .offset = offset, .hashes = hashes};
            strcpy(tracked->path, path);
        }
    }
    if (error) {
        if (!(flags & LINUX_MAP_FIXED)) linuxAbiMunmap(memory, size);
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    diagnosticsTrace("vm.mmap.file address=%p bytes=%zu prot=%d flags=0x%x offset=%lld read=%zu", memory, bytes, protection, flags, (long long)offset, done);
    *result_error = 0;
    return memory;
}

// ---- the guest's calls -------------------------------------------------------------------------------------------------------------
void *linuxAbiMmap(void *address, size_t bytes, int protection, int flags, int fd, int64_t offset) {
    int error = 0;
    void *result = (flags & LINUX_MAP_ANONYMOUS) ? mapAnonymous(address, bytes, protection, flags, &error)  // fd and offset are ignored
                                                 : mapFile(address, bytes, protection, flags, fd, offset, &error);
    if (error) {
        failure(error);
        diagnosticsTrace("vm.mmap=ERROR address=%p bytes=%zu prot=%d flags=0x%x fd=%d errno=%d", address, bytes, protection, flags, fd, error);
    }
    return result;
}
int linuxAbiMprotect(void *address, size_t bytes, int protection) {
    size_t size = 0;
    int error = protectionError(protection);
    if (!error && (uintptr_t)address % PAGE) error = LINUX_EINVAL;
    if (!error && !bytes) return 0;
    if (!error && !rounded(bytes, &size)) error = LINUX_EINVAL;
    if (!error) {
        pthread_mutex_lock(&vm_lock);
        Arena *arena = findArena(address, size);
        if (!arena)
            error = LINUX_ENOMEM;
        else {
            size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / PAGE, end = first + size / PAGE;
            for (size_t i = first; i < end && !error; ++i)
                if (!(pageRead(arena, i) & OWNED)) error = LINUX_ENOMEM;
            if (!error) error = materialize(arena, first, end);
            for (size_t i = first; i < end && !error; ++i) {
                uint8_t *page = pageRef(arena, i);
                *page = (uint8_t)((*page & ~PROT_MASK) | protection);
            }
            if (!error) error = apply(arena, first, end);
            if (!error) compact(arena, first, end);
        }
        pthread_mutex_unlock(&vm_lock);
    }
    if (error) diagnosticsTrace("vm.mprotect=ERROR address=%p bytes=%zu prot=%d errno=%d", address, bytes, protection, error);
    return error ? failure(error) : 0;
}
int linuxAbiMsync(void *address, size_t bytes, int flags) {
    enum { MS_ASYNC = 1, MS_INVALIDATE = 2, MS_SYNC = 4 };
    if ((uintptr_t)address % PAGE || (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) || ((flags & MS_ASYNC) && (flags & MS_SYNC))) return failure(LINUX_EINVAL);
    int error = flushOverlapping(address, bytes ? bytes : 1, false);
    return error ? failure(error) : 0;
}
int linuxAbiMunmap(void *address, size_t bytes) {
    size_t size = 0;
    int error = LINUX_EINVAL;
    if (!((uintptr_t)address % PAGE) && rounded(bytes, &size)) {
        int write_back = flushOverlapping(address, size, true);  // shared writable file mappings reach their file first
        if (write_back) diagnosticsTrace("vm.munmap.write_back=ERROR address=%p errno=%d", address, write_back);
        pthread_mutex_lock(&vm_lock);
        Arena *arena = findArena(address, size);
        error = 0;  // Linux: unmapping a range with nothing mapped is not an error
        if (arena) {
            size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / PAGE;
            error = setPages(arena, first, first + size / PAGE, 0);
            if (!error && allUnmapped(arena)) releaseArena(arena);
        }
        pthread_mutex_unlock(&vm_lock);
    }
    if (error) diagnosticsTrace("vm.munmap=ERROR address=%p bytes=%zu errno=%d", address, bytes, error);
    return error ? failure(error) : 0;
}
// madvise: DONTNEED (4) discards the contents of private anonymous pages (they read as zero afterwards); every other advice is a hint.
int linuxAbiMadvise(void *address, size_t bytes, int advice) {
    enum { MADV_DONTNEED = 4, MADV_FREE = 8 };
    if ((uintptr_t)address % PAGE) return failure(LINUX_EINVAL);
    if (advice != MADV_DONTNEED && advice != MADV_FREE) return 0;
    size_t size = 0;
    if (!rounded(bytes, &size)) return 0;
    pthread_mutex_lock(&vm_lock);
    Arena *arena = findArena(address, size);
    int error = 0;
    if (arena) {
        size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / PAGE, end = first + size / PAGE;
        error = materialize(arena, first, end);
        for (size_t i = first; i < end && !error; ++i) {
            uint8_t *page = pageRef(arena, i);
            if (*page & OWNED) *page |= FRESH;
        }
        if (!error) error = apply(arena, first, end);
        if (!error) compact(arena, first, end);
    }
    pthread_mutex_unlock(&vm_lock);
    return error ? failure(error) : 0;
}
bool linuxVmReleaseAll(void) {
    linuxVmFlushFileMappings();
    for (unsigned i = 0; i < MAX_FILE_MAPPINGS; ++i)
        if (file_mappings[i].used) {
            free(file_mappings[i].hashes);
            memset(&file_mappings[i], 0, sizeof(file_mappings[i]));
        }
    pthread_mutex_lock(&vm_lock);
    for (unsigned i = 0; i < MAX_ARENAS; ++i)
        if (arenas[i].chunks) releaseArena(&arenas[i]);
    pthread_mutex_unlock(&vm_lock);
    return true;
}
LinuxVmStats linuxVmStats(void) {
    pthread_mutex_lock(&vm_lock);
    LinuxVmStats result = {.backing_bytes = backing_bytes};
    for (unsigned i = 0; i < MAX_ARENAS; ++i)
        if (arenas[i].chunks) {
            ++result.arenas;
            result.active_pages += arenas[i].active;
            result.backed_blocks += arenas[i].backed_blocks;
        }
    pthread_mutex_unlock(&vm_lock);
    return result;
}
