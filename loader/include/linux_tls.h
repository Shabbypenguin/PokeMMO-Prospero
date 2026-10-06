// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// Design after PokeMMO-NX's include/linux_tls.h (Petit_Prince, MIT); the x86-64 mechanism is different and written for this port.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Thread-local storage of the shared libraries loaded at run time (x86-64, general-dynamic model).
//
// The client's libraries are built with -fPIC, so every access to a thread-local variable goes through
// __tls_get_addr({module, offset}). Each registered module gets a number; each thread gets its own copy of a module's
// block the first time it touches it (initialized from the module's image), freed when the thread ends. Nothing uses
// the thread register (%fs), which belongs to the PS5's own C library.
#define LINUX_TLS_MAX_MODULES 32u

typedef struct {
    uint64_t module, offset;
} LinuxTlsIndex;  // glibc tls_index
// Registers a module; *module receives its number (1..). The image (file_bytes) is copied. False when the table is full.
bool linuxTlsRegisterModule(const void *image, size_t file_bytes, size_t memory_bytes, size_t alignment, size_t *module);
void *linuxTlsGetAddr(const LinuxTlsIndex *index);
void linuxTlsGetAddrEntry(void);  // assembly entry bound as __tls_get_addr (realigns the stack, then linuxTlsGetAddr)
bool linuxTlsReset(void);
unsigned linuxTlsModuleCount(void);
