// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// ROM identification (see roms.h). GBA: game code at 0xAC, version at 0xBC, fixed value 0x96 at 0xB2. NDS: game code at 0x0C,
// version at 0x1E. The last letter of a code is the region (E USA, P Europe, O USA and Europe, J Japan, ...). Revision 32920
// of the client loaded FireRed 1.1 (BPRE), Emerald (BPEE), Black (IRBO), HeartGold (IPKE) and Platinum (CPUE) on a PS5.
#include "roms.h"
#include "platform.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *const names[ROM_GAMES] = {"Black / White", "FireRed", "Emerald", "Platinum", "HeartGold / SoulSilver"};
const char *romGameName(int game) { return game >= 0 && game < ROM_GAMES ? names[game] : "?"; }
bool romGameRequired(int game) { return game == ROM_BLACK_WHITE; }
unsigned romGamesFound(const RomScan *scan) {
    unsigned found = 0;
    for (int i = 0; i < ROM_GAMES; ++i) found += scan->found[i] >= 0;
    return found;
}

static const struct {
    const char *prefix;  // first three letters of the game code
    int game;
    const char *what;
} known[] = {
    {"IRB", ROM_BLACK_WHITE, "Pokemon Black"},     {"IRA", ROM_BLACK_WHITE, "Pokemon White"},  {"BPR", ROM_FIRERED, "Pokemon FireRed"},
    {"BPE", ROM_EMERALD, "Pokemon Emerald"},       {"CPU", ROM_PLATINUM, "Pokemon Platinum"},  {"IPK", ROM_HGSS, "Pokemon HeartGold"},
    {"IPG", ROM_HGSS, "Pokemon SoulSilver"},       {"IRE", -1, "Black 2: PokeMMO uses Black / White 1"},
    {"IRD", -1, "White 2: PokeMMO uses Black / White 1"}, {"BPG", -1, "LeafGreen: PokeMMO uses FireRed"},
    {"AXV", -1, "Ruby: PokeMMO uses Emerald"},     {"AXP", -1, "Sapphire: PokeMMO uses Emerald"},
    {"ADA", -1, "Diamond: PokeMMO uses Platinum"}, {"APA", -1, "Pearl: PokeMMO uses Platinum"},
};
static const char *region(char letter) {
    switch (letter) {
        case 'E': return "USA";
        case 'P': return "Europe";
        case 'O': return "USA/Europe";
        case 'J': return "Japan";
        case 'D': return "Germany";
        case 'F': return "France";
        case 'I': return "Italy";
        case 'S': return "Spain";
        case 'K': return "Korea";
        default: return "other region";
    }
}
static bool endsWith(const char *name, const char *suffix) {
    size_t a = strlen(name), b = strlen(suffix);
    return a >= b && !strcasecmp(name + a - b, suffix);
}
static void identify(const char *folder, RomFile *f) {
    f->game = -1;
    if (endsWith(f->file, ".zip") || endsWith(f->file, ".7z") || endsWith(f->file, ".rar")) {
        snprintf(f->note, sizeof(f->note), "compressed: unpack the .nds/.gba file first");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", folder, f->file);
    unsigned char header[0xC0];
    int fd = open(path, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, header, sizeof(header)) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof(header)) {
        snprintf(f->note, sizeof(f->note), fd < 0 ? "cannot be opened" : "too small to be a game");
        return;
    }
    bool gba = header[0xB2] == 0x96 && !endsWith(f->file, ".nds");
    const unsigned char *code = gba ? header + 0xAC : header + 0x0C;
    for (int i = 0; i < 4; ++i)
        if (code[i] < 'A' || code[i] > 'Z') {
            if (code[i] < '0' || code[i] > '9') {
                snprintf(f->note, sizeof(f->note), "not a GBA or DS game");
                return;
            }
        }
    char game_code[5] = {(char)code[0], (char)code[1], (char)code[2], (char)code[3], 0};
    memcpy(f->code, game_code, 5);
    f->version = gba ? header[0xBC] : header[0x1E];
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (!strncmp(game_code, known[i].prefix, 3)) {
            f->game = known[i].game;
            if (f->game >= 0)
                snprintf(f->note, sizeof(f->note), "%s, %s (%s v%u)", known[i].what, region(game_code[3]), game_code, f->version);
            else
                snprintf(f->note, sizeof(f->note), "%s (%s)", known[i].what, game_code);
            return;
        }
    snprintf(f->note, sizeof(f->note), "%s: not a game PokeMMO uses", game_code);
}

void romsScan(const char *folder, RomScan *scan) {
    memset(scan, 0, sizeof(*scan));
    for (int i = 0; i < ROM_GAMES; ++i) scan->found[i] = -1;
    int error = 0;
    PlatformDirectory *directory = platformDirectoryOpen(folder, &error);
    if (!directory) return;
    scan->listed = true;
    char name[256];
    uint8_t type;
    uint64_t inode;
    while (platformDirectoryRead(directory, name, &type, &inode, &error) == 1) {
        if (name[0] == '.' || type == 4 /* folder */) continue;
        if (scan->count >= ROM_FILES_MAX) {
            ++scan->more;
            continue;
        }
        RomFile *f = &scan->files[scan->count];
        snprintf(f->file, sizeof(f->file), "%s", name);
        identify(folder, f);
        if (f->game >= 0 && scan->found[f->game] < 0) scan->found[f->game] = (int)scan->count;
        ++scan->count;
    }
    platformDirectoryClose(directory);
}
