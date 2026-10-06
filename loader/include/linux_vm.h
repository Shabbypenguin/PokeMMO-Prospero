// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_vm.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the backing comes from the platform layer in 64 KiB blocks; private file mappings are supported.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    LINUX_PROT_NONE = 0,
    LINUX_PROT_READ = 1,
    LINUX_PROT_WRITE = 2,
    LINUX_PROT_EXEC = 4,
    LINUX_MAP_SHARED = 1,
    LINUX_MAP_PRIVATE = 2,
    LINUX_MAP_FIXED = 0x10,
    LINUX_MAP_ANONYMOUS = 0x20,
    LINUX_MAP_NORESERVE = 0x4000,
    LINUX_MAP_STACK = 0x20000
};
#define LINUX_MAP_FAILED ((void *)(intptr_t)-1)
#define LINUX_VM_PAGE 4096u
#define LINUX_VM_CHUNK (2u * 1024u * 1024u)
// Address space a guest may reserve in one mapping (GraalVM asks for 32 GiB for its heap). When the system cannot reserve that
// much, the reservation is clipped down to LINUX_VM_MIN_WINDOW at least: the guest only ever uses the start of it.
#define LINUX_VM_MAX_RESERVATION ((size_t)40 << 30)
#define LINUX_VM_MIN_WINDOW ((size_t)1 << 30)
typedef struct {
    size_t arenas, active_pages, backed_blocks, backing_bytes;
} LinuxVmStats;

void *linuxAbiMmap(void *address, size_t bytes, int protection, int flags, int fd, int64_t offset);
int linuxAbiMprotect(void *address, size_t bytes, int protection);
int linuxAbiMunmap(void *address, size_t bytes);
int linuxAbiMsync(void *address, size_t bytes, int flags);
int linuxAbiMadvise(void *address, size_t bytes, int advice);
// Writes the changed pages of every shared writable file mapping back to its file (also done by msync and munmap).
int linuxVmFlushFileMappings(void);
bool linuxVmReleaseAll(void);
LinuxVmStats linuxVmStats(void);
