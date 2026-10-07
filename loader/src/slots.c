// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Client slots (see slots.h).
#include "slots.h"
#include "diagnostics.h"
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[256], shared_config[300];

void slotsInit(const char *folder) {
    snprintf(root, sizeof(root), "%s", folder);
    snprintf(shared_config, sizeof(shared_config), "%s/shared/config", root);
}
const char *slotsSharedConfig(void) { return shared_config; }
char slotsOther(char slot) { return slot == 'a' ? 'b' : 'a'; }
void slotsPath(char slot, char *out, size_t size) { snprintf(out, size, "%s/slots/%c", root, slot ? slot : 'a'); }

static bool readText(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t got = read(fd, out, size - 1);
    close(fd);
    out[got > 0 ? got : 0] = 0;
    return got > 0;
}
static bool writeText(const char *path, const char *text) {
    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    if (!ok || rename(temporary, path)) {
        unlink(temporary);
        return false;
    }
    return true;
}
static void makeFolders(const char *path) {
    char partial[512];
    snprintf(partial, sizeof(partial), "%s", path);
    for (char *slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(partial, 0755);
        *slash = '/';
    }
    mkdir(partial, 0755);
}

bool slotsLoad(SlotState *state) {
    memset(state, 0, sizeof(*state));
    char path[300], text[256];
    snprintf(path, sizeof(path), "%s/slots/state", root);
    if (!readText(path, text, sizeof(text))) return false;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "active=", 7)) state->active = line[7] == 'a' || line[7] == 'b' ? line[7] : 0;
        if (!strncmp(line, "previous=", 9)) state->previous = line[9] == 'a' || line[9] == 'b' ? line[9] : 0;
        if (!strncmp(line, "trial=", 6)) state->trial = line[6] == '1';
        if (!strncmp(line, "failed=", 7)) snprintf(state->failed, sizeof(state->failed), "%s", line + 7);
    }
    return state->active != 0;
}
bool slotsSave(const SlotState *state) {
    char path[300], text[160];
    snprintf(path, sizeof(path), "%s/slots", root);
    makeFolders(path);
    snprintf(path, sizeof(path), "%s/slots/state", root);
    snprintf(text, sizeof(text), "active=%c\nprevious=%c\ntrial=%d\nfailed=%s\n", state->active ? state->active : '-', state->previous ? state->previous : '-',
             state->trial ? 1 : 0, state->failed);
    bool ok = writeText(path, text);
    diagnosticsTrace("slots: active=%c previous=%c trial=%d failed=%s%s", state->active ? state->active : '-', state->previous ? state->previous : '-',
                     state->trial, state->failed[0] ? state->failed : "-", ok ? "" : " (NOT SAVED)");
    return ok;
}

bool slotsRevision(char slot, char *out, size_t size) {
    char path[320], text[64];
    slotsPath(slot, path, sizeof(path));
    strncat(path, "/.prospero-complete", sizeof(path) - strlen(path) - 1);
    if (!readText(path, text, sizeof(text))) return false;
    text[strcspn(text, "\r\n")] = 0;
    snprintf(out, size, "%s", text);
    return text[0] != 0;
}
bool slotsMarkComplete(char slot, const char *revision) {
    char path[320], text[64];
    slotsPath(slot, path, sizeof(path));
    strncat(path, "/.prospero-complete", sizeof(path) - strlen(path) - 1);
    snprintf(text, sizeof(text), "%s\n", revision);
    return writeText(path, text);
}

static bool removeTree(const char *path, unsigned depth) {
    int error = 0;
    PlatformDirectory *directory = depth < 16 ? platformDirectoryOpen(path, &error) : NULL;
    if (!directory) return unlink(path) == 0 || errno == ENOENT;
    char name[256], child[1024];
    uint8_t type;
    uint64_t inode;
    bool ok = true;
    while (platformDirectoryRead(directory, name, &type, &inode, &error) == 1) {
        snprintf(child, sizeof(child), "%s/%s", path, name);
        if (type == 4)
            ok = removeTree(child, depth + 1) && ok;
        else if (unlink(child) && errno != ENOENT)
            ok = false;
    }
    platformDirectoryClose(directory);
    return (rmdir(path) == 0 || errno == ENOENT) && ok;
}
bool slotsClear(char slot) {
    char path[300];
    slotsPath(slot, path, sizeof(path));
    bool ok = removeTree(path, 0);
    makeFolders(path);
    diagnosticsTrace("slots: slot %c cleared%s", slot, ok ? "" : " (some files stayed)");
    return true;  // leftovers are overwritten by the new client
}

bool slotsRemove(char slot) {
    char path[300];
    slotsPath(slot, path, sizeof(path));
    bool ok = removeTree(path, 0);
    diagnosticsTrace("slots: slot %c removed%s", slot, ok ? "" : " (some files stayed)");
    return ok;
}

bool slotsMigrate(void) {
    char old_game[300], slot_a[300], state_path[300], text[64] = "";
    snprintf(old_game, sizeof(old_game), "%s/game", root);
    snprintf(state_path, sizeof(state_path), "%s/slots/state", root);
    struct stat info;
    makeFolders(shared_config);
    if (stat(old_game, &info) || !S_ISDIR(info.st_mode)) return true;
    if (!stat(state_path, &info)) return true;  // already slotted (a stray old folder is left alone)
    snprintf(slot_a, sizeof(slot_a), "%s/slots", root);
    makeFolders(slot_a);
    slotsPath('a', slot_a, sizeof(slot_a));
    removeTree(slot_a, 0);
    if (rename(old_game, slot_a)) {
        diagnosticsTrace("slots: moving %s to %s failed errno=%d", old_game, slot_a, errno);
        return false;
    }
    // The settings move out of the slot into the shared folder (they are mounted back over /game/config).
    char old_config[320];
    snprintf(old_config, sizeof(old_config), "%s/config", slot_a);
    if (!stat(old_config, &info)) {
        removeTree(shared_config, 0);
        if (rename(old_config, shared_config)) diagnosticsTrace("slots: moving the settings failed errno=%d", errno);
    }
    makeFolders(shared_config);
    char revision_path[320];
    snprintf(revision_path, sizeof(revision_path), "%s/revision.txt", slot_a);
    SlotState state = {.active = 'a'};
    if (readText(revision_path, text, sizeof(text))) {
        text[strcspn(text, "\r\n")] = 0;
        slotsMarkComplete('a', text);
    }
    diagnosticsTrace("slots: the single client folder became slot a (revision %s)", text[0] ? text : "?");
    return slotsSave(&state);
}
