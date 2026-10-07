// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Two client slots (slots.c), so that a new PokeMMO revision never replaces the one that works until it has started once:
//   <root>/slots/a, <root>/slots/b   complete clients; a slot counts only with its .prospero-complete marker
//   <root>/shared/config             the player's settings, the same for both (mounted over /game/config)
//   <root>/slots/state               which slot is active, which was before, and whether the active one is on trial
// A slot switched to is "on trial" until the game shows its first picture; a start that finds a trial still open knows the
// previous start failed, and goes back to the previous slot.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char active, previous;  // 'a', 'b' or 0
    bool trial;
    char failed[32];        // a revision that did not start: not offered again
} SlotState;

void slotsInit(const char *root);
bool slotsLoad(SlotState *state);
bool slotsSave(const SlotState *state);
// The old single-folder layout (<root>/game) becomes slot a, its config/ the shared settings. True when nothing was left to do.
bool slotsMigrate(void);
void slotsPath(char slot, char *out, size_t size);  // <root>/slots/<slot>
const char *slotsSharedConfig(void);                // <root>/shared/config
bool slotsRevision(char slot, char *out, size_t size);  // the revision of a complete slot
bool slotsClear(char slot);                             // empties it (and drops its marker) for a new client
bool slotsRemove(char slot);                            // deletes it (a confirmed update no longer needs the old client)
bool slotsMarkComplete(char slot, const char *revision);
char slotsOther(char slot);
