// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Which game ROMs are in the ROM folder (roms.c), read from their headers (the game code PokeMMO itself logs, e.g. "IRBO",
// "BPRE"), before the game starts: PokeMMO needs Pokémon Black or White; FireRed, Emerald, Platinum and HeartGold/SoulSilver
// add regions.
#pragma once
#include <stdbool.h>

enum { ROM_BLACK_WHITE, ROM_FIRERED, ROM_EMERALD, ROM_PLATINUM, ROM_HGSS, ROM_GAMES };
#define ROM_FILES_MAX 24
typedef struct {
    char file[128];
    char code[5];     // the header's game code, "" when none could be read
    unsigned version;
    int game;         // ROM_* or -1: not one PokeMMO uses
    char note[96];    // what it is, or why it is not used
} RomFile;
typedef struct {
    bool listed;                // the folder could be read
    RomFile files[ROM_FILES_MAX];
    unsigned count, more;       // files shown, and how many more there were
    int found[ROM_GAMES];       // index into files, -1 when missing
} RomScan;

void romsScan(const char *folder, RomScan *scan);
const char *romGameName(int game);  // "Black / White", ...
bool romGameRequired(int game);
unsigned romGamesFound(const RomScan *scan);
