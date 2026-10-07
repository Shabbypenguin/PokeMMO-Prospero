// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The PS5 title, milestone 1 ("loader-1"): startup self-checks, then the official client inside the loader until it logs its
// first lines or reaches a function the loader does not provide yet. Graphics, input and audio for the client come later; the
// screen only shows one tile per step (green pass, red fail, blue information, grey not run, yellow running). Everything is logged
// over UDP (port 18194, tools/udplog.py) and to /app0/prospero.log.
//
// None of the checks can take the title down: risky calls run under a fault guard, and calls that might block run on their own
// thread with a time limit.
#include "diagnostics.h"
#include "game.h"
#include "linux_audio.h"
#include "linux_gtk.h"
#include "linux_sdl.h"
#include "loading_screen.h"
#include "overlay.h"
#include "platform.h"
#include "roms.h"
#include "slots.h"
#include "updater.h"
#include "upload_server.h"
#include "prospero_version.h"  // PROSPERO_VERSION, PROSPERO_TITLE_ID (generated at build time)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#define LOADER_MILESTONE "loader-29"
// ps5-opengl's app heap (malloc of the loader and of everything the client allocates with malloc): from direct memory.
const size_t ps5_opengl_heap_size = 768u << 20;

int sceKernelUsleep(unsigned int microseconds);
int sceKernelLoadStartModule(const char *path, size_t argument_size, const void *arguments, uint32_t flags, void *options, int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceUserServiceInitialize(void *parameters);
int sceUserServiceGetInitialUser(int *user);
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}

// ---- steps shown on screen ------------------------------------------------------------------------------------------------------
enum { NOT_RUN = 0, PASS, FAIL, INFO, RUNNING };
typedef struct {
    const char *name;
    _Atomic int state;
} Step;
// loader-8: the loading screen shows one bar; these are the details behind it (hold Triangle). The storage probes of loader-1
// (title folder writes, 1 GiB in /download0) and the audio beep of loader-4 answered their questions and are gone.
// loader-15: the system-module and HTTPS probes gave way to the client updater (client.update); client.dev is the developer copy
// uploaded by the installer (--client).
static Step steps[] = {
    {"fs.list.app0", NOT_RUN}, {"fs.list.roms", NOT_RUN}, {"fs.romread", NOT_RUN},  {"fs.data", NOT_RUN},  {"client.dev", NOT_RUN},
    {"client.update", NOT_RUN}, {"client.map", NOT_RUN},  {"client.start", NOT_RUN}, {"client.end", NOT_RUN},
};
#define STEP_COUNT (sizeof(steps) / sizeof(*steps))
static void mark(const char *name, int state) {
    for (size_t i = 0; i < STEP_COUNT; ++i)
        if (!strcmp(steps[i].name, name)) atomic_store(&steps[i].state, state);
    if (state != RUNNING)
        say("RESULT %s %s", name, state == PASS ? "PASS" : state == FAIL ? "FAIL" : state == INFO ? "INFO" : "NOT_RUN");
    else
        say("BEGIN %s", name);
}

// ---- fault guard (as in the probe): a fault inside a guarded call returns here instead of ending the title -------------------------
static __thread sigjmp_buf guard_point;
static __thread volatile sig_atomic_t guard_armed;
static _Atomic int fatal_signals;
// A fault outside a guarded call (the client itself, most likely): report where, then park the faulting thread for good instead of
// ending the title, so the screen and the log stay up. Other threads keep running.
static void guardHandler(int signal_number, siginfo_t *info, void *context) {
    if (guard_armed) {
        guard_armed = 0;
        siglongjmp(guard_point, signal_number);
    }
    ucontext_t *state = context;
    char line[200];
    snprintf(line, sizeof(line), "FATAL signal %d address=%p rip=0x%llx rsp=0x%llx (thread parked)", signal_number, info ? info->si_addr : NULL,
             state ? (unsigned long long)state->uc_mcontext.mc_rip : 0ull, state ? (unsigned long long)state->uc_mcontext.mc_rsp : 0ull);
    platformLogLine(line);
    atomic_fetch_add(&fatal_signals, 1);
    for (;;) sceKernelUsleep(1000000);
}
static void guardInstall(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guardHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}
// Runs `body` (a statement block) under the guard; `faulted` receives the signal number or 0.
#define GUARDED(faulted, body)                                     \
    do {                                                           \
        int signal_number_ = sigsetjmp(guard_point, 1);            \
        if (!signal_number_) {                                     \
            guard_armed = 1;                                       \
            body;                                                  \
            guard_armed = 0;                                       \
        }                                                          \
        (faulted) = signal_number_;                                \
    } while (0)

// ---- file checks -------------------------------------------------------------------------------------------------------------------
static unsigned listFolder(const char *step, const char *path, char names[][256], unsigned max) {
    mark(step, RUNNING);
    int error = 0;
    PlatformDirectory *directory = platformDirectoryOpen(path, &error);
    if (!directory) {
        say("fs list %s: open failed errno=%d (opendir is refused in titles; no .prospero-index fallback either)", path, error);
        mark(step, FAIL);
        return 0;
    }
    char name[256], shown[600] = "";
    uint8_t type;
    uint64_t inode;
    unsigned count = 0;
    int result;
    while ((result = platformDirectoryRead(directory, name, &type, &inode, &error)) == 1) {
        if (names && count < max) snprintf(names[count], 256, "%s", name);
        if (count < 20 && strlen(shown) + strlen(name) + 4 < sizeof(shown)) {
            strcat(shown, name);
            strcat(shown, type == 4 ? "/ " : " ");
        }
        ++count;
    }
    platformDirectoryClose(directory);
    say("fs list %s: %u entries%s: %s", path, count, result < 0 ? " (stopped by an error)" : "", shown);
    if (result < 0) say("fs list %s: read error errno=%d", path, error);
    mark(step, result < 0 ? FAIL : PASS);
    return count;
}

// ---- ROMs (loader-16): what is in the ROM folder against what PokeMMO uses (roms.c) ---------------------------------------------
// Without Black or White the game cannot be played: the ROM screen stays up before the game starts (Cross checks the folder
// again after an upload; PokeMMO cannot get past its start without it, so there is no way round). Missing optional games are a note; holding Square shows the screen.
// loader-23: the title folder is writable in a folder install and read-only in an image (.ffpfsc). Read-only: the ROMs live in
// the title storage (only the title's own upload page and FTP server can reach them), the log too (the page offers it).
static bool title_writable;
static char rom_folder[128] = "/app0/roms";
#define ROM_FOLDER rom_folder
static void chooseStorage(void) {
    int fd = open("/app0/.prospero-write-check", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    title_writable = fd >= 0;
    if (fd >= 0) {
        close(fd);
        unlink("/app0/.prospero-write-check");
        unlink("/app0/prospero-write-test.bin");  // loader-1's probe file
    } else {
        mkdir("/download0/root", 0755);
        snprintf(rom_folder, sizeof(rom_folder), "%s", "/download0/root/roms");
        mkdir(rom_folder, 0755);
    }
    say("storage: title folder %s; ROMs in %s; log in %s", title_writable ? "writable (folder install)" : "read-only (image install)", rom_folder,
        platformLogPath()[0] ? platformLogPath() : "(none)");
}
static _Atomic unsigned rom_count;  // games found
static RomScan rom_scan;
static pthread_mutex_t rom_lock = PTHREAD_MUTEX_INITIALIZER;
static char upload_address[32];
// loader-19: an FTP payload may not be running. The title looks for one (uploadDetectFtp) and, while the ROM screen is up, runs
// its own upload page (port 8080) and, when no FTP server answered, its own FTP server (port 2121) into the ROM folder.
#define ROM_FTP_PATH "/data/homebrew/" PROSPERO_TITLE_ID "/roms"
static unsigned existing_ftp_port;
static _Atomic bool uploads_started;
static void scanRoms(void);
static void romsChanged(void) { scanRoms(); }
static void startUploads(void) {
    if (atomic_exchange(&uploads_started, true)) return;
    // A running FTP payload is only of use when it can reach the ROM folder (a folder install).
    uploadServersStart(ROM_FOLDER, ROM_FTP_PATH, !(title_writable && existing_ftp_port), romsChanged);
}
// loader-26: where the full log can be had. A folder install: the title folder, over FTP. An image: the title storage, which
// only the title's upload page reaches, and only before the game starts (the page stops then).
static const char *logWhere(void) {
    static char where[128];
    if (title_writable) return "/data/homebrew/" PROSPERO_TITLE_ID "/prospero.log (FTP)";
    if (atomic_load(&uploadStatus()->http_running))
        snprintf(where, sizeof(where), "http://%s:%u/log (in a browser)", upload_address, (unsigned)UPLOAD_HTTP_PORT);
    else
        snprintf(where, sizeof(where), "http://%s:%u/log, before the game starts", upload_address, (unsigned)UPLOAD_HTTP_PORT);
    return where;
}
static _Atomic bool rom_blocking;
static _Atomic int rom_choice;  // 0 none, 1 check again (loader-22: PokeMMO cannot get past its start without Black/White)
static void scanRoms(void) {
    mark("fs.list.roms", RUNNING);
    RomScan scan;
    romsScan(ROM_FOLDER, &scan);
    pthread_mutex_lock(&rom_lock);
    rom_scan = scan;
    pthread_mutex_unlock(&rom_lock);
    for (unsigned i = 0; i < scan.count; ++i) say("roms: %s: %s", scan.files[i].file, scan.files[i].note);
    say("roms: %u of %u games found%s", romGamesFound(&scan), (unsigned)ROM_GAMES, scan.listed ? "" : " (the folder cannot be read)");
    atomic_store(&rom_count, romGamesFound(&scan));
    mark("fs.list.roms", scan.listed ? PASS : FAIL);
    mark("fs.romread", scan.found[ROM_BLACK_WHITE] >= 0 ? (romGamesFound(&scan) == ROM_GAMES ? PASS : INFO) : FAIL);
}
static void waitForRequiredRom(void) {
    while (rom_scan.found[ROM_BLACK_WHITE] < 0) {
        say("roms: Black/White missing: showing the ROM screen");
        atomic_store(&rom_choice, 0);
        atomic_store(&rom_blocking, true);
        while (!atomic_load(&rom_choice)) sceKernelUsleep(20000);
        scanRoms();
    }
    atomic_store(&rom_blocking, false);
}

// ---- the client: copied from the title folder (dev builds) into the title storage, where it can write next to itself ------------------
#define CLIENT_SOURCE "/app0/client"
#define ROOT "/download0/root"
// loader-18: the client lives in one of two slots (slots.c); `active_slot` is the one that starts, `target_slot` receives a
// new client (the other one) before it is switched to.
static SlotState slot_state;
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;
static char active_dir[300];
static char active_revision[64];  // of the active slot, "" when there is no client
static bool readSmall(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t got = read(fd, out, size - 1);
    close(fd);
    out[got > 0 ? got : 0] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return got > 0;
}
static bool makeParents(const char *path) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *slash = copy + 1; (slash = strchr(slash, '/')); ++slash) {
        *slash = 0;
        if (mkdir(copy, 0755) && errno != EEXIST) return false;
        *slash = '/';
    }
    return true;
}
static bool copyFile(const char *from, const char *to, char *buffer, size_t size) {
    int in = open(from, O_RDONLY);
    if (in < 0) {
        say("client copy: open %s errno=%d", from, errno);
        return false;
    }
    if (!makeParents(to)) {
        close(in);
        return false;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = out >= 0;
    while (ok) {
        ssize_t got = read(in, buffer, size);
        if (got < 0) ok = false;
        if (got <= 0) break;
        for (ssize_t done = 0; done < got && ok;) {
            ssize_t wrote = write(out, buffer + done, (size_t)(got - done));
            if (wrote <= 0)
                ok = false;
            else
                done += wrote;
        }
    }
    if (!ok) say("client copy: %s -> %s failed errno=%d", from, to, errno);
    close(in);
    if (out >= 0) close(out);
    return ok;
}
// manifest.txt (written by the installer): one "<size> <relative path>" line per file of the client's Linux part.
static _Atomic unsigned long long install_done, install_total;  // bytes, for the loading bar
static char client_revision[64];                                  // shown on the loading screen
static void noteRevision(const char *revision) {
    size_t length = strcspn(revision, "\r\n");
    snprintf(client_revision, sizeof(client_revision), "%.*s", (int)(length < 60 ? length : 60), revision);
}
static void switchTo(char slot, const char *revision);
// Developer builds: the client the installer uploaded (--client) goes into the other slot when it is newer than the active one.
static bool installClient(void) {
    mark("client.dev", RUNNING);
    char source_revision[64] = "", installed_revision[64] = "", target[300];
    snprintf(installed_revision, sizeof(installed_revision), "%s", active_revision);
    char slot = slotsOther(slot_state.active ? slot_state.active : 'b');
    slotsPath(slot, target, sizeof(target));
    if (!readSmall(CLIENT_SOURCE "/revision.txt", source_revision, sizeof(source_revision))) {
        say("client dev: no client uploaded to the title folder (the updater provides it)");
        mark("client.dev", INFO);
        return true;
    }
    if (updaterCompareRevisions(source_revision, installed_revision) <= 0 || !strcmp(source_revision, slot_state.failed)) {
        say("client dev: uploaded revision %s, installed %s%s: nothing to copy", source_revision, installed_revision,
            !strcmp(source_revision, slot_state.failed) ? " (it did not start before)" : "");
        mark("client.dev", PASS);
        return true;
    }
    slotsClear(slot);
    int fd = open(CLIENT_SOURCE "/manifest.txt", O_RDONLY);
    struct stat info;
    char *manifest = NULL;
    if (fd >= 0 && !fstat(fd, &info) && (manifest = malloc((size_t)info.st_size + 1))) {
        ssize_t got = read(fd, manifest, (size_t)info.st_size);
        manifest[got > 0 ? got : 0] = 0;
    }
    if (fd >= 0) close(fd);
    if (!manifest) {
        say("client install: manifest.txt missing");
        mark("client.dev", FAIL);
        return false;
    }
    say("client install: revision %s -> %s", installed_revision[0] ? installed_revision : "(none)", source_revision);
    unsigned long long total = 0;
    for (const char *line = manifest; *line; line += strcspn(line, "\n"), line += *line == '\n') {
        unsigned long long size = 0;
        if (sscanf(line, "%llu", &size) == 1) total += size;
    }
    atomic_store(&install_total, total);
    size_t buffer_size = 4u << 20;
    char *buffer = malloc(buffer_size);
    unsigned files = 0;
    unsigned long long bytes = 0;
    bool ok = buffer != NULL;
    uint64_t start = platformMonotonicNs();
    for (char *line = manifest; ok && *line;) {
        size_t length = strcspn(line, "\r\n");
        char saved = line[length];
        line[length] = 0;
        unsigned long long size = 0;
        char relative[400];
        if (sscanf(line, "%llu %399[^\n]", &size, relative) == 2 && !strstr(relative, "..") && strcmp(relative, "revision.txt")) {
            char from[512], to[512];
            snprintf(from, sizeof(from), CLIENT_SOURCE "/%s", relative);
            snprintf(to, sizeof(to), "%s/%s", target, relative);
            ok = copyFile(from, to, buffer, buffer_size);
            ++files;
            bytes += size;
            atomic_store(&install_done, bytes);
            if (files % 200 == 0) say("client install: %u files, %llu MiB", files, bytes >> 20);
        }
        line[length] = saved;
        line += length;
        line += strspn(line, "\r\n");
    }
    // The revision goes last: an interrupted copy is redone on the next start.
    char revision_path[340];
    snprintf(revision_path, sizeof(revision_path), "%s/revision.txt", target);
    if (ok) ok = copyFile(CLIENT_SOURCE "/revision.txt", revision_path, buffer, buffer_size);
    if (ok) ok = slotsMarkComplete(slot, source_revision);
    if (ok) switchTo(slot, source_revision);
    free(buffer);
    free(manifest);
    say("client install: %s, %u files, %llu MiB in %.1f s", ok ? "done" : "FAILED", files, bytes >> 20, (double)(platformMonotonicNs() - start) / 1e9);
    mark("client.dev", ok ? PASS : FAIL);
    return ok;
}

// ---- the client updater (loader-15): the published client, read in place on PokeMMO's server (updater.c) ------------------------
// A newer revision is offered on the loading screen (Cross: download, Circle: skip; download after a few seconds); without any
// client installed it is downloaded straight away. The published ETag of the installed revision is kept, so an unchanged
// client costs one HEAD request.
#define UPDATE_ETAG ROOT "/update-etag"  // the published zip's ETag when the active slot holds its revision
#define PROMPT_SECONDS 6
enum { UPDATE_IDLE, UPDATE_CHECKING, UPDATE_PROMPT, UPDATE_DOWNLOADING, UPDATE_APPLYING };
enum { CHOICE_NONE, CHOICE_DOWNLOAD, CHOICE_SKIP };
static _Atomic int update_phase, update_choice;
static _Atomic uint64_t prompt_deadline_ns;
static UpdaterProgress update_progress;
static char update_question[128];
static const char *_Atomic start_problem, *_Atomic start_advice;  // why the game cannot start, for the loading screen
static char update_warning[160];
static _Atomic bool update_warning_set;
static bool writeSmall(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}
// ---- slots (loader-18) ------------------------------------------------------------------------------------------------------------
// A new client (download or developer copy) goes into the other slot and is switched to "on trial"; the trial ends when the
// game shows its first picture (gameShowedPicture). A start that finds a trial still open goes back to the previous slot.
static void switchTo(char slot, const char *revision) {
    pthread_mutex_lock(&slot_lock);
    if (slot_state.active && slot_state.active != slot) slot_state.previous = slot_state.active;
    slot_state.active = slot;
    slot_state.trial = true;
    slotsSave(&slot_state);
    slotsPath(slot, active_dir, sizeof(active_dir));
    snprintf(active_revision, sizeof(active_revision), "%s", revision);
    pthread_mutex_unlock(&slot_lock);
    noteRevision(revision);
    say("slots: switched to slot %c (revision %s), on trial until the game shows a picture", slot, revision);
}
// loader-21: once a new revision has proven itself, the previous client is deleted (on its own thread: a couple of hundred files),
// so the second copy only exists while an update is on trial.
static void *removeSlotMain(void *argument) {
    slotsRemove((char)(uintptr_t)argument);
    return NULL;
}
static void gameShowedPicture(void) {
    pthread_mutex_lock(&slot_lock);
    char old = 0;
    if (slot_state.trial) {
        slot_state.trial = false;
        slot_state.failed[0] = 0;
        old = slot_state.previous;
        slot_state.previous = 0;
        slotsSave(&slot_state);
        say("slots: revision %s started: slot %c confirmed%s", active_revision, slot_state.active, old ? ", the previous client is removed" : "");
    }
    pthread_mutex_unlock(&slot_lock);
    pthread_t thread;
    if (old && !pthread_create(&thread, NULL, removeSlotMain, (void *)(uintptr_t)old)) pthread_detach(thread);
}
// The game ended before its first picture while on trial: back to the previous slot for the next start.
static bool revertTrial(const char *why) {
    pthread_mutex_lock(&slot_lock);
    char revision[64] = "";
    bool reverted = false;
    if (slot_state.trial && slot_state.previous && slotsRevision(slot_state.previous, revision, sizeof(revision))) {
        snprintf(slot_state.failed, sizeof(slot_state.failed), "%s", active_revision);
        char failed = slot_state.active;
        slot_state.active = slot_state.previous;
        slot_state.previous = failed;
        slot_state.trial = false;
        slotsSave(&slot_state);
        say("slots: revision %s did not start (%s): back to slot %c, revision %s", active_revision, why, slot_state.active, revision);
        reverted = true;
    }
    pthread_mutex_unlock(&slot_lock);
    return reverted;
}
static char slot_notice[160];
static void prepareSlots(void) {
    slotsInit(ROOT);
    slotsMigrate();
    if (!slotsLoad(&slot_state)) memset(&slot_state, 0, sizeof(slot_state));
    if (slot_state.trial) {
        char failed[64];
        snprintf(failed, sizeof(failed), "%s", "");
        slotsRevision(slot_state.active, failed, sizeof(failed));
        snprintf(active_revision, sizeof(active_revision), "%s", failed);
        if (revertTrial("the previous start ended before the game showed a picture")) {
            char now[64] = "";
            slotsRevision(slot_state.active, now, sizeof(now));
            snprintf(slot_notice, sizeof(slot_notice), "PokeMMO revision %s did not start (it may need a newer PokeMMO Prospero): back to %s.", failed, now);
        }
    }
    active_revision[0] = 0;
    if (slot_state.active && !slotsRevision(slot_state.active, active_revision, sizeof(active_revision))) slot_state.active = 0;
    if (slot_state.active && !slot_state.trial && slot_state.previous) {  // a confirmed client from before loader-21 kept the old one
        say("slots: removing the previous client (slot %c)", slot_state.previous);
        slotsRemove(slot_state.previous);
        slot_state.previous = 0;
        slotsSave(&slot_state);
    }
    if (slot_state.active) slotsPath(slot_state.active, active_dir, sizeof(active_dir));
    noteRevision(active_revision);
    say("slots: active %c (revision %s), previous %c, trial %d, failed %s", slot_state.active ? slot_state.active : '-',
        active_revision[0] ? active_revision : "none", slot_state.previous ? slot_state.previous : '-', slot_state.trial, slot_state.failed[0] ? slot_state.failed : "-");
}

static void updateClient(bool forget_installed) {
    mark("client.update", RUNNING);
    atomic_store(&update_phase, UPDATE_CHECKING);
    char installed[64] = "", known_etag[160] = "", error[256] = "";
    bool have_client = !forget_installed && active_revision[0];
    if (have_client) snprintf(installed, sizeof(installed), "%s", active_revision);
    readSmall(UPDATE_ETAG, known_etag, sizeof(known_etag));
    UpdaterRemote remote;
    int checked = updaterCheck(UPDATER_URL, have_client ? known_etag : NULL, &remote, error, sizeof(error));
    say("client update: installed %s, check %d, published %s (etag %s)%s%s", have_client ? installed : "(none)", checked, remote.revision[0] ? remote.revision : "-",
        remote.etag, error[0] ? ": " : "", error);
    if (checked < 0) {
        atomic_store(&update_phase, UPDATE_IDLE);
        if (have_client) {
            snprintf(update_warning, sizeof(update_warning), "Could not check for updates: starting revision %s.", installed);
            atomic_store(&update_warning_set, true);
            mark("client.update", INFO);
        } else {
            atomic_store(&start_advice, "Check the console's internet connection, then close the title and start it again.");
            atomic_store(&start_problem, "PokeMMO could not be downloaded");
            mark("client.update", FAIL);
        }
        updaterFree(&remote);
        return;
    }
    bool newer = checked == 0 && updaterCompareRevisions(remote.revision, installed) > 0 && strcmp(remote.revision, slot_state.failed);
    if (checked == 0 && !strcmp(remote.revision, slot_state.failed)) say("client update: revision %s did not start before: not offered again", remote.revision);
    if (!newer) {
        if (checked == 0 && !updaterCompareRevisions(remote.revision, installed)) writeSmall(UPDATE_ETAG, remote.etag);
        say("client update: up to date");
        atomic_store(&update_phase, UPDATE_IDLE);
        mark("client.update", PASS);
        updaterFree(&remote);
        return;
    }
    if (have_client) {
        snprintf(update_question, sizeof(update_question), "PokeMMO update: revision %s to %s (%llu MB)", installed, remote.revision,
                 (unsigned long long)(remote.download_bytes >> 20));
        atomic_store(&update_choice, CHOICE_NONE);
        atomic_store(&prompt_deadline_ns, platformMonotonicNs() + PROMPT_SECONDS * 1000000000ull);
        atomic_store(&update_phase, UPDATE_PROMPT);
        while (atomic_load(&update_choice) == CHOICE_NONE && platformMonotonicNs() < atomic_load(&prompt_deadline_ns)) sceKernelUsleep(20000);
        if (atomic_load(&update_choice) == CHOICE_SKIP) {
            say("client update: skipped by the player");
            snprintf(update_warning, sizeof(update_warning), "Update skipped: starting revision %s.", installed);
            atomic_store(&update_warning_set, true);
            atomic_store(&update_phase, UPDATE_IDLE);
            mark("client.update", INFO);
            updaterFree(&remote);
            return;
        }
    }
    atomic_store(&update_phase, UPDATE_DOWNLOADING);
    char slot = slotsOther(slot_state.active ? slot_state.active : 'b'), target[300];
    slotsPath(slot, target, sizeof(target));
    slotsClear(slot);
    uint64_t start = platformMonotonicNs();
    int result = updaterDownload(UPDATER_URL, &remote, target, &update_progress, error, sizeof(error));
    double seconds = (double)(platformMonotonicNs() - start) / 1e9;
    say("client update: download %s, %llu MB in %.0f s%s%s", result ? "FAILED" : "done", (unsigned long long)(atomic_load(&update_progress.done) >> 20), seconds,
        error[0] ? ": " : "", error);
    if (!result) {
        atomic_store(&update_phase, UPDATE_APPLYING);
        result = slotsMarkComplete(slot, remote.revision) ? 0 : -1;
        if (!result) switchTo(slot, remote.revision);
        say("client update: slot %c %s", slot, result ? "could not be marked complete" : "ready");
    }
    atomic_store(&update_phase, UPDATE_IDLE);
    if (!result) {
        writeSmall(UPDATE_ETAG, remote.etag);
        mark("client.update", PASS);
    } else if (have_client) {
        snprintf(update_warning, sizeof(update_warning), "The update failed: starting revision %s.", installed);
        atomic_store(&update_warning_set, true);
        mark("client.update", INFO);
    } else {
        atomic_store(&start_advice, "Check the console's internet connection and free space, then close the title and start it again.");
        atomic_store(&start_problem, "PokeMMO could not be downloaded");
        mark("client.update", FAIL);
    }
    updaterFree(&remote);
}

// ---- the game's settings: a copy where FTP can see it ---------------------------------------------------------------------------
// The client keeps its settings in config/main.properties next to itself, in the title storage (not visible over FTP). A copy
// goes to the title folder at start and whenever the file changes (checked every minute): a backup, and the way to read it.
#define SHARED_CONFIG ROOT "/shared/config"  // mounted over the game's /game/config (slots.c)
#define SETTINGS SHARED_CONFIG "/main.properties"
#define SETTINGS_COPY "/app0/settings/main.properties"
static void backupSettings(void) {
    if (!title_writable) return;  // an image install: the upload page offers the settings instead
    static time_t last_time;
    static off_t last_size = -1;
    struct stat info;
    if (stat(SETTINGS, &info) || (info.st_mtime == last_time && info.st_size == last_size)) return;
    static char buffer[65536];
    bool ok = copyFile(SETTINGS, SETTINGS_COPY, buffer, sizeof(buffer));
    say("settings: %s copied to %s (%lld bytes)%s", SETTINGS, SETTINGS_COPY, (long long)info.st_size, ok ? "" : " FAILED");
    if (ok) {
        last_time = info.st_mtime;
        last_size = info.st_size;
    }
}

// Default settings (assets/settings/defaults.properties, shipped in the title): written in full when the game has no settings yet,
// and applied once per DEFAULTS_VERSION to settings that exist, so that what the player changes afterwards stays. The version
// applied is kept in config/.prospero-defaults. Lines of other keys are left exactly as they were.
#define DEFAULTS "/app0/assets/defaults.properties"
#define DEFAULTS_MARK SHARED_CONFIG "/.prospero-defaults"
static char *readWhole(const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat info;
    char *text = NULL;
    if (fd >= 0 && !fstat(fd, &info) && info.st_size < (1 << 20) && (text = malloc((size_t)info.st_size + 1))) {
        ssize_t got = read(fd, text, (size_t)info.st_size);
        text[got > 0 ? got : 0] = 0;
    }
    if (fd >= 0) close(fd);
    return text;
}
static bool writeWhole(const char *path, const char *text) {
    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.prospero-new", path);
    if (!makeParents(path)) return false;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t length = strlen(text), done = 0;
    while (done < length) {
        ssize_t wrote = write(fd, text + done, length - done);
        if (wrote <= 0) break;
        done += (size_t)wrote;
    }
    close(fd);
    if (done != length || rename(temporary, path)) {
        unlink(temporary);
        return false;
    }
    return true;
}
static void applyDefaults(void) {
    char *defaults = readWhole(DEFAULTS);
    if (!defaults) {
        say("settings defaults: %s missing", DEFAULTS);
        return;
    }
    int version = 0, applied = 0;
    const char *found = strstr(defaults, "DEFAULTS_VERSION=");
    if (found) version = atoi(found + 17);
    char mark[32] = "";
    if (readSmall(DEFAULTS_MARK, mark, sizeof(mark))) applied = atoi(mark);
    if (applied >= version) {
        free(defaults);
        return;
    }
    char *current = readWhole(SETTINGS);
    size_t capacity = (current ? strlen(current) : 0) + strlen(defaults) + 64;
    char *result = malloc(capacity);
    if (!result) {
        free(defaults);
        free(current);
        return;
    }
    result[0] = 0;
    // The player's lines, except those the defaults set.
    unsigned replaced = 0, added = 0;
    for (const char *line = current ? current : ""; *line;) {
        size_t length = strcspn(line, "\n");
        size_t key_length = strcspn(line, "=\n");
        bool overridden = false;
        if (line[0] != '#' && key_length < length) {
            for (const char *d = defaults; *d;) {
                size_t d_length = strcspn(d, "\n"), d_key = strcspn(d, "=\n");
                if (d[0] != '#' && d_key == key_length && !strncmp(d, line, key_length)) overridden = true;
                d += d_length;
                d += *d == '\n';
            }
        }
        if (!overridden) strncat(result, line, length), strcat(result, "\n");
        replaced += overridden;
        line += length;
        line += *line == '\n';
    }
    for (const char *d = defaults; *d;) {  // then the defaults' own lines
        size_t d_length = strcspn(d, "\n");
        if (d[0] != '#' && d[0] != '\r' && d_length && strncmp(d, "DEFAULTS_VERSION=", 17)) {
            strncat(result, d, d_length);
            strcat(result, "\n");
            ++added;
        }
        d += d_length;
        d += *d == '\n';
    }
    bool ok = writeWhole(SETTINGS, result);
    if (ok) {
        snprintf(mark, sizeof(mark), "%d\n", version);
        ok = writeWhole(DEFAULTS_MARK, mark);
    }
    say("settings defaults: version %d applied to %s settings (%u lines set, %u replaced)%s", version, current ? "existing" : "new", added, replaced,
        ok ? "" : " FAILED");
    free(result);
    free(defaults);
    free(current);
}

// ---- the client run -----------------------------------------------------------------------------------------------------------------
static _Atomic bool game_finished, release_screen, screen_released;
static _Atomic uint64_t client_started_ns;
// The SDL layer calls this on the game's thread just before it makes its window surface (ps5-opengl has one): the loading screen
// stays up through the client's start and gives the display away only then.
static void acquireDisplay(void) {
    if (atomic_load(&screen_released)) return;
    atomic_store(&release_screen, true);
    while (!atomic_load(&screen_released)) sceKernelUsleep(5000);
}
static void drawLoadingInGame(void);
static void *gameThread(void *argument) {
    (void)argument;
    // This project's SDL3, EGL/GLX, OpenAL and GTK (the file chooser), adapted from PokeMMO-NX.
    static LinuxVirtualLibrary virtual_libraries[8];
    virtual_libraries[0] = linuxSdlLibrary;
    virtual_libraries[1] = linuxEglLibrary;
    virtual_libraries[2] = linuxOpenAlLibrary;
    virtual_libraries[3] = linuxGlxLibrary;
    for (unsigned i = 0; i < 4; ++i) virtual_libraries[4 + i] = linuxGtkLibraries[i];
    linuxAudioOutAttach();
    linuxSdlSetDisplayAcquire(acquireDisplay);
    linuxSdlSetLoadingOverlay(drawLoadingInGame);
    atomic_store(&client_started_ns, platformMonotonicNs());
    mark("client.map", RUNNING);
    static const char *const options[] = {"-XX:MaxHeapSize=640m", "-XX:MaxNewSize=128m", NULL};
    char client_path[400];
    snprintf(client_path, sizeof(client_path), "%s/bin/linux/x64/PokeMMO", active_dir);
    say("client: starting slot %c, revision %s", slot_state.active, active_revision);
    GameConfig config = {.root = ROOT,
                         .client_path = client_path,
                         // The game sees its folder as /game: the active slot, with the shared settings over /game/config. The C++
                         // runtime the client's native libraries need (libstdc++, libgcc_s) ships with the title.
                         .mounts = {{"/game", NULL}, {"/game/config", SHARED_CONFIG}, {"/game/roms", NULL}, {"/lib", "/app0/assets/lib"}},
                         .mount_count = 4,
                         .arguments = options,
                         .timeout_seconds = 0,
                         .virtual_libraries = virtual_libraries,
                         .virtual_count = 8};
    config.mounts[0].native = active_dir;
    config.mounts[2].native = rom_folder;
    mkdir(ROOT, 0755);
    linuxSdlSetFirstPicture(gameShowedPicture);
    bool ok = gameRun(&config);
    if (!ok && revertTrial(gameFailure())) {
        atomic_store(&start_advice, "It may need a newer PokeMMO Prospero. Close the title and start it again to use the previous revision.");
        atomic_store(&start_problem, "This PokeMMO update could not start");
    }
    say("client: %s%s", ok ? "ended normally" : "ended: ", ok ? "" : gameFailure());
    if (ok) {  // loader-21: the player chose Exit in the game: the title closes and the console goes back to the home screen
        backupSettings();
        say("client: exited from the game: closing the title");
        platformQuit();
    }
    mark("client.map", strstr(gameFailure(), "cannot be read") ? FAIL : PASS);
    mark("client.start", strstr(gameFailure(), "cannot be read") ? NOT_RUN : PASS);
    mark("client.end", ok ? PASS : INFO);
    atomic_store(&game_finished, true);
    return NULL;
}

// ---- screen ---------------------------------------------------------------------------------------------------------------------
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext screen_context = EGL_NO_CONTEXT;
static EGLint width, height;
static bool screenOpen(void) {
    static const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0, major = 0, minor = 0;
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        return false;
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    screen_context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (surface == EGL_NO_SURFACE || screen_context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, screen_context)) return false;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    return true;
}
static int stepState(const char *name) {
    for (size_t i = 0; i < STEP_COUNT; ++i)
        if (!strcmp(steps[i].name, name)) return atomic_load(&steps[i].state);
    return NOT_RUN;
}
// Without the overlay (no OpenGL entry points): the coloured tiles of the earlier loaders, drawn with scissored clears.
static void drawTilesPlain(unsigned frame) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glClearColor(0.03f, 0.07f, 0.16f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    const int columns = 5, size = width / 12, gap = size / 6;
    const int left = (width - columns * size - (columns - 1) * gap) / 2;
    const int rows = (int)((STEP_COUNT + columns - 1) / columns);
    const int top = (height + rows * size + (rows - 1) * gap) / 2;
    for (size_t i = 0; i < STEP_COUNT; ++i) {
        int column = (int)(i % columns), row = (int)(i / columns);
        glScissor(left + column * (size + gap), top - (row + 1) * size - row * gap, size, size);
        switch (atomic_load(&steps[i].state)) {
            case PASS: glClearColor(0.15f, 0.75f, 0.30f, 1); break;
            case FAIL: glClearColor(0.85f, 0.20f, 0.20f, 1); break;
            case INFO: glClearColor(0.25f, 0.45f, 0.85f, 1); break;
            case RUNNING: glClearColor((frame / 20) % 2 ? 0.95f : 0.75f, (frame / 20) % 2 ? 0.80f : 0.60f, 0.10f, 1); break;
            default: glClearColor(0.35f, 0.35f, 0.38f, 1); break;
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}
// What the loading screen says, from the steps and the install progress.
static void loadingView(LoadingView *view, LoadingStep *step_view, char *detail, size_t detail_size, unsigned frame) {
    static char version[128];
    snprintf(version, sizeof(version), "Prospero %s (%s)", LOADER_MILESTONE, PROSPERO_VERSION);
    for (size_t i = 0; i < STEP_COUNT; ++i) step_view[i] = (LoadingStep){steps[i].name, atomic_load(&steps[i].state)};
    *view = (LoadingView){.status = "Getting ready", .detail = detail, .version = version, .revision = client_revision,
                          .log_where = logWhere(), .steps = step_view, .step_count = STEP_COUNT, .frame = frame};
    detail[0] = 0;
    static const char *const checks[] = {"fs.list.app0", "fs.list.roms", "fs.romread"};
    unsigned checked = 0;
    for (unsigned i = 0; i < 3; ++i) checked += stepState(checks[i]) != NOT_RUN && stepState(checks[i]) != RUNNING;
    view->fraction = 0.08f * (float)checked / 3.0f;
    if (checked < 3) view->status = "Checking files";
    int dev = stepState("client.dev"), update = stepState("client.update"), phase = atomic_load(&update_phase);
    if (dev == RUNNING) {
        unsigned long long done = atomic_load(&install_done), total = atomic_load(&install_total);
        view->status = "Installing PokeMMO";
        if (total) {
            view->fraction = 0.08f + 0.52f * (float)((double)done / (double)total);
            snprintf(detail, detail_size, "%llu of %llu MB", done >> 20, total >> 20);
        }
    } else if (update == RUNNING) {
        view->fraction = 0.08f;
        if (phase == UPDATE_CHECKING)
            view->status = "Checking for updates";
        else if (phase == UPDATE_PROMPT) {
            static char choices[96];
            uint64_t now = platformMonotonicNs(), deadline = atomic_load(&prompt_deadline_ns);
            unsigned left = deadline > now ? (unsigned)((deadline - now) / 1000000000ull) + 1 : 0;
            snprintf(choices, sizeof(choices), "\x01 Download     \x02 Skip          (downloading in %u)", left);
            view->question = update_question;
            view->choices = choices;
        } else if (phase == UPDATE_DOWNLOADING) {
            uint64_t done = atomic_load(&update_progress.done), total = atomic_load(&update_progress.total);
            view->status = "Downloading PokeMMO";
            if (total) {
                view->fraction = 0.08f + 0.52f * (float)((double)done / (double)total);
                snprintf(detail, detail_size, "%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
            }
        } else if (phase == UPDATE_APPLYING) {
            view->fraction = 0.6f;
            view->status = "Installing the update";
        }
    } else if (update != NOT_RUN && update != FAIL) {
        uint64_t started = atomic_load(&client_started_ns);
        float seconds = started ? (float)((double)(platformMonotonicNs() - started) / 1e9) : 0.0f;
        view->fraction = 0.6f + 0.38f * (1.0f - expf(-seconds / 20.0f));  // the client's start has no progress to report: an easing guess
        view->status = "Starting PokeMMO";
        if (atomic_load(&update_warning_set)) view->warning = update_warning;
    }
    const char *problem = atomic_load(&start_problem);
    if (stepState("fs.list.app0") == FAIL) {
        view->problem = "The title's files cannot be read";
        view->advice = "Run the installer on your computer again.";
    } else if (problem) {
        view->problem = problem;
        view->advice = atomic_load(&start_advice);
    } else if (atomic_load(&game_finished) || atomic_load(&fatal_signals)) {
        view->problem = "PokeMMO stopped while starting";
        static char advice[200];
        if (title_writable)
            snprintf(advice, sizeof(advice), "%s", "Close the title with the PS button and start it again. If it keeps happening, send prospero.log from the title folder (FTP).");
        else
            snprintf(advice, sizeof(advice), "Close the title with the PS button and start it again. If it keeps happening, hold Square and save "
                                             "http://%s:%u/log-previous (this start's log) from a browser.", upload_address, (unsigned)UPLOAD_HTTP_PORT);
        view->advice = advice;
    }
    if (!view->warning && stepState("fs.romread") == INFO) {
        static char note[96];
        snprintf(note, sizeof(note), "%u of %u games found: hold \x03 for the ROM list", atomic_load(&rom_count), (unsigned)ROM_GAMES);
        view->warning = note;
    }
}
// The same screen over the game's first (black) frames, on the game's thread and context.
static void drawLoadingInGame(void) {
    static unsigned frame;
    LoadingView view;
    LoadingStep step_view[STEP_COUNT];
    char detail[96];
    loadingView(&view, step_view, detail, sizeof(detail), ++frame);
    loadingScreenDraw(&view);
}
static void drawScreen(unsigned frame) {
    if (!overlayBegin((unsigned)width, (unsigned)height)) {
        drawTilesPlain(frame);
        return;
    }
    LoadingView view;
    LoadingStep step_view[STEP_COUNT];
    char detail[96];
    loadingView(&view, step_view, detail, sizeof(detail), frame);
    PlatformPad pad;
    bool read = platformPadRead(&pad);
    view.details = read && (pad.buttons & PLATFORM_PAD_TRIANGLE);
    if (view.details && !title_writable) startUploads();  // an image: the page is the only way to the log (no-op once the game starts)
    static uint32_t before = ~0u;  // what is held when the question appears does not answer it
    uint32_t pressed = read ? pad.buttons & ~before : 0;
    before = read ? pad.buttons : 0;
    if (atomic_load(&update_phase) == UPDATE_PROMPT) {
        if (pressed & PLATFORM_PAD_CROSS) atomic_store(&update_choice, CHOICE_DOWNLOAD);
        if (pressed & PLATFORM_PAD_CIRCLE) atomic_store(&update_choice, CHOICE_SKIP);
    }
    bool blocking = atomic_load(&rom_blocking);
    if (blocking && !atomic_load(&rom_choice)) {
        if (pressed & PLATFORM_PAD_CROSS) atomic_store(&rom_choice, 1);
    }
    if (!view.details && (blocking || (read && (pad.buttons & PLATFORM_PAD_SQUARE)))) {
        pthread_mutex_lock(&rom_lock);
        RomScan scan = rom_scan;
        pthread_mutex_unlock(&rom_lock);
        startUploads();
        UploadStatus *upload = uploadStatus();
        char receiving[200] = "";
        if (upload->current[0]) {
            uint64_t done = atomic_load(&upload->current_done), total = atomic_load(&upload->current_total);
            if (total)
                snprintf(receiving, sizeof(receiving), "Receiving %s: %llu of %llu MB", upload->current, (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
            else
                snprintf(receiving, sizeof(receiving), "Receiving %s: %llu MB", upload->current, (unsigned long long)(done >> 20));
        } else if (atomic_load(&upload->received))
            snprintf(receiving, sizeof(receiving), "%u file(s) received.", atomic_load(&upload->received));
        RomUploadInfo info = {.address = upload_address,
                              .web = atomic_load(&upload->http_running),
                              .ftp_port = title_writable && existing_ftp_port ? existing_ftp_port
                                          : atomic_load(&upload->ftp_running)    ? atomic_load(&upload->ftp_port)
                                                                                 : 0,
                              .folder = ROM_FTP_PATH "/",
                              .receiving = receiving};
        romScreenDraw(&scan, &info, blocking);
        overlayEnd();
        return;
    }
    loadingScreenDraw(&view);
    overlayEnd();
}

// loader-28 probe: can the title keep files in /data, outside its own storage? There they would survive deleting or
// reinstalling the title, and FTP payloads could reach them. Only tested here: nothing is moved yet. The probe file stays so that
// it can be looked for over FTP (/data/pokemmo-prospero/probe.txt).
static bool probeFolder(const char *folder) {
    char path[160], text[160], back[160] = "";
    errno = 0;
    int made = mkdir(folder, 0755);
    say("data probe: mkdir %s: %s (errno %d)", folder, made == 0 ? "made" : errno == EEXIST ? "already there" : "failed", made == 0 ? 0 : errno);
    snprintf(path, sizeof(path), "%s/probe.txt", folder);
    int length = snprintf(text, sizeof(text), "Written by PokeMMO-Prospero %s to test this folder; safe to delete.\n", LOADER_MILESTONE);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        say("data probe: create %s failed (errno %d)", path, errno);
        return false;
    }
    errno = 0;
    bool written = write(fd, text, (size_t)length) == length;
    int write_errno = errno;
    close(fd);
    fd = open(path, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, back, sizeof(back) - 1) : -1;
    if (fd >= 0) close(fd);
    bool same = written && got == length && !memcmp(back, text, (size_t)length);
    say("data probe: %s: write %s (errno %d), read back %s", path, written ? "ok" : "failed", write_errno, same ? "the same" : "different or failed");
    return same;
}
static void probeData(void) {
    mark("fs.data", RUNNING);
    struct stat info;
    say("data probe: /data %s; /data/homebrew %s", stat("/data", &info) ? "not visible" : "visible", stat("/data/homebrew", &info) ? "not visible" : "visible");
    bool data = probeFolder("/data/pokemmo-prospero");
    bool user = probeFolder("/user/data/pokemmo-prospero");
    say("data probe: /data %s, /user/data %s", data ? "WRITABLE" : "not writable", user ? "WRITABLE" : "not writable");
    mark("fs.data", data || user ? PASS : INFO);
}

// The checks and the client start run behind the screen.
static void *workMain(void *argument) {
    (void)argument;
    listFolder("fs.list.app0", "/app0", NULL, 0);
    prepareSlots();
    probeData();
    char address[16];
    snprintf(upload_address, sizeof(upload_address), "%s", platformLocalIPv4(address) ? address : "<console IP>");
    existing_ftp_port = uploadDetectFtp();
    say("upload: %s", existing_ftp_port ? "an FTP server is already running" : "no FTP server is running: the ROM screen will start one");
    scanRoms();
    // The installer's --redownload-client: forget the installed client (its files are overwritten, settings stay) and skip the
    // developer copy this once, so the updater downloads it.
    bool redownload = !unlink("/app0/redownload-client");
    if (redownload) {
        unlink(UPDATE_ETAG);
        say("client: the installer asked for a fresh download (into the other slot)");
        mark("client.dev", INFO);
    } else
        installClient();
    updateClient(redownload);
    if (slot_notice[0] && !atomic_load(&update_warning_set)) {
        snprintf(update_warning, sizeof(update_warning), "%s", slot_notice);
        atomic_store(&update_warning_set, true);
    }
    if (slot_state.active && active_revision[0]) {
        waitForRequiredRom();
        // The ports are free again before the game starts; marking them started keeps Square/Triangle from starting them after.
        if (atomic_exchange(&uploads_started, true)) uploadServersStop();
        applyDefaults();
        pthread_t game;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 1u << 20);
        if (!pthread_create(&game, &attributes, gameThread, NULL)) pthread_detach(game);
        pthread_attr_destroy(&attributes);
    } else if (!atomic_load(&start_problem)) {
        atomic_store(&start_advice, "Check the console's internet connection, then close the title and start it again.");
        atomic_store(&start_problem, "PokeMMO is not installed yet");
    }
    return NULL;
}

int main(void) {
    say("PokeMMO-Prospero %s (build %s, %s) starting; UDP log port 18194", LOADER_MILESTONE, PROSPERO_VERSION, PROSPERO_TITLE_ID);
    guardInstall();
    signal(SIGPIPE, SIG_IGN);  // a write to a closed socket or pipe must fail with EPIPE, not end the title
    bool screen = screenOpen();
    say("screen: %s %dx%d", screen ? "ready" : "unavailable", width, height);
    chooseStorage();
    uploadSetDownloads(platformLogPath(), SETTINGS);
    pthread_t worker;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_create(&worker, &attributes, workMain, NULL);
    pthread_attr_destroy(&attributes);
    bool reported = false;
    for (unsigned frame = 1;; ++frame) {
        if (atomic_load(&release_screen) && !atomic_load(&screen_released)) {
            if (screen) {
                overlayContextLost();  // the game's context may get this one's address
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(display, screen_context);
                eglDestroySurface(display, surface);  // the display stays initialized: the client's SDL initializes it again
                screen = false;
                say("screen: handed to the client");
            }
            atomic_store(&screen_released, true);
        }
        if (screen) {
            drawScreen(frame);
            eglSwapBuffers(display, surface);
        }
        if (frame == 1 || frame % 3750 == 0) backupSettings();  // about once a minute
        if (atomic_load(&fatal_signals) && atomic_load(&steps[STEP_COUNT - 1].state) != FAIL) mark("client.end", FAIL);
        bool install_failed = atomic_load(&start_problem) != NULL;
        if ((atomic_load(&game_finished) || install_failed) && !reported) {
            reported = true;
            say("DONE. Close the title with the PS button. The full log is above and in %s.", platformLogPath());
        }
        sceKernelUsleep(16000);
    }
}
