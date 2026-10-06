// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Platform layer for a Linux PC: lets the loader and its Linux ABI adapters run the real client on a development machine,
// where every failure can be inspected with ordinary tools. Not shipped.

#include "platform.h"
#include "linux_net_translate.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

const char *platformName(void) { return "host"; }

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
void platformLogLine(const char *line) {
    pthread_mutex_lock(&log_lock);
    if (!log_file) {
        const char *path = getenv("PROSPERO_LOG");
        log_file = path ? fopen(path, "w") : NULL;
        if (!log_file) log_file = stderr;
    }
    fprintf(log_file, "%s\n", line);
    fflush(log_file);
    pthread_mutex_unlock(&log_lock);
}

uint64_t platformMonotonicNs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}
void platformSleepNs(uint64_t nanoseconds) {
    struct timespec wait = {(time_t)(nanoseconds / 1000000000u), (long)(nanoseconds % 1000000000u)};
    while (nanosleep(&wait, &wait) && errno == EINTR) {}
}
void platformYield(void) { sched_yield(); }
void platformRandom(void *buffer, size_t bytes) {
    unsigned char *cursor = buffer;
    while (bytes) {
        ssize_t got = getrandom(cursor, bytes, 0);
        if (got <= 0) continue;
        cursor += got;
        bytes -= (size_t)got;
    }
}

size_t platformPageSize(void) { return 4096; }
static int nativeProtection(int protection) {
    return (protection & PLATFORM_PROT_READ ? PROT_READ : 0) | (protection & PLATFORM_PROT_WRITE ? PROT_WRITE : 0) |
           (protection & PLATFORM_PROT_EXEC ? PROT_EXEC : 0);
}
int platformReserve(size_t bytes, void **address) {
    // Over-reserve by one block so the result can be aligned to PLATFORM_VM_BLOCK.
    size_t total = bytes + PLATFORM_VM_BLOCK;
    unsigned char *raw = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return ENOMEM;
    uintptr_t aligned = ((uintptr_t)raw + PLATFORM_VM_BLOCK - 1) & ~(uintptr_t)(PLATFORM_VM_BLOCK - 1);
    if (aligned > (uintptr_t)raw) munmap(raw, aligned - (uintptr_t)raw);
    uintptr_t end = (uintptr_t)raw + total;
    if (end > aligned + bytes) munmap((void *)(aligned + bytes), end - (aligned + bytes));
    *address = (void *)aligned;
    return 0;
}
int platformUnreserve(void *address, size_t bytes) { return munmap(address, bytes) ? errno : 0; }
int platformCommit(void *address, size_t bytes, int kind) {
    (void)kind;
    void *result = mmap(address, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return result == MAP_FAILED ? ENOMEM : 0;
}
int platformDecommit(void *address, size_t bytes) {
    void *result = mmap(address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return result == MAP_FAILED ? ENOMEM : 0;
}
int platformProtect(void *address, size_t bytes, int protection) {
    return mprotect(address, bytes, nativeProtection(protection)) ? errno : 0;
}
void *platformAllocatePages(size_t bytes) {
    void *result = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return result == MAP_FAILED ? NULL : result;
}
void platformFreePages(void *address, size_t bytes) { munmap(address, bytes); }

bool platformThreadStack(void **low, size_t *bytes) {
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes)) return false;
    bool ok = pthread_attr_getstack(&attributes, low, bytes) == 0;
    pthread_attr_destroy(&attributes);
    return ok;
}
unsigned platformCpuCount(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (unsigned)count : 1;
}

// The host can use readdir; the PS5 cannot (see platform_ps5.c).
struct PlatformDirectory {
    DIR *native;
};
PlatformDirectory *platformDirectoryOpen(const char *path, int *error) {
    DIR *native = opendir(path);
    if (!native) {
        *error = errno;
        return NULL;
    }
    PlatformDirectory *directory = malloc(sizeof(*directory));
    if (!directory) {
        closedir(native);
        *error = ENOMEM;
        return NULL;
    }
    directory->native = native;
    return directory;
}
int platformDirectoryRead(PlatformDirectory *directory, char name[256], uint8_t *type, uint64_t *inode, int *error) {
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory->native);
        if (!entry) {
            *error = errno;
            return errno ? -1 : 0;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(name, 256, "%s", entry->d_name);
        *type = entry->d_type;
        *inode = entry->d_ino;
        return 1;
    }
}
void platformDirectoryClose(PlatformDirectory *directory) {
    if (!directory) return;
    closedir(directory->native);
    free(directory);
}

int platformResolveIPv4(const char *name, uint32_t *address) {
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    int code = getaddrinfo(name, NULL, &hints, &list);
    if (code) return code == EAI_NONAME ? LINUX_EAI_NONAME : code == EAI_AGAIN ? LINUX_EAI_AGAIN : LINUX_EAI_FAIL;
    int result = LINUX_EAI_NONAME;
    for (struct addrinfo *item = list; item; item = item->ai_next)
        if (item->ai_family == AF_INET && item->ai_addr) {
            *address = ((struct sockaddr_in *)item->ai_addr)->sin_addr.s_addr;
            result = 0;
            break;
        }
    freeaddrinfo(list);
    return result;
}

int platformOpenUrl(const char *url) {
    (void)url;
    return -1;
}

void platformFatal(const char *message) {
    char line[512];
    snprintf(line, sizeof(line), "FATAL %s", message);
    platformLogLine(line);
    abort();
}

bool platformPadRead(PlatformPad *pad) {
    *pad = (PlatformPad){0};
    return false;
}

int platformAudioOpen(unsigned frames) {
    (void)frames;
    return -1;
}
int platformAudioWrite(int handle, const int16_t *interleaved) {
    (void)handle;
    (void)interleaved;
    return -1;
}
void platformAudioClose(int handle) { (void)handle; }
