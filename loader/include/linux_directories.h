// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_directories.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: none.
#pragma once
#include "linux_files.h"
#define LINUX_DIRECTORY_MAX 32u
#define LINUX_DIRECTORY_OBSERVED_MAX 128u
typedef struct {
    uint64_t inode;
    int64_t offset;
    uint16_t record_bytes;
    uint8_t type;
    char name[256];
} LinuxDirectoryEntry;
// DIR is opaque. Returned entries belong to the stream until its next read/close.
void *linuxAbiOpendir(const char *);
LinuxDirectoryEntry *linuxAbiReaddir64(void *);
int linuxAbiClosedir(void *);
int linuxAbiMkdir(const char *, unsigned);
int linuxAbiRmdir(const char *);
int linuxAbiRemove(const char *);
int linuxAbiChdir(const char *);
char *linuxAbiGetcwd(char *, size_t);
