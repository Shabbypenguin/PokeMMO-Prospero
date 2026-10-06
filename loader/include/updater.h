// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The client updater (updater.c): reads PokeMMO's official client zip where it is published, with HTTP range requests, and
// installs only the parts a PS5 runs (the Linux x86-64 binary and the data; about a third of the zip). Nothing of PokeMMO's is
// ever shipped with this project.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATER_URL "https://dl.pokemmo.com/download/PokeMMO-Client.zip"

typedef struct {
    char *name;
    uint32_t crc, compressed, size, offset;  // offset: the entry's local header in the zip
    uint16_t method, name_length, extra_length;
    bool executable;
} UpdaterEntry;
typedef struct {
    char etag[128];
    int64_t length;          // of the whole zip
    char revision[32];       // its revision.txt
    UpdaterEntry *entries;   // what a PS5 needs, in zip order
    unsigned count;
    uint64_t download_bytes; // what downloading them takes
} UpdaterRemote;
typedef struct {
    _Atomic uint64_t done, total;  // bytes, for a progress bar
} UpdaterProgress;

// Fills `remote` from the published zip. 1 when its ETag equals `known_etag` (nothing else is read then), 0 when read in full,
// -1 on failure (reason in `error`).
int updaterCheck(const char *url, const char *known_etag, UpdaterRemote *remote, char *error, size_t error_size);
// Downloads and unpacks every entry into `staging`, checking each one's CRC. 0 or -1.
int updaterDownload(const char *url, const UpdaterRemote *remote, const char *staging, UpdaterProgress *progress, char *error, size_t error_size);
// Moves the staged files into `game`; the player's config/ files stay; revision.txt goes last, so an interrupted update is
// redone on the next start. 0 or -1.
int updaterApply(const UpdaterRemote *remote, const char *staging, const char *game, char *error, size_t error_size);
void updaterFree(UpdaterRemote *remote);
// Revision numbers compared as numbers ("32920" < "33001"); an empty or unreadable one is older than anything.
int updaterCompareRevisions(const char *a, const char *b);
