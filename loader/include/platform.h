// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The platform layer: everything the Linux ABI adapters need from the system they run on, and nothing else.
// Two implementations exist:
//   platform/ps5/   the PS5 title (FreeBSD-based libkernel, direct memory, sceNet resolver, UDP log)
//   platform/host/  a Linux build of the same loader, so the adapters can be run against the real client on a PC
// Adapters never call system memory, DNS or logging functions directly. Plain POSIX calls (open, read, stat, pthread,
// sockets) are used directly: both systems provide them, and every constant and structure is translated by name.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- identity -----------------------------------------------------------------------------------------------------------------
const char *platformName(void);  // "ps5" or "host"

// ---- logging ----------------------------------------------------------------------------------------------------------------
// One line, without its newline. Thread-safe; never allocates; may be called before anything else is initialized.
void platformLogLine(const char *line);

// ---- time and randomness ------------------------------------------------------------------------------------------------------
uint64_t platformMonotonicNs(void);
void platformSleepNs(uint64_t nanoseconds);
void platformYield(void);
void platformRandom(void *buffer, size_t bytes);

// ---- memory ---------------------------------------------------------------------------------------------------------------------
// The system page (16 KiB on the PS5, 4 KiB on a PC). Protections apply to whole system pages.
size_t platformPageSize(void);
enum { PLATFORM_PROT_NONE = 0, PLATFORM_PROT_READ = 1, PLATFORM_PROT_WRITE = 2, PLATFORM_PROT_EXEC = 4 };
// Address space only, nothing behind it. bytes and the returned address are multiples of PLATFORM_VM_BLOCK.
#define PLATFORM_VM_BLOCK ((size_t)64 << 10)
int platformReserve(size_t bytes, void **address);  // 0 or a Linux errno
int platformUnreserve(void *address, size_t bytes);
// Backs [address, address+bytes) of a reservation with memory, readable and writable. Whole blocks only. The content is
// NOT guaranteed to be zero. kind: data (PS5: direct memory) or code (PS5: flexible memory, the only kind that may become
// executable).
enum { PLATFORM_MEMORY_DATA = 0, PLATFORM_MEMORY_CODE = 1 };
int platformCommit(void *address, size_t bytes, int kind);
int platformDecommit(void *address, size_t bytes);  // back to reserved, inaccessible address space
int platformProtect(void *address, size_t bytes, int protection);  // whole system pages
// Stand-alone memory for the loader's own use (JIT pages): page-aligned, readable and writable, may later become executable.
void *platformAllocatePages(size_t bytes);
void platformFreePages(void *address, size_t bytes);

// ---- threads --------------------------------------------------------------------------------------------------------------------
// The calling thread's stack: lowest address and size (the guard is excluded when known).
bool platformThreadStack(void **low, size_t *bytes);
unsigned platformCpuCount(void);

// ---- files ------------------------------------------------------------------------------------------------------------------------
// Directory listing without the C library's opendir (refused inside PS5 titles). Names exclude "." and "..".
typedef struct PlatformDirectory PlatformDirectory;
PlatformDirectory *platformDirectoryOpen(const char *path, int *error);  // error: native errno
// 1 entry, 0 end, -1 error (native errno in *error). type: 4 directory, 8 regular file, 10 link, 0 unknown (Linux DT_*).
int platformDirectoryRead(PlatformDirectory *directory, char name[256], uint8_t *type, uint64_t *inode, int *error);
void platformDirectoryClose(PlatformDirectory *directory);

// ---- network ----------------------------------------------------------------------------------------------------------------------
// Name to IPv4 address (network order). 0, or a negative glibc EAI_* value.
int platformResolveIPv4(const char *name, uint32_t *address);

// ---- end ----------------------------------------------------------------------------------------------------------------------
void platformFatal(const char *message) __attribute__((noreturn));
