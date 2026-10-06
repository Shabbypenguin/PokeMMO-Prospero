// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_process.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: process facts from the platform (pid, cores, a fixed memory size) instead of Horizon's kernel.
#include "linux_process.h"
#include <limits.h>
#include <string.h>
#include "platform.h"
#include <unistd.h>

_Static_assert(sizeof(size_t) == 8 && sizeof(LinuxTimespec) == 16, "Linux LP64 process ABI");
static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}

// What the process knows about itself. Each returns a Linux errno, zero on success.
static int nativePid(uint64_t *pid) {
    *pid = (uint64_t)getpid();
    return 0;
}
static int nativeCores(uint64_t *mask) {
    unsigned count = platformCpuCount();
    *mask = count >= 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
    return 0;
}
static int nativeAffinity(bool main_thread, uint64_t *mask) {
    (void)main_thread;
    return nativeCores(mask);
}
// The memory the guest is told about (sysconf _SC_PHYS_PAGES / _SC_AVPHYS_PAGES). The client gets explicit heap limits on its
// command line, so these only need to be plausible: what a PS5 title can reasonably use.
static int nativeMemory(uint64_t *total, uint64_t *used) {
    *total = (uint64_t)4 << 30;
    *used = (uint64_t)1 << 30;
    return 0;
}
int linuxProcessGetpid(void) {
    uint64_t pid;
    int error = nativePid(&pid);
    if (error) return fail(error);
    if (!pid || pid > INT_MAX) return fail(LINUX_EOVERFLOW);
    return (int)pid;
}
void *linuxProcessCpuAlloc(size_t count) {
    if (count > LINUX_CPU_SET_MAX_BYTES * 8u) {
        fail(LINUX_ENOMEM);
        return NULL;
    }
    // malloc semantics: the caller initializes its mask, including padding.
    return linuxAbiMalloc(((count + 63) / 64) * 8);
}
void linuxProcessCpuFree(void *set) { linuxAbiFree(set); }
int linuxProcessCpuCount(size_t bytes, const void *set) {
    if (bytes > LINUX_CPU_SET_MAX_BYTES) return fail(LINUX_EINVAL);
    size_t words = bytes / 8;
    if (words && !set) return fail(LINUX_EFAULT);
    int count = 0;
    for (size_t i = 0; i < words; ++i) {
        uint64_t word;
        memcpy(&word, (const unsigned char *)set + i * 8, 8);
        count += __builtin_popcountll(word);
    }
    // glibc ignores any trailing incomplete word.
    return count;
}
int linuxProcessGetAffinity(int pid, size_t bytes, void *set) {
    if (pid < 0) return fail(LINUX_ESRCH);
    if (!set) return fail(LINUX_EFAULT);
    if (bytes < 8 || bytes % 8 || bytes > LINUX_CPU_SET_MAX_BYTES) return fail(LINUX_EINVAL);
    if (pid) {
        int own = linuxProcessGetpid();
        if (own < 0) return -1;
        if (own != pid) return fail(LINUX_ESRCH);
    }
    uint64_t mask;
    int error = nativeAffinity(pid != 0, &mask);
    if (error) return fail(error);
    if (!mask) return fail(LINUX_EIO);
    memset(set, 0, bytes);
    memcpy(set, &mask, 8);
    return 0;
}
int64_t linuxProcessSysconf(int name) {
    if (name == 83 || name == 84) {
        uint64_t mask;
        int error = nativeCores(&mask);
        if (error) return fail(error);
        if (!mask) return fail(LINUX_EIO);
        return __builtin_popcountll(mask);
    }
    if (name == 85 || name == 86) {
        uint64_t total, used;
        int error = nativeMemory(&total, &used);
        if (error) return fail(error);
        if (used > total || !total) return fail(LINUX_EIO);
        return (int64_t)((name == 85 ? total : total - used) / 4096);
    }
    return fail(name < 0 ? LINUX_EINVAL : LINUX_ENOSYS);
}
static int monotonic(int64_t *value) {
    LinuxTimespec time;
    int error = linuxAbiReadClock(1, &time);
    if (error) return error;
    if (time.seconds < 0 || time.nanoseconds < 0 || time.nanoseconds >= 1000000000 || time.seconds > (INT64_MAX - time.nanoseconds) / 1000000000)
        return LINUX_EOVERFLOW;
    *value = time.seconds * 1000000000 + time.nanoseconds;
    return 0;
}
// The sleep is never interrupted, so `remaining` is never written.
int linuxProcessNanosleep(const LinuxTimespec *request, LinuxTimespec *remaining) {
    (void)remaining;
    if (!request) return fail(LINUX_EFAULT);
    LinuxTimespec duration = *request;  // request and remaining may alias.
    if (duration.seconds < 0 || duration.nanoseconds < 0 || duration.nanoseconds >= 1000000000) return fail(LINUX_EINVAL);
    if (duration.seconds > (INT64_MAX - duration.nanoseconds) / 1000000000) return fail(LINUX_EOVERFLOW);
    int64_t ns = duration.seconds * 1000000000 + duration.nanoseconds;
    if (!ns) return 0;
    int64_t start;
    int error = monotonic(&start);
    if (error) return fail(error);
    if (ns > INT64_MAX - start) return fail(LINUX_EOVERFLOW);
    int64_t deadline = start + ns, left = ns;
    for (;;) {
        platformSleepNs((uint64_t)left);
        int64_t now;
        int clock_error = monotonic(&now);
        if (clock_error) return fail(clock_error);
        if (now < start) return fail(LINUX_EIO);
        left = now < deadline ? deadline - now : 0;
        if (!left) return 0;
        // A sleep may end early; enforce the monotonic deadline.
    }
}
