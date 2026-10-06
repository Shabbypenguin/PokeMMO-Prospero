// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Platform layer of the PS5 title. What the hardware probe established on firmware 12.40 decides each choice here:
//   memory     anonymous mmap is limited to the ~400 MiB flexible budget and cannot reserve address space; the console's own
//              reservation call (sceKernelReserveVirtualRange) can, and direct memory (GiBs) maps at fixed addresses inside it
//   code       only flexible memory is used for anything that becomes executable (RW, then mprotect to RX)
//   listing    opendir is refused in titles (EPERM); open + getdents is tried, with an index file written by the installer as fallback
//   DNS        getaddrinfo crashes a title (it lives in a WebKit-only module); sceNetResolver works
//   log        UDP broadcast on port 18194 (+ the host in /app0/assets/loghost.txt) and /app0/prospero.log
#include "platform.h"
#include "linux_net_translate.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---- system functions without public headers --------------------------------------------------------------------------------------
int sceKernelUsleep(unsigned int microseconds);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment, int memory_type,
                                      int64_t *physical_start);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t physical_start, size_t alignment);
int32_t sceKernelReleaseDirectMemory(int64_t physical_start, size_t length);
int sceKernelReserveVirtualRange(void **address, size_t length, int flags, size_t alignment);
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address, int timeout, int retries, int flags);
int sceNetResolverDestroy(int resolver);
#define SCE_KERNEL_MAP_FIXED 0x10
#define SCE_KERNEL_WB_ONION 12  // CPU-cached; the type ps5-opengl's app heap uses
#define LOG_PORT 18194

const char *platformName(void) { return "ps5"; }

// ---- logging --------------------------------------------------------------------------------------------------------------------
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_socket = -1;
static struct sockaddr_in log_broadcast, log_host;
static bool log_has_host, log_ready;
static int log_file = -1;
static void logOpen(void) {
    log_ready = true;
    log_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (log_socket >= 0) {
        int on = 1;
        setsockopt(log_socket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        memset(&log_broadcast, 0, sizeof(log_broadcast));
        log_broadcast.sin_family = AF_INET;
        log_broadcast.sin_port = htons(LOG_PORT);
        log_broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }
    int host = open("/app0/assets/loghost.txt", O_RDONLY);
    if (host >= 0) {
        char text[64] = {0};
        ssize_t got = read(host, text, sizeof(text) - 1);
        close(host);
        if (got > 0) {
            text[strcspn(text, " \r\n")] = 0;
            memset(&log_host, 0, sizeof(log_host));
            log_host.sin_family = AF_INET;
            log_host.sin_port = htons(LOG_PORT);
            log_has_host = inet_pton(AF_INET, text, &log_host.sin_addr) == 1;
        }
    }
    log_file = open("/app0/prospero.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
void platformLogLine(const char *line) {
    char text[1100];
    int length = snprintf(text, sizeof(text), "%s\n", line);
    if (length < 0) return;
    if ((size_t)length >= sizeof(text)) length = sizeof(text) - 1;
    pthread_mutex_lock(&log_lock);
    if (!log_ready) logOpen();
    if (log_socket >= 0) {
        sendto(log_socket, text, (size_t)length, 0, (struct sockaddr *)&log_broadcast, sizeof(log_broadcast));
        if (log_has_host) sendto(log_socket, text, (size_t)length, 0, (struct sockaddr *)&log_host, sizeof(log_host));
    }
    if (log_file >= 0) write(log_file, text, (size_t)length);
    pthread_mutex_unlock(&log_lock);
}
bool platformLogHasHost(void) { return log_has_host; }

// ---- time and randomness ------------------------------------------------------------------------------------------------------------
uint64_t platformMonotonicNs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}
void platformSleepNs(uint64_t nanoseconds) {
    uint64_t microseconds = nanoseconds / 1000u;
    while (microseconds) {
        unsigned step = microseconds > 1000000u ? 1000000u : (unsigned)microseconds;
        sceKernelUsleep(step);
        microseconds -= step;
    }
    if (!nanoseconds) sched_yield();
}
void platformYield(void) { sched_yield(); }
// No kernel randomness function is exported to titles outside the WebKit module (which the loader must not pull in), so the
// generator is seeded from the cycle counter and clocks: good enough for stack guards and hash seeds, not for keys.
static uint64_t splitmix(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
void platformRandom(void *buffer, size_t bytes) {
    static _Atomic uint64_t counter;
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    uint64_t state = ((uint64_t)high << 32 | low) ^ platformMonotonicNs() ^ (uint64_t)(uintptr_t)buffer ^ (atomic_fetch_add(&counter, 1) << 48);
    unsigned char *cursor = buffer;
    while (bytes) {
        uint64_t value = splitmix(&state);
        size_t take = bytes < sizeof(value) ? bytes : sizeof(value);
        memcpy(cursor, &value, take);
        cursor += take;
        bytes -= take;
    }
}

// ---- memory ---------------------------------------------------------------------------------------------------------------------------
size_t platformPageSize(void) { return 16384; }
static int nativeProtection(int protection) {
    return (protection & PLATFORM_PROT_READ ? PROT_READ : 0) | (protection & PLATFORM_PROT_WRITE ? PROT_WRITE : 0) |
           (protection & PLATFORM_PROT_EXEC ? PROT_EXEC : 0);
}
// Every committed block is remembered: its physical memory (direct) or that it is flexible memory. A decommitted block keeps its
// memory (made inaccessible) and is reused by the next commit of the same address; everything is released with its reservation.
// Direct memory is never handed back piecemeal because releasing a mapped range would also drop the address reservation there.
typedef struct {
    uintptr_t address;  // 0: empty slot
    int64_t physical;   // -1: flexible memory
} Block;
static Block *blocks;
static size_t block_capacity, block_count;
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t blockSlot(uintptr_t address) { return (size_t)((address / PLATFORM_VM_BLOCK) * 0x9e3779b97f4a7c15ull >> 20) & (block_capacity - 1); }
static Block *blockFind(uintptr_t address) {
    if (!block_capacity) return NULL;
    for (size_t i = blockSlot(address);; i = (i + 1) & (block_capacity - 1)) {
        if (blocks[i].address == address) return &blocks[i];
        if (!blocks[i].address) return NULL;
    }
}
static bool blockInsert(uintptr_t address, int64_t physical);
static bool blockGrow(void) {
    size_t old_capacity = block_capacity;
    Block *old = blocks;
    size_t capacity = old_capacity ? old_capacity * 2 : 65536;
    Block *fresh = calloc(capacity, sizeof(*fresh));
    if (!fresh) return false;
    blocks = fresh;
    block_capacity = capacity;
    block_count = 0;
    for (size_t i = 0; i < old_capacity; ++i)
        if (old[i].address) blockInsert(old[i].address, old[i].physical);
    free(old);
    return true;
}
static bool blockInsert(uintptr_t address, int64_t physical) {
    if ((block_count + 1) * 10 > block_capacity * 7 && !blockGrow()) return false;
    for (size_t i = blockSlot(address);; i = (i + 1) & (block_capacity - 1))
        if (!blocks[i].address) {
            blocks[i] = (Block){address, physical};
            ++block_count;
            return true;
        }
}
// Removal with backward-shift (linear probing stays correct without tombstones).
static void blockRemove(Block *slot) {
    size_t i = (size_t)(slot - blocks);
    blocks[i].address = 0;
    --block_count;
    for (size_t j = (i + 1) & (block_capacity - 1); blocks[j].address; j = (j + 1) & (block_capacity - 1)) {
        Block moved = blocks[j];
        blocks[j].address = 0;
        --block_count;
        blockInsert(moved.address, moved.physical);
    }
}

int platformReserve(size_t bytes, void **address) {
    void *base = NULL;
    int rc = sceKernelReserveVirtualRange(&base, bytes, 0, PLATFORM_VM_BLOCK);
    if (rc || !base) return ENOMEM;
    *address = base;
    return 0;
}
int platformUnreserve(void *address, size_t bytes) {
    pthread_mutex_lock(&block_lock);
    for (uintptr_t at = (uintptr_t)address; at < (uintptr_t)address + bytes; at += PLATFORM_VM_BLOCK) {
        Block *block = blockFind(at);
        if (!block) continue;
        if (block->physical >= 0) sceKernelReleaseDirectMemory(block->physical, PLATFORM_VM_BLOCK);
        blockRemove(block);
    }
    pthread_mutex_unlock(&block_lock);
    return munmap(address, bytes) ? errno : 0;
}
static int commitRun(uintptr_t address, size_t bytes, int kind) {
    if (kind == PLATFORM_MEMORY_CODE) {
        void *mapped = mmap((void *)address, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (mapped == MAP_FAILED) return errno ? errno : ENOMEM;
        for (size_t offset = 0; offset < bytes; offset += PLATFORM_VM_BLOCK)
            if (!blockInsert(address + offset, -1)) return ENOMEM;
        return 0;
    }
    int64_t physical = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, PLATFORM_VM_BLOCK, SCE_KERNEL_WB_ONION, &physical);
    if (rc) return ENOMEM;
    void *where = (void *)address;
    rc = sceKernelMapDirectMemory(&where, bytes, PROT_READ | PROT_WRITE, SCE_KERNEL_MAP_FIXED, physical, PLATFORM_VM_BLOCK);
    if (rc || where != (void *)address) {
        sceKernelReleaseDirectMemory(physical, bytes);
        return ENOMEM;
    }
    for (size_t offset = 0; offset < bytes; offset += PLATFORM_VM_BLOCK)
        if (!blockInsert(address + offset, physical + (int64_t)offset)) return ENOMEM;
    return 0;
}
int platformCommit(void *address, size_t bytes, int kind) {
    int error = 0;
    pthread_mutex_lock(&block_lock);
    uintptr_t at = (uintptr_t)address, end = at + bytes;
    while (at < end && !error) {
        Block *block = blockFind(at);
        if (block) {  // kept from an earlier commit: make it accessible again
            if (mprotect((void *)at, PLATFORM_VM_BLOCK, PROT_READ | PROT_WRITE)) error = errno;
            at += PLATFORM_VM_BLOCK;
            continue;
        }
        uintptr_t run = at;
        while (run < end && !blockFind(run)) run += PLATFORM_VM_BLOCK;
        error = commitRun(at, run - at, kind);
        at = run;
    }
    pthread_mutex_unlock(&block_lock);
    return error;
}
int platformDecommit(void *address, size_t bytes) { return mprotect(address, bytes, PROT_NONE) ? errno : 0; }
int platformProtect(void *address, size_t bytes, int protection) { return mprotect(address, bytes, nativeProtection(protection)) ? errno : 0; }
void *platformAllocatePages(size_t bytes) {
    void *result = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return result == MAP_FAILED ? NULL : result;
}
void platformFreePages(void *address, size_t bytes) { munmap(address, bytes); }

// ---- threads --------------------------------------------------------------------------------------------------------------------------
bool platformThreadStack(void **low, size_t *bytes) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    bool ok = pthread_attr_get_np(pthread_self(), &attributes) == 0 && pthread_attr_getstack(&attributes, low, bytes) == 0;
    pthread_attr_destroy(&attributes);
    return ok;
}
unsigned platformCpuCount(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (unsigned)count : 8;
}

// ---- directory listing ------------------------------------------------------------------------------------------------------------
// The kernel's records (FreeBSD 11 layout): d_fileno (4), d_reclen (2), d_type (1), d_namlen (1), d_name.
struct PlatformDirectory {
    int fd;          // -1: listing comes from an index file
    char *path;
    char buffer[65536];  // loader-1: an 8 KiB buffer gave EINVAL on /app0 (PFS); 64 KiB is the largest block size there
    int filled, offset;
    char *index;     // index file contents ("name" or "name/" per line)
    size_t index_offset;
};
static char *readIndex(const char *path) {
    char index_path[1024];
    snprintf(index_path, sizeof(index_path), "%s/.prospero-index", path);
    int fd = open(index_path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat info;
    char *text = NULL;
    if (!fstat(fd, &info) && info.st_size >= 0 && info.st_size < (8 << 20) && (text = malloc((size_t)info.st_size + 1))) {
        ssize_t got = read(fd, text, (size_t)info.st_size);
        text[got > 0 ? got : 0] = 0;
    }
    close(fd);
    return text;
}
PlatformDirectory *platformDirectoryOpen(const char *path, int *error) {
    PlatformDirectory *directory = calloc(1, sizeof(*directory));
    if (!directory) {
        *error = ENOMEM;
        return NULL;
    }
    directory->path = strdup(path);
    directory->fd = open(path, O_RDONLY | O_DIRECTORY);
    if (directory->fd >= 0) return directory;
    int open_error = errno;
    directory->index = readIndex(path);
    if (directory->index) return directory;
    free(directory->path);
    free(directory);
    *error = open_error;
    return NULL;
}
int platformDirectoryRead(PlatformDirectory *directory, char name[256], uint8_t *type, uint64_t *inode, int *error) {
    if (directory->fd < 0) {
        while (directory->index[directory->index_offset]) {
            char *line = directory->index + directory->index_offset;
            size_t length = strcspn(line, "\r\n");
            directory->index_offset += length + strspn(line + length, "\r\n");
            if (!length || length > 255) continue;
            bool folder = line[length - 1] == '/';
            size_t name_length = folder ? length - 1 : length;
            memcpy(name, line, name_length);
            name[name_length] = 0;
            *type = folder ? 4 : 8;
            *inode = 0;
            return 1;
        }
        return 0;
    }
    for (;;) {
        if (directory->offset >= directory->filled) {
            int got = getdents(directory->fd, directory->buffer, (int)sizeof(directory->buffer));
            if (got < 0) {
                long base = 0;
                got = getdirentries(directory->fd, directory->buffer, (int)sizeof(directory->buffer), &base);
            }
            if (got < 0) {
                *error = errno;
                // Neither works on this file system: switch to the installer's index file, if there is one.
                if (!directory->offset && !directory->filled && (directory->index = readIndex(directory->path))) {
                    close(directory->fd);
                    directory->fd = -1;
                    return platformDirectoryRead(directory, name, type, inode, error);
                }
                return -1;
            }
            if (got == 0) return 0;
            directory->filled = got;
            directory->offset = 0;
        }
        const unsigned char *record = (const unsigned char *)directory->buffer + directory->offset;
        uint16_t length;
        memcpy(&length, record + 4, sizeof(length));
        if (length < 8 || directory->offset + length > directory->filled) {
            *error = EIO;
            return -1;
        }
        directory->offset += length;
        uint32_t fileno;
        memcpy(&fileno, record, sizeof(fileno));
        unsigned name_length = record[7];
        if (!fileno || name_length > 255 || 8u + name_length > length) continue;
        memcpy(name, record + 8, name_length);
        name[name_length] = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        *type = record[6];  // DT_* values are the same as Linux's
        *inode = fileno;
        return 1;
    }
}
void platformDirectoryClose(PlatformDirectory *directory) {
    if (!directory) return;
    if (directory->fd >= 0) close(directory->fd);
    free(directory->index);
    free(directory->path);
    free(directory);
}

// ---- network --------------------------------------------------------------------------------------------------------------------------
static pthread_once_t net_once = PTHREAD_ONCE_INIT;
static int net_pool = -1;
static void netInit(void) {
    sceNetInit();
    net_pool = sceNetPoolCreate("prospero-dns", 64 * 1024, 0);
}
int platformResolveIPv4(const char *name, uint32_t *address) {
    pthread_once(&net_once, netInit);
    if (net_pool < 0) return LINUX_EAI_FAIL;
    int resolver = sceNetResolverCreate("prospero-dns", net_pool, 0);
    if (resolver < 0) return LINUX_EAI_AGAIN;
    struct in_addr result = {0};
    int rc = sceNetResolverStartNtoa(resolver, name, &result, 0, 0, 0);
    sceNetResolverDestroy(resolver);
    if (rc || !result.s_addr) return LINUX_EAI_NONAME;
    *address = result.s_addr;
    return 0;
}

// ---- end ----------------------------------------------------------------------------------------------------------------------
void platformFatal(const char *message) {
    char line[512];
    snprintf(line, sizeof(line), "FATAL %s", message);
    platformLogLine(line);
    for (;;) sceKernelUsleep(1000000);
}
