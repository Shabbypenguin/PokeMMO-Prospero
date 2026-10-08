// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// loader-39: is a newer PokeMMO Prospero published? (release_check.c) One request to GitHub's releases list at start; the
// loading screen shows a small banner when there is. Nothing is downloaded: installing it stays the player's (homebrew store).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define RELEASES_URL "https://api.github.com/repos/Shabbypenguin/PokeMMO-Prospero/releases?per_page=10"
// The newest published release's tag ("v0.2.0-beta"), pre-releases included (every release so far is a beta). False when
// GitHub could not be asked or said nothing usable.
bool releaseNewest(const char *url, char *tag, size_t size);
// Compares release numbers ("v0.1.1-beta", "0.2.0"): < 0 when a is older than b, 0 the same, > 0 newer. Numbers compare as
// numbers; a final release is newer than its pre-releases ("0.2.0" > "0.2.0-beta"); pre-release labels compare piece by piece.
int releaseCompare(const char *a, const char *b);
