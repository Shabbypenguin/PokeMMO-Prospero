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

#define LOADER_MILESTONE "loader-11"
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
static Step steps[] = {
    {"fs.list.app0", NOT_RUN}, {"fs.list.roms", NOT_RUN}, {"fs.romread", NOT_RUN},   {"sys.modules", NOT_RUN}, {"net.https", NOT_RUN},
    {"client.install", NOT_RUN}, {"client.map", NOT_RUN},  {"client.start", NOT_RUN}, {"client.end", NOT_RUN},
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

// Runs a check on its own thread and gives up waiting after `seconds` (the thread is then left behind).
typedef struct {
    void (*function)(void);
    _Atomic bool done;
} Timed;
static void *timedMain(void *argument) {
    Timed *timed = argument;
    timed->function();
    atomic_store(&timed->done, true);
    return NULL;
}
static bool runWithLimit(void (*function)(void), unsigned seconds, const char *step) {
    Timed *timed = calloc(1, sizeof(*timed));  // leaked on purpose when the thread is left behind
    timed->function = function;
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&thread, &attributes, timedMain, timed);
    pthread_attr_destroy(&attributes);
    if (rc) {
        say("%s: thread failed %d", step, rc);
        return false;
    }
    for (unsigned waited = 0; waited < seconds * 10; ++waited) {
        if (atomic_load(&timed->done)) {
            free(timed);
            return true;
        }
        sceKernelUsleep(100000);
    }
    say("%s: no answer after %u s, left running", step, seconds);
    return false;
}

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

static void checkRoms(char names[][256], unsigned count) {
    mark("fs.romread", RUNNING);
    if (!count) {
        say("fs romread: no ROMs listed in /app0/roms (upload them with the installer)");
        mark("fs.romread", INFO);
        return;
    }
    unsigned readable = 0;
    for (unsigned i = 0; i < count; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "/app0/roms/%s", names[i]);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            say("fs romread %s: open errno=%d", names[i], errno);
            continue;
        }
        unsigned char header[0xb0];
        ssize_t got = read(fd, header, sizeof(header));
        struct stat info;
        fstat(fd, &info);
        close(fd);
        char title[13] = {0};
        bool gba = strstr(names[i], ".gba") || strstr(names[i], ".GBA");
        if (got == (ssize_t)sizeof(header)) memcpy(title, gba ? header + 0xa0 : header, 12);
        for (int c = 0; c < 12; ++c)
            if (title[c] && (title[c] < 32 || title[c] > 126)) title[c] = '?';
        say("fs romread %s: %lld bytes, header title \"%s\"", names[i], (long long)info.st_size, title);
        if (got == (ssize_t)sizeof(header)) ++readable;
    }
    mark("fs.romread", readable == count ? PASS : FAIL);
}

// ---- system modules: can a title load them at run time, and from which path? ----------------------------------------------------
static int loadModule(const char *name) {
    const char *word = sceKernelGetFsSandboxRandomWord();
    char paths[4][256];
    snprintf(paths[0], 256, "%s", name);
    snprintf(paths[1], 256, "/system/common/lib/%s", name);
    snprintf(paths[2], 256, "/%s/common/lib/%s", word ? word : "?", name);
    snprintf(paths[3], 256, "/system_ex/common_ex/lib/%s", name);
    for (int i = 0; i < 4; ++i) {
        int start_result = 0, handle = -1, faulted = 0;
        GUARDED(faulted, handle = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &start_result));
        say("sys module %s -> 0x%x (start 0x%x)%s", paths[i], handle, start_result, faulted ? " FAULTED" : "");
        if (handle >= 0 && !faulted) return handle;
    }
    return -1;
}
// Sony's NID of a symbol name: SHA-1 of the name and a fixed suffix, first 8 bytes reversed, base64 with '+' and '-'.
static void sha1(const unsigned char *data, size_t length, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    unsigned char block[64];
    size_t total = length + 9, blocks = (total + 63) / 64;
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t i = 0; i < 64; ++i) {
            size_t at = b * 64 + i;
            block[i] = at < length ? data[at] : at == length ? 0x80 : 0;
        }
        if (b == blocks - 1)
            for (int i = 0; i < 8; ++i) block[63 - i] = (unsigned char)(((uint64_t)length * 8) >> (8 * i));
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)block[4 * i] << 24 | block[4 * i + 1] << 16 | block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 80; ++i) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = x << 1 | x >> 31;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f = i < 20 ? (bb & c) | (~bb & d) : i < 40 ? bb ^ c ^ d : i < 60 ? (bb & c) | (bb & d) | (c & d) : bb ^ c ^ d;
            uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d, d = c, c = bb << 30 | bb >> 2, bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 20; ++i) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}
static void nidEncode(const char *name, char nid[12]) {
    static const unsigned char suffix[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    unsigned char input[256 + 16], digest[20], bytes[8];
    size_t length = strlen(name) > 256 ? 256 : strlen(name);
    memcpy(input, name, length);
    memcpy(input + length, suffix, 16);
    sha1(input, length + 16, digest);
    for (int i = 0; i < 8; ++i) bytes[i] = digest[7 - i];
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = value << 8 | bytes[i];
    // 64 bits -> 11 characters (the last one carries 4 bits, shifted as base64 does)
    for (int i = 0; i < 10; ++i) nid[i] = alphabet[(value >> (58 - 6 * i)) & 63];
    nid[10] = alphabet[(value & 15) << 2];
    nid[11] = 0;
}
static void *symbol(int module, const char *name) {
    void *address = NULL;
    if (module < 0) return NULL;
    int rc = sceKernelDlsym(module, name, &address);
    if (!rc && address) return address;
    char nid[12];
    nidEncode(name, nid);
    int rc_nid = sceKernelDlsym(module, nid, &address);
    say("sys symbol %s: by name 0x%x, by NID %s 0x%x -> %p", name, rc, nid, rc_nid, rc_nid ? NULL : address);
    return rc_nid ? NULL : address;
}
static int ssl_module = -1, http_module = -1, audio_module = -1;
static void checkModules(void) {
    mark("sys.modules", RUNNING);
    const char *word = sceKernelGetFsSandboxRandomWord();
    say("sys sandbox word: %s", word ? word : "(none)");
    ssl_module = loadModule("libSceSsl.sprx");
    http_module = loadModule("libSceHttp.sprx");
    audio_module = loadModule("libSceAudioOut.sprx");
    int ime = loadModule("libSceImeDialog.sprx");  // the system keyboard: probe-3 could not load it by name or /system path
    say("sys modules: ssl=%d http=%d audio=%d ime=%d", ssl_module, http_module, audio_module, ime);
    mark("sys.modules", ssl_module >= 0 && http_module >= 0 && audio_module >= 0 ? PASS : INFO);
}

// ---- HTTPS through the system's own library (certificate checks included) -------------------------------------------------------------
static void httpsBody(void) {
    int (*sslInit)(size_t) = symbol(ssl_module, "sceSslInit");
    int (*httpInit)(int, int, size_t) = symbol(http_module, "sceHttpInit");
    int (*createTemplate)(int, const char *, int, int) = symbol(http_module, "sceHttpCreateTemplate");
    int (*createConnection)(int, const char *, int) = symbol(http_module, "sceHttpCreateConnectionWithURL");
    int (*createRequest)(int, int, const char *, uint64_t) = symbol(http_module, "sceHttpCreateRequestWithURL");
    int (*sendRequest)(int, const void *, size_t) = symbol(http_module, "sceHttpSendRequest");
    int (*statusCode)(int, int *) = symbol(http_module, "sceHttpGetStatusCode");
    int (*allHeaders)(int, char **, size_t *) = symbol(http_module, "sceHttpGetAllResponseHeaders");
    if (!sslInit || !httpInit || !createTemplate || !createConnection || !createRequest || !sendRequest || !statusCode) {
        mark("net.https", FAIL);
        return;
    }
    const char *url = "https://dl.pokemmo.com/download/PokeMMO-Client.zip";
    int faulted = 0, net = -1, ssl = -1, http = -1, template_id = -1, connection = -1, request = -1, sent = -1, status = 0, got_status = -1;
    GUARDED(faulted, {
        sceNetInit();
        net = sceNetPoolCreate("prospero-http", 128 * 1024, 0);
        ssl = sslInit(256 * 1024);
        http = httpInit(net, ssl, 64 * 1024);
        template_id = createTemplate(http, "PokeMMO-Prospero/" PROSPERO_VERSION, 2 /* HTTP/1.1 */, 1);
        connection = createConnection(template_id, url, 1);
        request = createRequest(connection, 2 /* HEAD */, url, 0);
        sent = sendRequest(request, NULL, 0);
        got_status = statusCode(request, &status);
    });
    say("net https: pool=0x%x ssl=0x%x http=0x%x template=0x%x connection=0x%x request=0x%x send=0x%x status_rc=0x%x status=%d%s", net, ssl, http,
        template_id, connection, request, sent, got_status, status, faulted ? " FAULTED" : "");
    if (!faulted && !got_status && allHeaders) {
        char *headers = NULL;
        size_t length = 0;
        int rc = -1;
        GUARDED(faulted, rc = allHeaders(request, &headers, &length));
        if (!faulted && !rc && headers) {
            char line[256];
            for (const char *cursor = headers; cursor < headers + length && *cursor;) {
                size_t n = strcspn(cursor, "\r\n");
                if (!strncasecmp(cursor, "etag", 4) || !strncasecmp(cursor, "content-length", 14) || !strncasecmp(cursor, "last-modified", 13)) {
                    snprintf(line, sizeof(line), "%.*s", (int)n, cursor);
                    say("net https header: %s", line);
                }
                cursor += n;
                cursor += strspn(cursor, "\r\n");
            }
        }
    }
    mark("net.https", !faulted && status == 200 ? PASS : INFO);  // not needed yet: the client does its own HTTPS
}
static void checkHttps(void) {
    mark("net.https", RUNNING);
    if (ssl_module < 0 || http_module < 0) {
        say("net https: the system HTTP/SSL modules could not be loaded (a bundled TLS library is the fallback)");
        mark("net.https", INFO);
        return;
    }
    if (!runWithLimit(httpsBody, 30, "net.https")) mark("net.https", INFO);
}

// ---- the client: copied from the title folder (dev builds) into the title storage, where it can write next to itself ------------------
#define CLIENT_SOURCE "/app0/client"
#define ROOT "/download0/root"
#define GAME ROOT "/game"
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
static bool installClient(void) {
    mark("client.install", RUNNING);
    char source_revision[64] = "", installed_revision[64] = "";
    if (!readSmall(CLIENT_SOURCE "/revision.txt", source_revision, sizeof(source_revision))) {
        if (readSmall(GAME "/revision.txt", installed_revision, sizeof(installed_revision))) {
            say("client install: no client in the title folder; using the installed revision %s", installed_revision);
            noteRevision(installed_revision);
            mark("client.install", INFO);
            return true;
        }
        say("client install: no client found. Developer builds: run the installer with --client <PokeMMO-Client.zip>");
        mark("client.install", FAIL);
        return false;
    }
    readSmall(GAME "/revision.txt", installed_revision, sizeof(installed_revision));
    noteRevision(source_revision);
    if (!strcmp(source_revision, installed_revision)) {
        say("client install: revision %s already installed", installed_revision);
        mark("client.install", PASS);
        return true;
    }
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
        mark("client.install", FAIL);
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
            snprintf(to, sizeof(to), GAME "/%s", relative);
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
    if (ok) ok = copyFile(CLIENT_SOURCE "/revision.txt", GAME "/revision.txt", buffer, buffer_size);
    free(buffer);
    free(manifest);
    say("client install: %s, %u files, %llu MiB in %.1f s", ok ? "done" : "FAILED", files, bytes >> 20, (double)(platformMonotonicNs() - start) / 1e9);
    mark("client.install", ok ? PASS : FAIL);
    return ok;
}

// ---- the game's settings: a copy where FTP can see it ---------------------------------------------------------------------------
// The client keeps its settings in config/main.properties next to itself, in the title storage (not visible over FTP). A copy
// goes to the title folder at start and whenever the file changes (checked every minute): a backup, and the way to read it.
#define SETTINGS GAME "/config/main.properties"
#define SETTINGS_COPY "/app0/settings/main.properties"
static void backupSettings(void) {
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

// ---- the client run -----------------------------------------------------------------------------------------------------------------
static _Atomic bool game_finished, release_screen, screen_released;
static _Atomic uint64_t client_started_ns;
static _Atomic unsigned rom_count;
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
    GameConfig config = {.root = ROOT,
                         .client_path = GAME "/bin/linux/x64/PokeMMO",
                         // The C++ runtime the client's native libraries need (libstdc++, libgcc_s) ships with the title.
                         .mounts = {{"/game/roms", "/app0/roms"}, {"/lib", "/app0/assets/lib"}},
                         .mount_count = 2,
                         .arguments = options,
                         .timeout_seconds = 0,
                         .virtual_libraries = virtual_libraries,
                         .virtual_count = 8};
    mkdir(ROOT, 0755);
    bool ok = gameRun(&config);
    say("client: %s%s", ok ? "ended normally" : "ended: ", ok ? "" : gameFailure());
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
                          .log_path = "/data/homebrew/" PROSPERO_TITLE_ID "/prospero.log", .steps = step_view, .step_count = STEP_COUNT, .frame = frame};
    detail[0] = 0;
    static const char *const checks[] = {"fs.list.app0", "fs.list.roms", "fs.romread", "sys.modules", "net.https"};
    unsigned checked = 0;
    for (unsigned i = 0; i < 5; ++i) checked += stepState(checks[i]) != NOT_RUN && stepState(checks[i]) != RUNNING;
    view->fraction = 0.08f * (float)checked / 5.0f;
    if (checked < 5) view->status = checked < 3 ? "Checking files" : "Checking the system";
    int install = stepState("client.install");
    if (install == RUNNING) {
        unsigned long long done = atomic_load(&install_done), total = atomic_load(&install_total);
        view->status = "Installing PokeMMO";
        if (total) {
            view->fraction = 0.08f + 0.52f * (float)((double)done / (double)total);
            snprintf(detail, detail_size, "%llu of %llu MB", done >> 20, total >> 20);
        }
    } else if (install == PASS || install == INFO) {
        uint64_t started = atomic_load(&client_started_ns);
        float seconds = started ? (float)((double)(platformMonotonicNs() - started) / 1e9) : 0.0f;
        view->fraction = 0.6f + 0.38f * (1.0f - expf(-seconds / 20.0f));  // the client's start has no progress to report: an easing guess
        view->status = "Starting PokeMMO";
    }
    if (stepState("fs.list.app0") == FAIL) {
        view->problem = "The title's files cannot be read";
        view->advice = "Run the installer on your computer again.";
    } else if (install == FAIL) {
        view->problem = "PokeMMO is not installed yet";
        view->advice = "Run the installer on your computer and pick this console (developer builds: --client PokeMMO-Client.zip).";
    } else if (atomic_load(&game_finished) || atomic_load(&fatal_signals)) {
        view->problem = "PokeMMO stopped while starting";
        view->advice = "Close the title with the PS button and start it again. If it keeps happening, send prospero.log from the title folder (FTP).";
    }
    if (stepState("fs.romread") == INFO && !atomic_load(&rom_count)) view->warning = "No ROMs found: add them with the installer to play.";
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
    view.details = platformPadRead(&pad) && (pad.buttons & PLATFORM_PAD_TRIANGLE);
    loadingScreenDraw(&view);
    overlayEnd();
}

// The checks and the client start run behind the screen.
static void *workMain(void *argument) {
    (void)argument;
    static char rom_names[32][256];
    listFolder("fs.list.app0", "/app0", NULL, 0);
    unsigned roms = listFolder("fs.list.roms", "/app0/roms", rom_names, 32);
    atomic_store(&rom_count, roms);
    checkRoms(rom_names, roms > 32 ? 32 : roms);
    checkModules();
    checkHttps();
    if (installClient()) {
        pthread_t game;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 1u << 20);
        if (!pthread_create(&game, &attributes, gameThread, NULL)) pthread_detach(game);
        pthread_attr_destroy(&attributes);
    }
    return NULL;
}

int main(void) {
    say("PokeMMO-Prospero %s (build %s, %s) starting; UDP log port 18194", LOADER_MILESTONE, PROSPERO_VERSION, PROSPERO_TITLE_ID);
    guardInstall();
    signal(SIGPIPE, SIG_IGN);  // a write to a closed socket or pipe must fail with EPIPE, not end the title
    bool screen = screenOpen();
    say("screen: %s %dx%d", screen ? "ready" : "unavailable", width, height);
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
        bool install_failed = stepState("client.install") == FAIL;
        if ((atomic_load(&game_finished) || install_failed) && !reported) {
            reported = true;
            say("DONE. Close the title with the PS button. The full log is above and in /data/homebrew/" PROSPERO_TITLE_ID "/prospero.log (FTP).");
        }
        sceKernelUsleep(16000);
    }
}
