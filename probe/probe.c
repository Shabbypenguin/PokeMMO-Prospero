// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// PokeMMO-Prospero hardware probe.
//
// Answers, on the user's own console and firmware, the questions the loader design depends on:
//   gl     OpenGL 2.1 request without a profile mask (exactly what the client asks for) -> compatibility context,
//          legacy GLSL (no #version / 120 / 130, gl_FragColor, texture2D), client-side arrays, VBO without VAO,
//          alpha blending, immediate mode, readback.
//   tls    what %fs:0x28 holds (the client's 129 stack-canary checks read it) and whether it stays stable.
//   thread pthread stack size control and main-thread stack bounds (GraalVM needs pthread_getattr_np).
//   vm     flexible memory budget, large PROT_NONE reservations (GraalVM heap), MAP_FIXED commits inside them.
//   fs     writable /download0; ROM routes: next to the title (/app0/roms), a /data folder, USB drives.
//   net    DNS and TCP to the PokeMMO servers.
//   exec   executable memory: RWX mmap, RW->RX mprotect, JIT shared memory (libffi closure trampolines).
//   input  after the checks: a live controller tester (every press, stick and touch is logged and lit on screen),
//          and Triangle opens the system keyboard (IME dialog) to test text entry for login and chat.
//
// Results go to UDP (broadcast and an optional host baked in at build time), /download0/probe.log and the
// screen: one tile per check, green = pass, red = fail, grey = not run. Risky checks log "BEGIN <name>"
// first, so if the console kills the title the log shows where.

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <pthread_np.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

// ---- platform declarations not in public headers ----------------------------------------------
int sceKernelUsleep(unsigned int microseconds);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelJitCreateSharedMemory(const char *name, size_t length, int max_protection, int *fd);
int sceKernelJitCreateAliasOfSharedMemory(int fd, int max_protection, int *alias_fd);
int sceKernelJitMapSharedMemory(int fd, int protection, void **address);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment, int memory_type,
                                      int64_t *physical_start);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t physical_start, size_t alignment);
int32_t sceKernelReleaseDirectMemory(int64_t physical_start, size_t length);
#define SCE_KERNEL_MAP_FIXED 0x10
#define SCE_KERNEL_WB_ONION 12  // CPU-cached; the type ps5-opengl's app heap uses
int sceKernelLoadStartModule(const char *path, size_t argument_size, const void *arguments, uint32_t flags, void *options,
                             int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
int sceUserServiceInitialize(void *parameters);
int sceUserServiceGetInitialUser(int *user);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *parameters);


#define PROBE_PORT 18194
#define PROBE_VERSION "probe-2"

// ---- result tiles -----------------------------------------------------------------------------
enum { NOT_RUN = 0, PASS = 1, FAIL = 2, INFO = 3 };
typedef struct {
    const char *name;
    int state;
} Check;
static Check checks[] = {
    {"gl.context", NOT_RUN},  {"gl.compat", NOT_RUN},   {"gl.glsl110", NOT_RUN}, {"gl.glsl120", NOT_RUN},
    {"gl.glsl130", NOT_RUN},  {"gl.clientarr", NOT_RUN}, {"gl.vbo-novao", NOT_RUN}, {"gl.blend", NOT_RUN},
    {"gl.immediate", NOT_RUN}, {"tls.fs28", NOT_RUN},   {"thread.stack", NOT_RUN}, {"thread.getattr", NOT_RUN},
    {"vm.reserve", NOT_RUN},  {"vm.fixed", NOT_RUN},    {"vm.commit", NOT_RUN},  {"vm.direct", NOT_RUN},  {"vm.directfixed", NOT_RUN}, {"fs.download0", NOT_RUN}, {"fs.app0roms", NOT_RUN},
    {"fs.dataroms", NOT_RUN}, {"fs.usb", NOT_RUN},
    {"net.dns", NOT_RUN},     {"net.tcp", NOT_RUN},     {"exec.rwx", NOT_RUN},   {"exec.mprotect", NOT_RUN},
    {"exec.jit", NOT_RUN},    {"input.pad", NOT_RUN},   {"input.ime", NOT_RUN},
};
#define CHECK_COUNT (sizeof(checks) / sizeof(*checks))
static void mark(const char *name, int state);

// ---- logging: UDP broadcast + optional unicast host + file -------------------------------------
static int log_socket = -1;
static struct sockaddr_in log_broadcast, log_host;
static bool log_has_host;
static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void logInit(void) {
    log_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (log_socket >= 0) {
        int on = 1;
        setsockopt(log_socket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        memset(&log_broadcast, 0, sizeof(log_broadcast));
        log_broadcast.sin_family = AF_INET;
        log_broadcast.sin_port = htons(PROBE_PORT);
        log_broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }
    FILE *host = fopen("/app0/assets/loghost.txt", "r");
    if (host) {
        char text[64] = {0};
        if (fgets(text, sizeof(text), host)) {
            text[strcspn(text, " \r\n")] = 0;
            memset(&log_host, 0, sizeof(log_host));
            log_host.sin_family = AF_INET;
            log_host.sin_port = htons(PROBE_PORT);
            log_has_host = inet_pton(AF_INET, text, &log_host.sin_addr) == 1;
        }
        fclose(host);
    }
    log_file = fopen("/download0/probe.log", "w");
}

static void say(const char *format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((size_t)length > sizeof(line) - 2) length = sizeof(line) - 2;
    line[length++] = '\n';
    line[length] = 0;
    pthread_mutex_lock(&log_lock);
    fputs(line, stdout);
    fflush(stdout);
    if (log_file) {
        fputs(line, log_file);
        fflush(log_file);
        fsync(fileno(log_file));
    }
    if (log_socket >= 0) {
        sendto(log_socket, line, (size_t)length, 0, (struct sockaddr *)&log_broadcast, sizeof(log_broadcast));
        if (log_has_host) sendto(log_socket, line, (size_t)length, 0, (struct sockaddr *)&log_host, sizeof(log_host));
    }
    pthread_mutex_unlock(&log_lock);
}

static void mark(const char *name, int state) {
    for (size_t i = 0; i < CHECK_COUNT; ++i)
        if (!strcmp(checks[i].name, name)) checks[i].state = state;
    say("RESULT %s %s", name, state == PASS ? "PASS" : state == FAIL ? "FAIL" : state == INFO ? "INFO" : "NOT_RUN");
}

// ---- fault guard: risky reads/executions longjmp back instead of killing the title -------------
static sigjmp_buf guard_point;
static volatile sig_atomic_t guard_armed, guard_signal;
static void guardHandler(int signal_number) {
    if (guard_armed) {
        guard_signal = signal_number;
        guard_armed = 0;
        siglongjmp(guard_point, 1);
    }
    _exit(128 + signal_number);
}
static void guardInstall(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = guardHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER;
    int rc_segv = sigaction(SIGSEGV, &action, NULL), rc_bus = sigaction(SIGBUS, &action, NULL);
    int rc_ill = sigaction(SIGILL, &action, NULL);
    say("guard sigaction segv=%d bus=%d ill=%d", rc_segv, rc_bus, rc_ill);
}
#define GUARDED(ok_expression)                                                                      \
    (guard_signal = 0, sigsetjmp(guard_point, 1) == 0 ? (guard_armed = 1, (ok_expression), guard_armed = 0, 1) \
                                                      : 0)

static double now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

// ---- system information --------------------------------------------------------------------------
static void probeSystem(void) {
    // uname() is not exported to titles; ask sysctl directly.
    char text[256];
    size_t length;
    static const char *const names[] = {"kern.ostype", "kern.osrelease", "kern.version", "hw.model"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        length = sizeof(text) - 1;
        if (!sysctlbyname(names[i], text, &length, NULL, 0)) {
            text[length] = 0;
            text[strcspn(text, "\n")] = 0;
            say("%s %s", names[i], text);
        } else
            say("%s unavailable errno=%d", names[i], errno);
    }
    int cpus = 0;
    length = sizeof(cpus);
    if (!sysctlbyname("hw.ncpu", &cpus, &length, NULL, 0)) say("hw.ncpu %d", cpus);
    unsigned long physical = 0;
    length = sizeof(physical);
    if (!sysctlbyname("hw.physmem", &physical, &length, NULL, 0)) say("hw.physmem %lu MiB", physical >> 20);
    say("sysconf pagesize=%ld nprocessors_onln=%ld", sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
    size_t flexible = 0;
    int rc = sceKernelAvailableFlexibleMemorySize(&flexible);
    say("flexible memory available rc=0x%x size=%zu MiB", rc, flexible >> 20);
    say("pid=%d uid=%d", (int)getpid(), (int)getuid());
}

// ---- %fs:0x28 -----------------------------------------------------------------------------------
static uint64_t readFs(unsigned offset) {
    uint64_t value = 0;
    switch (offset) {
    case 0x00: __asm__ volatile("movq %%fs:0x00, %0" : "=r"(value)); break;
    case 0x08: __asm__ volatile("movq %%fs:0x08, %0" : "=r"(value)); break;
    case 0x10: __asm__ volatile("movq %%fs:0x10, %0" : "=r"(value)); break;
    case 0x18: __asm__ volatile("movq %%fs:0x18, %0" : "=r"(value)); break;
    case 0x20: __asm__ volatile("movq %%fs:0x20, %0" : "=r"(value)); break;
    case 0x28: __asm__ volatile("movq %%fs:0x28, %0" : "=r"(value)); break;
    case 0x30: __asm__ volatile("movq %%fs:0x30, %0" : "=r"(value)); break;
    default: break;
    }
    return value;
}

typedef struct {
    int index;
    uint64_t first, last;
    unsigned changes, samples;
    bool faulted;
} FsSample;

static void *fsWorker(void *argument) {
    FsSample *sample = argument;
    uint64_t value = 0;
    if (!GUARDED(value = readFs(0x28))) {
        sample->faulted = true;
        return NULL;
    }
    sample->first = sample->last = value;
    // Mix work that touches the thread's runtime state (allocation, sleeping, stdio) between samples.
    for (unsigned i = 0; i < 400; ++i) {
        void *block = malloc(64 + (i * 37) % 4096);
        free(block);
        if (i % 50 == 0) sceKernelUsleep(5000);
        uint64_t current = readFs(0x28);
        if (current != sample->last) ++sample->changes;
        sample->last = current;
        ++sample->samples;
    }
    return NULL;
}

static void probeFs28(void) {
    say("BEGIN tls.fs28");
    uint64_t words[7];
    bool ok = true;
    for (unsigned i = 0; i < 7 && ok; ++i)
        ok = GUARDED(words[i] = readFs(i * 8));
    if (!ok) {
        say("tls main thread %%fs read faulted signal=%d", (int)guard_signal);
        mark("tls.fs28", FAIL);
        return;
    }
    say("tls main fs:00=%#lx 08=%#lx 10=%#lx 18=%#lx 20=%#lx 28=%#lx 30=%#lx", words[0], words[1], words[2], words[3], words[4],
        words[5], words[6]);
    uint64_t before = words[5];
    // Allocation pressure and thread creation on the main thread, then re-read.
    for (unsigned i = 0; i < 2000; ++i) free(malloc(16 + i));
    FsSample samples[4];
    pthread_t threads[4];
    memset(samples, 0, sizeof(samples));
    for (int i = 0; i < 4; ++i) {
        samples[i].index = i;
        pthread_create(&threads[i], NULL, fsWorker, &samples[i]);
    }
    unsigned main_changes = 0;
    uint64_t last = before;
    for (unsigned i = 0; i < 200; ++i) {
        free(malloc(128 + i));
        uint64_t current = readFs(0x28);
        if (current != last) ++main_changes;
        last = current;
        if (i % 40 == 0) sceKernelUsleep(5000);
    }
    bool stable = main_changes == 0;
    for (int i = 0; i < 4; ++i) {
        pthread_join(threads[i], NULL);
        say("tls thread%d fs:28 first=%#lx last=%#lx changes=%u samples=%u faulted=%d", i, samples[i].first, samples[i].last,
            samples[i].changes, samples[i].samples, samples[i].faulted);
        if (samples[i].faulted || samples[i].changes) stable = false;
    }
    say("tls main fs:28 before=%#lx after=%#lx changes=%u", before, last, main_changes);
    mark("tls.fs28", stable ? PASS : FAIL);
}

// ---- threads --------------------------------------------------------------------------------------
static void *stackWorker(void *argument) {
    volatile char deep[512 * 1024];  // touch half a MiB of stack: fails on a small default stack
    memset((void *)deep, 1, sizeof(deep));
    *(int *)argument = deep[1000] == 1;
    return NULL;
}

static void probeThreads(void) {
    say("BEGIN thread.stack");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    int rc_size = pthread_attr_setstacksize(&attributes, 8u << 20);
    pthread_t thread;
    int touched = 0;
    int rc_create = pthread_create(&thread, &attributes, stackWorker, &touched);
    if (!rc_create) pthread_join(thread, NULL);
    pthread_attr_destroy(&attributes);
    say("thread setstacksize(8MiB) rc=%d create rc=%d touched=%d", rc_size, rc_create, touched);
    mark("thread.stack", !rc_size && !rc_create && touched ? PASS : FAIL);

    say("BEGIN thread.getattr");
    pthread_attr_t self;
    pthread_attr_init(&self);
    int rc_get = pthread_attr_get_np(pthread_self(), &self);
    void *base = NULL;
    size_t size = 0, guard = 0;
    int rc_stack = rc_get ? -1 : pthread_attr_getstack(&self, &base, &size);
    int rc_guard = rc_get ? -1 : pthread_attr_getguardsize(&self, &guard);
    int local = 0;
    bool inside = base && (char *)&local >= (char *)base && (char *)&local < (char *)base + size;
    say("thread main get_np rc=%d getstack rc=%d base=%p size=%zu KiB guard rc=%d size=%zu local=%p inside=%d", rc_get, rc_stack,
        base, size >> 10, rc_guard, guard, (void *)&local, inside);
    pthread_attr_destroy(&self);
    mark("thread.getattr", !rc_get && !rc_stack && inside ? PASS : FAIL);
}

// ---- virtual memory ------------------------------------------------------------------------------
static void probeVm(void) {
    say("BEGIN vm.reserve");
    static const unsigned gib[] = {1, 4, 16, 32, 64};
    unsigned largest = 0;
    for (size_t i = 0; i < sizeof(gib) / sizeof(*gib); ++i) {
        size_t bytes = (size_t)gib[i] << 30;
        void *area = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        say("vm reserve %u GiB PROT_NONE -> %p errno=%d", gib[i], area == MAP_FAILED ? NULL : area, area == MAP_FAILED ? errno : 0);
        if (area != MAP_FAILED) {
            largest = gib[i];
            munmap(area, bytes);
        }
    }
    mark("vm.reserve", largest >= 4 ? PASS : FAIL);

    say("BEGIN vm.fixed");
    size_t reserve = (size_t)4 << 30;
    char *area = mmap(NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    bool fixed_ok = false;
    if (area != MAP_FAILED) {
        char *chunk = area + ((size_t)1 << 30);
        void *mapped = mmap(chunk, 64u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        int mapped_errno = mapped == MAP_FAILED ? errno : 0;
        bool touched = false;
        if (mapped == chunk) touched = GUARDED(memset(chunk, 0x5a, 64u << 20));
        int rc_none = mprotect(chunk, 64u << 20, PROT_NONE);
        int rc_back = mprotect(chunk, 64u << 20, PROT_READ | PROT_WRITE);
        volatile char sample = 0;
        bool kept = rc_back == 0 && GUARDED(sample = chunk[12345]) && sample == 0x5a;
        int rc_unmap = munmap(area, reserve);
        say("vm fixed-in-reservation map=%p errno=%d touched=%d mprotect none=%d rw=%d contents-kept=%d munmap=%d",
            mapped == MAP_FAILED ? NULL : mapped, mapped_errno, touched, rc_none, rc_back, kept, rc_unmap);
        fixed_ok = mapped == chunk && touched && !rc_none && !rc_back && kept;
    } else
        say("vm fixed: 4 GiB reservation failed errno=%d", errno);
    mark("vm.fixed", fixed_ok ? PASS : FAIL);

    say("BEGIN vm.commit");
    enum { CHUNK = 64u << 20, MAX_CHUNKS = 40 };  // up to 2.5 GiB
    void *chunks[MAX_CHUNKS];
    unsigned count = 0;
    double start = now();
    while (count < MAX_CHUNKS) {
        void *chunk = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (chunk == MAP_FAILED) {
            say("vm commit stopped at %u MiB errno=%d", count * 64, errno);
            break;
        }
        for (size_t offset = 0; offset < CHUNK; offset += 16384) ((volatile char *)chunk)[offset] = 1;
        chunks[count++] = chunk;
    }
    size_t flexible = 0;
    sceKernelAvailableFlexibleMemorySize(&flexible);
    say("vm committed+touched %u MiB in %.2fs; flexible left %zu MiB", count * 64, now() - start, flexible >> 20);
    for (unsigned i = 0; i < count; ++i) munmap(chunks[i], CHUNK);
    // Anonymous mmap draws on flexible memory (a few hundred MiB per ps5-opengl); report it, the verdict comes from
    // direct memory below, which is what the loader will use for the Java heap if flexible memory is too small.
    mark("vm.commit", count * 64 >= 1024 ? PASS : INFO);

    say("BEGIN vm.direct");
    int64_t direct_total = sceKernelGetDirectMemorySize();
    enum { DIRECT_CHUNK = 256u << 20, DIRECT_MAX = 8 };  // up to 2 GiB
    int64_t physical[DIRECT_MAX];
    void *mapped[DIRECT_MAX];
    unsigned direct = 0;
    for (; direct < DIRECT_MAX; ++direct) {
        int rc_allocate = sceKernelAllocateDirectMemory(0, direct_total, DIRECT_CHUNK, 2u << 20, SCE_KERNEL_WB_ONION,
                                                        &physical[direct]);
        mapped[direct] = NULL;
        int rc_map = rc_allocate ? -1
                                 : sceKernelMapDirectMemory(&mapped[direct], DIRECT_CHUNK, PROT_READ | PROT_WRITE, 0,
                                                            physical[direct], 2u << 20);
        if (rc_allocate || rc_map || !mapped[direct]) {
            say("vm direct stopped at %u MiB allocate rc=0x%x map rc=0x%x", direct * 256, rc_allocate, rc_map);
            if (!rc_allocate) sceKernelReleaseDirectMemory(physical[direct], DIRECT_CHUNK);
            break;
        }
        bool touched = GUARDED(memset(mapped[direct], 0x33, DIRECT_CHUNK));
        if (!touched) {
            say("vm direct chunk %u not writable signal=%d", direct, (int)guard_signal);
            munmap(mapped[direct], DIRECT_CHUNK);
            sceKernelReleaseDirectMemory(physical[direct], DIRECT_CHUNK);
            break;
        }
    }
    say("vm direct memory size=%lld MiB; allocated+mapped+touched %u MiB", (long long)(direct_total >> 20), direct * 256);
    for (unsigned i = 0; i < direct; ++i) {
        munmap(mapped[i], DIRECT_CHUNK);
        sceKernelReleaseDirectMemory(physical[i], DIRECT_CHUNK);
    }
    mark("vm.direct", direct * 256 >= 1024 ? PASS : FAIL);

    // GraalVM reserves its heap range first and commits chunks inside it: direct memory at a fixed address.
    say("BEGIN vm.directfixed");
    size_t range = (size_t)1 << 30;
    char *reservation = mmap(NULL, range, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    bool fixed_direct = false;
    if (reservation != MAP_FAILED) {
        char *aligned = (char *)(((uintptr_t)reservation + (64u << 20) + (2u << 20) - 1) & ~(uintptr_t)((2u << 20) - 1));
        int64_t phys = 0;
        void *where = aligned;
        int rc_allocate = sceKernelAllocateDirectMemory(0, direct_total, 64u << 20, 2u << 20, SCE_KERNEL_WB_ONION, &phys);
        int rc_map = rc_allocate ? -1
                                 : sceKernelMapDirectMemory(&where, 64u << 20, PROT_READ | PROT_WRITE, SCE_KERNEL_MAP_FIXED, phys,
                                                            2u << 20);
        bool touched = !rc_map && where == aligned && GUARDED(memset(aligned, 0x44, 64u << 20));
        say("vm direct MAP_FIXED inside mmap reservation: allocate=0x%x map=0x%x want=%p got=%p touched=%d", rc_allocate, rc_map,
            (void *)aligned, where, touched);
        fixed_direct = touched;
        munmap(reservation, range);
        if (!rc_allocate) sceKernelReleaseDirectMemory(phys, 64u << 20);
    } else
        say("vm direct fixed: reservation failed errno=%d", errno);
    mark("vm.directfixed", fixed_direct ? PASS : FAIL);
}

// ---- filesystem -----------------------------------------------------------------------------------
// The installer uploads ROMs to /data/homebrew/<TITLE_ID>/roms/, which the title mounter exposes read-only as
// /app0/roms. Check that the files show up there and can be read end to end (PASS), or report that no ROMs were
// uploaded (INFO). The client only reads ROMs; its caches go to /download0.
static void probeAppRoms(void) {
    say("BEGIN fs.app0roms");
    DIR *directory = opendir("/app0/roms");
    if (!directory) {
        say("fs /app0/roms not present errno=%d (upload ROMs with the installer to test this path)", errno);
        mark("fs.app0roms", INFO);
        return;
    }
    unsigned files = 0, readable = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "/app0/roms/%s", entry->d_name);
        struct stat info;
        if (stat(path, &info) || !S_ISREG(info.st_mode)) continue;
        ++files;
        int fd = open(path, O_RDONLY);
        long long total = 0;
        unsigned char header[16] = {0};
        if (fd >= 0) {
            static char block[1 << 16];
            ssize_t got;
            bool first = true;
            while ((got = read(fd, block, sizeof(block))) > 0) {
                if (first) memcpy(header, block, got < 16 ? (size_t)got : 16), first = false;
                total += got;
            }
            close(fd);
        }
        bool ok = fd >= 0 && total == (long long)info.st_size;
        readable += ok;
        say("fs rom %s size=%lld read=%lld ok=%d header=%02x%02x%02x%02x", entry->d_name, (long long)info.st_size, total, ok,
            header[0], header[1], header[2], header[3]);
    }
    closedir(directory);
    int probe_fd = open("/app0/roms/.write-test", O_WRONLY | O_CREAT, 0644);
    say("fs /app0/roms files=%u readable=%u writable=%d (expected read-only)", files, readable, probe_fd >= 0);
    if (probe_fd >= 0) {
        close(probe_fd);
        unlink("/app0/roms/.write-test");
    }
    mark("fs.app0roms", files == 0 ? INFO : readable == files ? PASS : FAIL);
}

// Reads every .nds/.gba (any regular file when any_file) in a directory, one level deep. Returns how many files were
// found and how many could be read end to end; *open_errno is the opendir error (0 when the directory opened).
static void readRomDir(const char *directory_path, bool any_file, unsigned *found, unsigned *readable, int *open_errno) {
    *found = *readable = 0;
    *open_errno = 0;
    DIR *directory = opendir(directory_path);
    if (!directory) {
        *open_errno = errno;
        return;
    }
    struct dirent *entry;
    unsigned listed = 0;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", directory_path, entry->d_name);
        struct stat info;
        if (stat(path, &info)) continue;
        if (listed++ < 12) say("fs   %s/%s%s size=%lld", directory_path, entry->d_name, S_ISDIR(info.st_mode) ? "/" : "", (long long)info.st_size);
        size_t length = strlen(entry->d_name);
        bool rom = length > 4 && (!strcasecmp(entry->d_name + length - 4, ".nds") || !strcasecmp(entry->d_name + length - 4, ".gba"));
        if (!S_ISREG(info.st_mode) || !(rom || any_file)) continue;
        ++*found;
        int fd = open(path, O_RDONLY);
        long long total = 0;
        if (fd >= 0) {
            static char block[1 << 16];
            ssize_t got;
            while ((got = read(fd, block, sizeof(block))) > 0) total += got;
            close(fd);
        }
        if (fd >= 0 && total == (long long)info.st_size) ++*readable;
        else say("fs   could not read %s fully (open errno=%d, read %lld of %lld)", path, fd < 0 ? errno : 0, total, (long long)info.st_size);
    }
    closedir(directory);
}

// Route 2: ROMs in a /data folder, read directly. Uses the folder the installer uploads to (/data/homebrew/<ID>/roms),
// so no extra upload is needed, plus the dedicated folder an image-based install would use.
static void probeDataRoms(void) {
    say("BEGIN fs.dataroms");
    static const char *const directories[] = {"/data/homebrew/PPSA27165/roms", "/data/pokemmo-prospero/roms"};
    bool denied = false, worked = false;
    for (size_t i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
        unsigned found, readable;
        int open_errno;
        readRomDir(directories[i], false, &found, &readable, &open_errno);
        say("fs %s opendir errno=%d roms=%u readable=%u", directories[i], open_errno, found, readable);
        if (open_errno == EACCES || open_errno == EPERM) denied = true;
        if (!open_errno && found && readable == found) worked = true;
    }
    DIR *data = opendir("/data");
    say("fs opendir(/data) %s errno=%d", data ? "ok" : "failed", data ? 0 : errno);
    if (data) closedir(data);
    mark("fs.dataroms", worked ? PASS : denied ? FAIL : INFO);  // INFO: nothing there to read (or no ROMs uploaded)
}

// Route 3: USB drives. Lists /mnt/usb0..7 and reads any ROMs found at their top level.
static void probeUsb(void) {
    say("BEGIN fs.usb");
    bool present = false, denied = false, worked = false;
    for (int index = 0; index < 8; ++index) {
        char mount[32];
        snprintf(mount, sizeof(mount), "/mnt/usb%d", index);
        unsigned found, readable;
        int open_errno;
        readRomDir(mount, false, &found, &readable, &open_errno);
        if (open_errno == ENOENT || open_errno == ENOTDIR) continue;
        present = true;
        say("fs %s opendir errno=%d roms=%u readable=%u", mount, open_errno, found, readable);
        if (open_errno) denied = true;
        else worked = true;  // the drive is readable even without ROMs on it
    }
    if (!present) say("fs no /mnt/usbN visible (no drive plugged in, or the sandbox hides them)");
    mark("fs.usb", worked ? PASS : denied ? FAIL : INFO);
}

static void probeFiles(void) {
    say("BEGIN fs.download0");
    bool ok = mkdir("/download0/probe-dir", 0755) == 0 || errno == EEXIST;
    int fd = ok ? open("/download0/probe-dir/test.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    ok = fd >= 0 && write(fd, "pokemmo", 7) == 7 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename("/download0/probe-dir/test.tmp", "/download0/probe-dir/test.bin") == 0;
    struct stat info;
    ok = ok && stat("/download0/probe-dir/test.bin", &info) == 0 && info.st_size == 7;
    unlink("/download0/probe-dir/test.bin");
    rmdir("/download0/probe-dir");
    say("fs download0 write/rename/stat ok=%d errno=%d", ok, errno);
    mark("fs.download0", ok ? PASS : FAIL);
    struct stat data;
    int rc_data = stat("/data", &data);
    int data_fd = open("/data/.pokemmo-prospero-probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    say("fs /data stat=%d writable=%d (info only: titles are sandboxed by default)", rc_data, data_fd >= 0);
    if (data_fd >= 0) {
        close(data_fd);
        unlink("/data/.pokemmo-prospero-probe");
    }
}

// ---- network --------------------------------------------------------------------------------------
static void probeNetwork(void) {
    say("BEGIN net.dns");
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo("pokemmo.com", "443", &hints, &result);
    char address[64] = "?";
    if (!rc && result) inet_ntop(AF_INET, &((struct sockaddr_in *)result->ai_addr)->sin_addr, address, sizeof(address));
    say("net getaddrinfo(pokemmo.com) rc=%d addr=%s", rc, address);
    mark("net.dns", !rc && result ? PASS : FAIL);
    if (rc || !result) return;

    say("BEGIN net.tcp");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval timeout = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    double start = now();
    int rc_connect = fd >= 0 ? connect(fd, result->ai_addr, result->ai_addrlen) : -1;
    say("net tcp connect pokemmo.com:443 rc=%d errno=%d %.0f ms", rc_connect, rc_connect ? errno : 0, (now() - start) * 1000);
    if (fd >= 0) close(fd);
    freeaddrinfo(result);
    mark("net.tcp", !rc_connect ? PASS : FAIL);
}

// ---- executable memory ----------------------------------------------------------------------------
static const unsigned char return_42[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};  // mov eax, 42; ret

static int callCode(void *code) {
    int value = -1;
    if (!GUARDED(value = ((int (*)(void))code)())) {
        say("  executing faulted signal=%d", (int)guard_signal);
        return -1;
    }
    return value;
}

static void probeExec(void) {
    say("BEGIN exec.rwx");
    void *rwx = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    say("exec rwx mmap -> %p errno=%d", rwx == MAP_FAILED ? NULL : rwx, rwx == MAP_FAILED ? errno : 0);
    int value = -1;
    if (rwx != MAP_FAILED && GUARDED(memcpy(rwx, return_42, sizeof(return_42)))) value = callCode(rwx);
    say("exec rwx call -> %d", value);
    if (rwx != MAP_FAILED) munmap(rwx, 16384);
    mark("exec.rwx", value == 42 ? PASS : FAIL);

    say("BEGIN exec.mprotect");
    void *page = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    value = -1;
    int rc_protect = -1;
    if (page != MAP_FAILED) {
        memcpy(page, return_42, sizeof(return_42));
        rc_protect = mprotect(page, 16384, PROT_READ | PROT_EXEC);
        say("exec mprotect(RX) rc=%d errno=%d", rc_protect, rc_protect ? errno : 0);
        if (!rc_protect) value = callCode(page);
        munmap(page, 16384);
    }
    say("exec mprotect call -> %d", value);
    mark("exec.mprotect", value == 42 ? PASS : FAIL);

    say("BEGIN exec.jit");
    int fd = -1, alias = -1;
    int rc_create = sceKernelJitCreateSharedMemory("pokemmo-ffi", 65536, PROT_READ | PROT_WRITE | PROT_EXEC, &fd);
    int rc_alias = rc_create ? -1 : sceKernelJitCreateAliasOfSharedMemory(fd, PROT_READ | PROT_WRITE, &alias);
    say("exec jit create rc=0x%x fd=%d alias rc=0x%x fd=%d", rc_create, fd, rc_alias, alias);
    value = -1;
    if (!rc_create && !rc_alias) {
        void *executable = mmap(NULL, 65536, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        void *writable = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, alias, 0);
        say("exec jit mmap rx=%p rw=%p errno=%d", executable == MAP_FAILED ? NULL : executable,
            writable == MAP_FAILED ? NULL : writable, errno);
        if (executable != MAP_FAILED && writable != MAP_FAILED) {
            memcpy(writable, return_42, sizeof(return_42));
            value = callCode(executable);
        }
        if (executable != MAP_FAILED) munmap(executable, 65536);
        if (writable != MAP_FAILED) munmap(writable, 65536);
    }
    if (alias >= 0) close(alias);
    if (fd >= 0) close(fd);
    say("exec jit call -> %d", value);
    mark("exec.jit", value == 42 ? PASS : FAIL);
}

// ---- OpenGL ----------------------------------------------------------------------------------------
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static EGLint width, height;

static bool glOpen(void) {
    say("BEGIN gl.context");
    // The same request the client makes through SDL: desktop GL, 2.1, no profile mask.
    static const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 2, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
    EGLConfig config;
    EGLint count = 0, major = 0, minor = 0;
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1) {
        say("gl egl setup failed error=0x%x", eglGetError());
        mark("gl.context", FAIL);
        return false;
    }
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
        say("gl context failed error=0x%x surface=%p context=%p", eglGetError(), surface, context);
        mark("gl.context", FAIL);
        return false;
    }
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    say("gl EGL %d.%d surface %dx%d", major, minor, width, height);
    say("gl GL_VERSION=%s", glGetString(GL_VERSION));
    say("gl GL_RENDERER=%s", glGetString(GL_RENDERER));
    say("gl GLSL=%s", glGetString(GL_SHADING_LANGUAGE_VERSION));
    mark("gl.context", PASS);
    GLint profile = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetError();
    say("gl context profile mask=0x%x (2 = compatibility)", profile);
    mark("gl.compat", profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT ? PASS : FAIL);
    return true;
}

static GLuint compileProgram(const char *label, const char *vertex, const char *fragment) {
    const char *sources[2] = {vertex, fragment};
    GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    GLuint program = glCreateProgram();
    for (int i = 0; i < 2; ++i) {
        GLuint shader = glCreateShader(types[i]);
        glShaderSource(shader, 1, &sources[i], NULL);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char text[512];
            GLsizei length = 0;
            glGetShaderInfoLog(shader, sizeof(text), &length, text);
            say("gl %s %s compile failed: %.*s", label, i ? "fragment" : "vertex", (int)length, text);
            glDeleteShader(shader);
            glDeleteProgram(program);
            return 0;
        }
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texCoord0");
    glBindAttribLocation(program, 2, "a_color");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char text[512];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(text), &length, text);
        say("gl %s link failed: %.*s", label, (int)length, text);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// libGDX SpriteBatch-style shaders, in the three dialects the client binary carries.
static const char sprite_vs_110[] = "attribute vec4 a_position;\nattribute vec4 a_color;\nattribute vec2 a_texCoord0;\n"
                                    "varying vec4 v_color;\nvarying vec2 v_texCoords;\n"
                                    "void main() { v_color = a_color; v_texCoords = a_texCoord0; gl_Position = a_position; }\n";
static const char sprite_fs_110[] = "#ifdef GL_ES\nprecision mediump float;\n#endif\nvarying vec4 v_color;\nvarying vec2 v_texCoords;\n"
                                    "uniform sampler2D u_texture;\n"
                                    "void main() { gl_FragColor = v_color * texture2D(u_texture, v_texCoords); }\n";
static const char sprite_vs_120[] = "#version 120\nattribute vec4 a_position;\nattribute vec4 a_color;\nattribute vec2 a_texCoord0;\n"
                                    "varying vec4 v_color;\nvarying vec2 v_texCoords;\n"
                                    "void main() { v_color = a_color; v_texCoords = a_texCoord0; gl_Position = a_position; }\n";
static const char sprite_fs_120[] = "#version 120\nvarying vec4 v_color;\nvarying vec2 v_texCoords;\nuniform sampler2D u_texture;\n"
                                    "void main() { gl_FragColor = v_color * texture2D(u_texture, v_texCoords); }\n";
static const char sprite_vs_130[] = "#version 130\nin vec4 a_position;\nin vec4 a_color;\nin vec2 a_texCoord0;\n"
                                    "out vec4 v_color;\nout vec2 v_texCoords;\n"
                                    "void main() { v_color = a_color; v_texCoords = a_texCoord0; gl_Position = a_position; }\n";
static const char sprite_fs_130[] = "#version 130\nin vec4 v_color;\nin vec2 v_texCoords;\nuniform sampler2D u_texture;\n"
                                    "void main() { gl_FragColor = v_color * texture(u_texture, v_texCoords); }\n";

// A quad covering the pixel rectangle [x, x+w) x [y, y+h) of the window (origin bottom-left).
static void quad(float *out, int x, int y, int w, int h) {
    float x0 = 2.0f * x / width - 1.0f, x1 = 2.0f * (x + w) / width - 1.0f;
    float y0 = 2.0f * y / height - 1.0f, y1 = 2.0f * (y + h) / height - 1.0f;
    // position(4) color(4) uv(2) per vertex, triangle strip
    const float v[4][10] = {{x0, y0, 0, 1, 1, 1, 1, 1, 0, 0}, {x1, y0, 0, 1, 1, 1, 1, 1, 1, 0},
                            {x0, y1, 0, 1, 1, 1, 1, 1, 0, 1}, {x1, y1, 0, 1, 1, 1, 1, 1, 1, 1}};
    memcpy(out, v, sizeof(v));
}

static bool pixelNear(int x, int y, int r, int g, int b) {
    unsigned char pixel[4] = {0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    bool near = abs(pixel[0] - r) <= 6 && abs(pixel[1] - g) <= 6 && abs(pixel[2] - b) <= 6;
    if (!near) say("gl   pixel(%d,%d)=%d,%d,%d expected %d,%d,%d", x, y, pixel[0], pixel[1], pixel[2], r, g, b);
    return near;
}

static void probeGl(void) {
    // A 1x1 texture: red with alpha 0.5, drawn over a green clear with standard alpha blending.
    GLuint texture = 0;
    const unsigned char red_half[4] = {255, 0, 0, 128};
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red_half);
    glViewport(0, 0, width, height);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    struct {
        const char *name, *vs, *fs;
    } dialects[] = {{"gl.glsl110", sprite_vs_110, sprite_fs_110}, {"gl.glsl120", sprite_vs_120, sprite_fs_120},
                    {"gl.glsl130", sprite_vs_130, sprite_fs_130}};
    GLuint programs[3];
    for (int i = 0; i < 3; ++i) {
        say("BEGIN %s", dialects[i].name);
        programs[i] = compileProgram(dialects[i].name, dialects[i].vs, dialects[i].fs);
        mark(dialects[i].name, programs[i] ? PASS : FAIL);
    }
    GLuint program = programs[0] ? programs[0] : programs[1] ? programs[1] : programs[2];
    if (!program) return;
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_texture"), 0);

    // Client-side vertex arrays (compatibility profile only), no blending: expect pure red.
    say("BEGIN gl.clientarr");
    float vertices[40];
    quad(vertices, 0, 0, 64, 64);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 40, vertices);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 40, vertices + 4);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 40, vertices + 8);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GLenum error = glGetError();
    say("gl client arrays draw error=0x%x", error);
    mark("gl.clientarr", !error && pixelNear(32, 32, 255, 0, 0) ? PASS : FAIL);

    // VBO with no VAO bound (default VAO 0), with alpha blending: expect ~50% red over green.
    say("BEGIN gl.vbo-novao");
    quad(vertices, 100, 0, 64, 64);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 40, (void *)0);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 40, (void *)16);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 40, (void *)32);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    error = glGetError();
    say("gl vbo draw error=0x%x", error);
    mark("gl.vbo-novao", !error ? PASS : FAIL);
    say("BEGIN gl.blend");
    mark("gl.blend", pixelNear(132, 32, 128, 127, 0) ? PASS : FAIL);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
    glUseProgram(0);
    glDisable(GL_BLEND);

    // Fixed-function immediate mode: a blue quad.
    say("BEGIN gl.immediate");
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex2f(2.0f * 200 / width - 1, -1);
    glVertex2f(2.0f * 264 / width - 1, -1);
    glVertex2f(2.0f * 264 / width - 1, 2.0f * 64 / height - 1);
    glVertex2f(2.0f * 200 / width - 1, 2.0f * 64 / height - 1);
    glEnd();
    error = glGetError();
    say("gl immediate error=0x%x", error);
    mark("gl.immediate", !error && pixelNear(232, 32, 0, 0, 255) ? PASS : FAIL);

    for (int i = 0; i < 3; ++i)
        if (programs[i]) glDeleteProgram(programs[i]);
    glDeleteTextures(1, &texture);
}

// ---- input: controller tester and system keyboard ------------------------------------------------------------
// Pad record as the PS4/PS5 pad library returns it: 120 bytes. The button/stick/connected fields are the ones
// ps5-opengl's TV demo uses; the trigger and touch fields follow the PS4 layout and are logged raw so a mismatch shows.
typedef struct __attribute__((aligned(8))) {
    uint32_t buttons;         // 0x00
    uint8_t lx, ly, rx, ry;   // 0x04 sticks, 128 = centre
    uint8_t l2, r2;           // 0x08 analog triggers
    uint8_t pad0[2];
    float orientation[4];     // 0x0c
    float acceleration[3];    // 0x1c
    float angular[3];         // 0x28
    uint8_t touch_count;      // 0x34
    uint8_t touch_pad[7];
    struct {
        uint16_t x, y;
        uint8_t id;
        uint8_t reserved[3];
    } touch[2];               // 0x3c
    int32_t connected;        // 0x4c
    uint8_t rest[40];
} PadState;
_Static_assert(sizeof(PadState) == 120, "pad record size");
_Static_assert(offsetof(PadState, connected) == 0x4c, "pad connection offset");
int scePadReadState(int handle, PadState *state);

static const struct {
    uint32_t mask;
    const char *name;
} pad_buttons[] = {{0x00004000, "CROSS"},    {0x00002000, "CIRCLE"}, {0x00008000, "SQUARE"}, {0x00001000, "TRIANGLE"},
                   {0x00000010, "UP"},       {0x00000040, "DOWN"},   {0x00000080, "LEFT"},   {0x00000020, "RIGHT"},
                   {0x00000400, "L1"},       {0x00000800, "R1"},     {0x00000100, "L2"},     {0x00000200, "R2"},
                   {0x00000002, "L3"},       {0x00000004, "R3"},     {0x00000008, "OPTIONS"}, {0x00100000, "TOUCHPAD"}};
#define PAD_BUTTON_COUNT (sizeof(pad_buttons) / sizeof(*pad_buttons))

static int pad_handle = -1;
static PadState pad_now;
static bool pad_have;

static void padOpen(void) {
    say("BEGIN input.pad");
    int user = -1;
    int rc_service = sceUserServiceInitialize(NULL);
    int rc_user = sceUserServiceGetInitialUser(&user);
    int rc_init = scePadInit();
    pad_handle = rc_user == 0 && rc_init >= 0 ? scePadOpen(user, 0, 0, NULL) : -1;
    say("input user service=0x%x user rc=0x%x id=%d pad init=0x%x open=0x%x", rc_service, rc_user, user, rc_init, pad_handle);
    say("input CONTROLLER TESTER: press every button, move both sticks, touch the touchpad; Triangle opens the keyboard");
    if (pad_handle < 0) mark("input.pad", FAIL);
}

// Called every frame. Logs edges and movement (throttled) and marks input.pad on the first press.
static uint32_t padPoll(void) {
    static uint32_t previous;
    static uint8_t last_lx = 128, last_ly = 128, last_rx = 128, last_ry = 128, last_touches;
    static unsigned touch_frames;
    static unsigned reads_failed;
    if (pad_handle < 0) return 0;
    PadState state;
    int rc = scePadReadState(pad_handle, &state);
    if (rc < 0) {
        if (reads_failed++ < 3) say("input scePadReadState rc=0x%x", rc);
        return 0;
    }
    pad_now = state;
    pad_have = true;
    uint32_t buttons = state.connected ? state.buttons & 0x7fffffffu : 0;
    uint32_t pressed = buttons & ~previous, released = previous & ~buttons;
    for (size_t i = 0; i < PAD_BUTTON_COUNT; ++i) {
        if (pressed & pad_buttons[i].mask) say("input press   %-8s (0x%08x)", pad_buttons[i].name, pad_buttons[i].mask);
        if (released & pad_buttons[i].mask) say("input release %s", pad_buttons[i].name);
    }
    uint32_t known = 0;
    for (size_t i = 0; i < PAD_BUTTON_COUNT; ++i) known |= pad_buttons[i].mask;
    if (pressed & ~known) say("input press   unknown bits 0x%08x", pressed & ~known);
    static bool proven;
    if (pressed && !proven) {  // the first real press proves the whole path
        proven = true;
        mark("input.pad", PASS);
    }
    #define MOVED(a, b) (abs((int)(a) - (int)(b)) > 24)
    if (MOVED(state.lx, last_lx) || MOVED(state.ly, last_ly) || MOVED(state.rx, last_rx) || MOVED(state.ry, last_ry)) {
        say("input sticks L=(%3u,%3u) R=(%3u,%3u) triggers L2=%3u R2=%3u", state.lx, state.ly, state.rx, state.ry, state.l2, state.r2);
        last_lx = state.lx, last_ly = state.ly, last_rx = state.rx, last_ry = state.ry;
    }
    // Touchpad: log when the number of fingers changes, and positions every ~quarter second while touching.
    if (state.touch_count != last_touches || (state.touch_count && ++touch_frames % 15 == 0))
        say("input touch count=%u t0=(%u,%u id %u) t1=(%u,%u id %u)", state.touch_count, state.touch[0].x, state.touch[0].y,
            state.touch[0].id, state.touch[1].x, state.touch[1].y, state.touch[1].id);
    last_touches = state.touch_count;
    #undef MOVED
    previous = buttons;
    return pressed;
}

// System keyboard (IME dialog), loaded at run time so a missing module can't stop the probe from starting.
// Parameter block in the PS4 layout (96 bytes); the probe logs every return code so a layout mismatch is visible.
typedef struct {
    int32_t user;
    int32_t type;                // 0 = default
    uint64_t languages;          // 0 = system language
    int32_t enter_label;         // 0 = default
    int32_t input_method;        // 0 = default
    void *filter;
    uint32_t option;
    uint32_t max_length;
    uint16_t *text;              // UTF-16 in/out buffer, max_length + 1
    float x, y;
    int32_t horizontal, vertical;
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} ImeParam;
_Static_assert(sizeof(ImeParam) == 96, "IME parameter size");
typedef struct {
    int32_t end_status;          // 0 = OK, 1 = cancelled, 2 = aborted
    int8_t reserved[12];
} ImeResult;
typedef int (*ImeInitFn)(const ImeParam *, void *);
typedef int (*ImeStatusFn)(void);
typedef int (*ImeResultFn)(ImeResult *);
typedef int (*ImeTermFn)(void);
static struct {
    bool loaded, tried, running;
    ImeInitFn init;
    ImeStatusFn status;
    ImeResultFn result;
    ImeTermFn term;
    uint16_t text[65];
    uint16_t title[32];
} ime;

static void imeLoad(void) {
    if (ime.tried) return;
    ime.tried = true;
    static const char *const paths[] = {"libSceImeDialog.sprx", "/system/common/lib/libSceImeDialog.sprx"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths) && !ime.loaded; ++i) {
        int start_result = 0;
        int handle = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &start_result);
        say("input ime load %s -> 0x%x (start 0x%x)", paths[i], handle, start_result);
        if (handle < 0) continue;
        int a = sceKernelDlsym(handle, "sceImeDialogInit", (void **)&ime.init);
        int b = sceKernelDlsym(handle, "sceImeDialogGetStatus", (void **)&ime.status);
        int c = sceKernelDlsym(handle, "sceImeDialogGetResult", (void **)&ime.result);
        int d = sceKernelDlsym(handle, "sceImeDialogTerm", (void **)&ime.term);
        say("input ime dlsym init=0x%x status=0x%x result=0x%x term=0x%x", a, b, c, d);
        ime.loaded = !a && !b && !c && !d && ime.init && ime.status && ime.result && ime.term;
    }
    if (!ime.loaded) mark("input.ime", FAIL);
}

static void imeOpen(void) {
    say("BEGIN input.ime");
    imeLoad();
    if (!ime.loaded || ime.running) return;
    static const char title[] = "PokeMMO-Prospero keyboard test";
    for (size_t i = 0; i < sizeof(title) && i < sizeof(ime.title) / 2 - 1; ++i) ime.title[i] = (uint16_t)title[i];
    memset(ime.text, 0, sizeof(ime.text));
    int user = -1;
    sceUserServiceGetInitialUser(&user);
    ImeParam param;
    memset(&param, 0, sizeof(param));
    param.user = user;
    param.max_length = 64;
    param.text = ime.text;
    param.title = ime.title;
    param.x = 960;
    param.y = 540;
    param.horizontal = 1;  // centre
    param.vertical = 1;
    int rc = -1;
    if (!GUARDED(rc = ime.init(&param, NULL))) rc = -1;
    say("input ime init rc=0x%x%s", rc, guard_signal ? " (faulted: parameter layout differs on PS5)" : "");
    if (rc == 0) ime.running = true;
    else mark("input.ime", FAIL);
}

static void imePoll(void) {
    if (!ime.running) return;
    int status = ime.status();
    if (status != 2) return;  // 1 = running, 2 = finished
    ImeResult result;
    memset(&result, 0, sizeof(result));
    int rc = ime.result(&result);
    char ascii[66] = {0};
    for (int i = 0; i < 64 && ime.text[i]; ++i) ascii[i] = ime.text[i] < 128 ? (char)ime.text[i] : '?';
    say("input ime finished result rc=0x%x end_status=%d text=\"%s\"", rc, result.end_status, ascii);
    ime.term();
    ime.running = false;
    mark("input.ime", rc == 0 ? PASS : FAIL);
}

// Draws the result tiles with scissored clears only (works whatever else is broken).
static void drawTiles(void) {
    glUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glClearColor(0.08f, 0.08f, 0.10f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    const int columns = 7, size = width / 12, gap = size / 6;
    const int left = (width - columns * size - (columns - 1) * gap) / 2;
    const int rows = (int)((CHECK_COUNT + columns - 1) / columns);
    const int top = (height + rows * size + (rows - 1) * gap) / 2;
    for (size_t i = 0; i < CHECK_COUNT; ++i) {
        int column = (int)(i % columns), row = (int)(i / columns);
        glScissor(left + column * (size + gap), top - (row + 1) * size - row * gap, size, size);
        switch (checks[i].state) {
        case PASS: glClearColor(0.15f, 0.75f, 0.30f, 1); break;
        case FAIL: glClearColor(0.85f, 0.20f, 0.20f, 1); break;
        case INFO: glClearColor(0.25f, 0.45f, 0.85f, 1); break;
        default: glClearColor(0.35f, 0.35f, 0.38f, 1); break;
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // Controller tester: one small square per button along the bottom, white while held.
    if (pad_have) {
        const int cell = width / 40, step = cell + cell / 3;
        const int start = (width - (int)PAD_BUTTON_COUNT * step) / 2, y = height / 10;
        for (size_t i = 0; i < PAD_BUTTON_COUNT; ++i) {
            bool held = pad_now.connected && (pad_now.buttons & pad_buttons[i].mask);
            glScissor(start + (int)i * step, y, cell, cell);
            if (held) glClearColor(0.95f, 0.95f, 0.95f, 1);
            else glClearColor(0.22f, 0.22f, 0.26f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

static void summarize(void) {
    unsigned passed = 0, failed = 0, info = 0;
    for (size_t i = 0; i < CHECK_COUNT; ++i) {
        passed += checks[i].state == PASS;
        failed += checks[i].state == FAIL;
        info += checks[i].state == INFO;
    }
    say("SUMMARY pass=%u fail=%u info=%u not_run=%u", passed, failed, info, (unsigned)CHECK_COUNT - passed - failed - info);
    for (size_t i = 0; i < CHECK_COUNT; ++i)
        say("SUMMARY %-15s %s", checks[i].name,
            checks[i].state == PASS ? "PASS" : checks[i].state == FAIL ? "FAIL" : checks[i].state == INFO ? "INFO" : "NOT_RUN");
}

int main(void) {
    logInit();
    say("PokeMMO-Prospero %s starting; UDP log port %d%s", PROBE_VERSION, PROBE_PORT, log_has_host ? " (+unicast host)" : "");
    guardInstall();
    probeSystem();
    bool have_gl = glOpen();
    if (have_gl) probeGl();
    probeFs28();
    probeThreads();
    probeVm();
    probeFiles();
    probeAppRoms();
    probeDataRoms();
    probeUsb();
    probeNetwork();
    probeExec();  // last: the likeliest to take the title down

    summarize();
    say("DONE with the automatic checks. Now the CONTROLLER TESTER runs: press each button, move the sticks, touch");
    say("the touchpad, then press TRIANGLE to open the system keyboard and type something. Close the title with PS.");

    if (!have_gl) {
        for (;;) sceKernelUsleep(1000000);
    }
    padOpen();
    for (unsigned frame = 1;; ++frame) {
        uint32_t pressed = padPoll();
        if (pressed & 0x00001000) imeOpen();  // TRIANGLE
        imePoll();
        drawTiles();
        eglSwapBuffers(display, surface);
        if (frame % 1800 == 0) summarize();  // every ~30 s, for late listeners and to record the input results
        sceKernelUsleep(16000);
    }
}
