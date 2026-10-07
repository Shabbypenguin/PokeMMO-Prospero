// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Platform layer for a Linux PC: lets the loader and its Linux ABI adapters run the real client on a development machine,
// where every failure can be inspected with ordinary tools. Not shipped.

#include "platform.h"
#include "linux_net_translate.h"
#include <arpa/inet.h>
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
#include <strings.h>
#include <sys/socket.h>
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

// The address the system would send from: a UDP socket "connected" to a public address (nothing is sent) and asked its name.
bool platformLocalIPv4(char out[16]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in remote = {0}, local = {0};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x01010101u);
    socklen_t length = sizeof(local);
    bool ok = !connect(fd, (struct sockaddr *)&remote, sizeof(remote)) && !getsockname(fd, (struct sockaddr *)&local, &length) && local.sin_addr.s_addr;
    close(fd);
    if (ok) {
        uint32_t a = ntohl(local.sin_addr.s_addr);
        snprintf(out, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    }
    return ok;
}

// Plain http:// only: enough to test the updater against a local server (tools/range_server.py). HTTP/1.0, so the body is
// whatever follows the headers until the server closes.
struct PlatformHttp {
    int socket;
    int64_t length;
    char headers[8192];
    char *body;        // bytes read past the headers, not yet returned
    size_t body_size;
};
PlatformHttp *platformHttpOpen(const char *url, bool head, int64_t range_start, int64_t range_end, int *status, char *error, size_t error_size) {
    *status = 0;
    char host[256], path[1024] = "/";
    unsigned port = 80;
    if (sscanf(url, "http://%255[^:/]:%u%1023s", host, &port, path) < 2 && sscanf(url, "http://%255[^:/]%1023s", host, path) < 1) {
        snprintf(error, error_size, "only http:// addresses on a PC");
        return NULL;
    }
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char service[16];
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &list) || !list) {
        snprintf(error, error_size, "cannot resolve %s", host);
        return NULL;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, list->ai_addr, list->ai_addrlen)) {
        snprintf(error, error_size, "connect failed errno=%d", errno);
        freeaddrinfo(list);
        if (fd >= 0) close(fd);
        return NULL;
    }
    freeaddrinfo(list);
    char request[1400], range[80] = "";
    if (range_end >= 0) snprintf(range, sizeof(range), "Range: bytes=%lld-%lld\r\n", (long long)range_start, (long long)range_end);
    int length = snprintf(request, sizeof(request), "%s %s HTTP/1.0\r\nHost: %s\r\n%sUser-Agent: PokeMMO-Prospero\r\n\r\n", head ? "HEAD" : "GET", path, host, range);
    if (write(fd, request, (size_t)length) != length) {
        snprintf(error, error_size, "send failed");
        close(fd);
        return NULL;
    }
    PlatformHttp *h = calloc(1, sizeof(*h));
    h->socket = fd;
    h->length = -1;
    size_t have = 0;
    char *end = NULL;
    while (!end && have < sizeof(h->headers) - 1) {
        ssize_t got = read(fd, h->headers + have, sizeof(h->headers) - 1 - have);
        if (got <= 0) break;
        have += (size_t)got;
        h->headers[have] = 0;
        end = strstr(h->headers, "\r\n\r\n");
    }
    if (!end || sscanf(h->headers, "HTTP/%*s %d", status) != 1) {
        snprintf(error, error_size, "bad response");
        platformHttpClose(h);
        return NULL;
    }
    size_t header_size = (size_t)(end + 4 - h->headers);
    h->body_size = have - header_size;
    h->body = malloc(h->body_size + 1);
    memcpy(h->body, h->headers + header_size, h->body_size);
    *end = 0;
    char value[64];
    if (platformHttpHeader(h, "Content-Length", value, sizeof(value))) h->length = atoll(value);
    return h;
}
int64_t platformHttpLength(PlatformHttp *h) { return h->length; }
bool platformHttpHeader(PlatformHttp *h, const char *name, char *value, size_t size) {
    size_t name_length = strlen(name);
    for (const char *line = strstr(h->headers, "\r\n"); line && *line;) {
        line += strspn(line, "\r\n");
        size_t length = strcspn(line, "\r\n");
        if (length > name_length && line[name_length] == ':' && !strncasecmp(line, name, name_length)) {
            const char *start = line + name_length + 1;
            while (*start == ' ') ++start;
            snprintf(value, size, "%.*s", (int)(line + length - start), start);
            return true;
        }
        line += length;
    }
    return false;
}
int64_t platformHttpRead(PlatformHttp *h, void *buffer, size_t size) {
    if (h->body_size) {
        size_t n = h->body_size < size ? h->body_size : size;
        memcpy(buffer, h->body, n);
        memmove(h->body, h->body + n, h->body_size - n);
        h->body_size -= n;
        return (int64_t)n;
    }
    ssize_t got = read(h->socket, buffer, size);
    return got < 0 ? -errno : got;
}
void platformHttpClose(PlatformHttp *h) {
    if (!h) return;
    close(h->socket);
    free(h->body);
    free(h);
}

void platformQuit(void) { exit(0); }
const char *platformLogPath(void) {
    const char *path = getenv("PROSPERO_LOG");
    return path ? path : "";
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
