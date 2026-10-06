// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// Mutexes and condition variables of the guest on the platform's POSIX ones. Interface after PokeMMO-NX (MIT).
#include "linux_sync.h"
#include "diagnostics.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(LinuxMutex) == 40 && sizeof(LinuxCondition) == 48 && sizeof(LinuxMutexAttr) == 4 && sizeof(LinuxConditionAttr) == 4,
               "glibc x86-64 sync sizes");

static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
void linuxSyncLock(void) { pthread_mutex_lock(&registry); }
void linuxSyncUnlock(void) { pthread_mutex_unlock(&registry); }

int linuxSyncErrno(int native) {
    switch (native) {
        case 0: return 0;
        case EPERM: return LINUX_EPERM;
        case EINVAL: return LINUX_EINVAL;
        case EBUSY: return LINUX_EBUSY;
        case EAGAIN: return LINUX_EAGAIN;
        case EDEADLK: return LINUX_EDEADLK;
        case ETIMEDOUT: return LINUX_ETIMEDOUT;
        case ENOMEM: return LINUX_ENOMEM;
        case EINTR: return LINUX_EINTR;
        default: return LINUX_EIO;
    }
}

// ---- mutexes -------------------------------------------------------------------------------------------------------------------
// Slot layout inside the 40 guest bytes: offset 16 is glibc's kind (read for statically initialized recursive mutexes),
// offset 24 the native object.
enum { MUTEX_KIND = 16, MUTEX_NATIVE = 24 };
static _Atomic(pthread_mutex_t *) *mutexSlot(LinuxMutex *mutex) { return (_Atomic(pthread_mutex_t *) *)((unsigned char *)mutex + MUTEX_NATIVE); }
static pthread_mutex_t *createMutex(int kind) {
    pthread_mutex_t *native = malloc(sizeof(*native));
    if (!native) return NULL;
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, kind == 1 ? PTHREAD_MUTEX_RECURSIVE : kind == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    int result = pthread_mutex_init(native, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (result) {
        free(native);
        return NULL;
    }
    return native;
}
static pthread_mutex_t *nativeMutex(LinuxMutex *mutex) {
    _Atomic(pthread_mutex_t *) *slot = mutexSlot(mutex);
    pthread_mutex_t *native = atomic_load_explicit(slot, memory_order_acquire);
    if (native) return native;
    int kind;
    memcpy(&kind, (unsigned char *)mutex + MUTEX_KIND, sizeof(kind));
    pthread_mutex_t *created = createMutex(kind & 3);
    if (!created) return NULL;
    pthread_mutex_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(slot, &expected, created, memory_order_acq_rel, memory_order_acquire)) return created;
    pthread_mutex_destroy(created);  // another thread created it first
    free(created);
    return expected;
}
int linuxPthreadMutexInit(LinuxMutex *mutex, const LinuxMutexAttr *attributes) {
    if (!mutex) return LINUX_EINVAL;
    int kind = attributes ? (int)(attributes->value & 3) : 0;
    memset(mutex, 0, sizeof(*mutex));
    memcpy((unsigned char *)mutex + MUTEX_KIND, &kind, sizeof(kind));
    pthread_mutex_t *native = createMutex(kind);
    if (!native) return LINUX_ENOMEM;
    atomic_store_explicit(mutexSlot(mutex), native, memory_order_release);
    return 0;
}
int linuxPthreadMutexDestroy(LinuxMutex *mutex) {
    if (!mutex) return LINUX_EINVAL;
    pthread_mutex_t *native = atomic_exchange_explicit(mutexSlot(mutex), NULL, memory_order_acq_rel);
    if (native) {
        int result = pthread_mutex_destroy(native);
        if (result) {
            atomic_store_explicit(mutexSlot(mutex), native, memory_order_release);
            return linuxSyncErrno(result);
        }
        free(native);
    }
    return 0;
}
int linuxPthreadMutexLock(LinuxMutex *mutex) {
    pthread_mutex_t *native = mutex ? nativeMutex(mutex) : NULL;
    return native ? linuxSyncErrno(pthread_mutex_lock(native)) : LINUX_EINVAL;
}
int linuxPthreadMutexTryLock(LinuxMutex *mutex) {
    pthread_mutex_t *native = mutex ? nativeMutex(mutex) : NULL;
    return native ? linuxSyncErrno(pthread_mutex_trylock(native)) : LINUX_EINVAL;
}
int linuxPthreadMutexUnlock(LinuxMutex *mutex) {
    pthread_mutex_t *native = mutex ? nativeMutex(mutex) : NULL;
    return native ? linuxSyncErrno(pthread_mutex_unlock(native)) : LINUX_EINVAL;
}

// ---- condition variables ---------------------------------------------------------------------------------------------------------
// Offset 0: the native object; offset 8: the Linux clock id (0 realtime, 1 monotonic).
enum { LINUX_CLOCK_REALTIME = 0, LINUX_CLOCK_MONOTONIC = 1 };
static _Atomic(pthread_cond_t *) *condSlot(LinuxCondition *condition) { return (_Atomic(pthread_cond_t *) *)condition; }
static int condClock(const LinuxCondition *condition) {
    int clock;
    memcpy(&clock, (const unsigned char *)condition + 8, sizeof(clock));
    return clock;
}
static pthread_cond_t *createCond(int clock) {
    pthread_cond_t *native = malloc(sizeof(*native));
    if (!native) return NULL;
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    if (clock == LINUX_CLOCK_MONOTONIC) pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    int result = pthread_cond_init(native, &attributes);
    pthread_condattr_destroy(&attributes);
    if (result) {
        free(native);
        return NULL;
    }
    return native;
}
static pthread_cond_t *nativeCond(LinuxCondition *condition) {
    _Atomic(pthread_cond_t *) *slot = condSlot(condition);
    pthread_cond_t *native = atomic_load_explicit(slot, memory_order_acquire);
    if (native) return native;
    pthread_cond_t *created = createCond(condClock(condition));
    if (!created) return NULL;
    pthread_cond_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(slot, &expected, created, memory_order_acq_rel, memory_order_acquire)) return created;
    pthread_cond_destroy(created);
    free(created);
    return expected;
}
int linuxPthreadCondAttrInit(LinuxConditionAttr *attributes) {
    if (!attributes) return LINUX_EINVAL;
    attributes->value = 0;
    return 0;
}
int linuxPthreadCondAttrDestroy(LinuxConditionAttr *attributes) { return attributes ? 0 : LINUX_EINVAL; }
int linuxPthreadCondAttrSetClock(LinuxConditionAttr *attributes, int clock) {
    if (!attributes || (clock != LINUX_CLOCK_REALTIME && clock != LINUX_CLOCK_MONOTONIC)) return LINUX_EINVAL;
    attributes->value = (attributes->value & 1u) | ((uint32_t)clock << 1);
    return 0;
}
int linuxPthreadCondInit(LinuxCondition *condition, const LinuxConditionAttr *attributes) {
    if (!condition) return LINUX_EINVAL;
    int clock = attributes ? (int)(attributes->value >> 1) : LINUX_CLOCK_REALTIME;
    memset(condition, 0, sizeof(*condition));
    memcpy((unsigned char *)condition + 8, &clock, sizeof(clock));
    pthread_cond_t *native = createCond(clock);
    if (!native) return LINUX_ENOMEM;
    atomic_store_explicit(condSlot(condition), native, memory_order_release);
    return 0;
}
int linuxPthreadCondDestroy(LinuxCondition *condition) {
    if (!condition) return LINUX_EINVAL;
    pthread_cond_t *native = atomic_exchange_explicit(condSlot(condition), NULL, memory_order_acq_rel);
    if (native) {
        pthread_cond_destroy(native);
        free(native);
    }
    return 0;
}
int linuxPthreadCondWait(LinuxCondition *condition, LinuxMutex *mutex) {
    pthread_cond_t *native = condition ? nativeCond(condition) : NULL;
    pthread_mutex_t *lock = mutex ? nativeMutex(mutex) : NULL;
    if (!native || !lock) return LINUX_EINVAL;
    return linuxSyncErrno(pthread_cond_wait(native, lock));
}
int linuxPthreadCondTimedWait(LinuxCondition *condition, LinuxMutex *mutex, const LinuxTimespec *deadline) {
    pthread_cond_t *native = condition ? nativeCond(condition) : NULL;
    pthread_mutex_t *lock = mutex ? nativeMutex(mutex) : NULL;
    if (!native || !lock || !deadline) return LINUX_EINVAL;
    if (deadline->nanoseconds < 0 || deadline->nanoseconds >= 1000000000) return LINUX_EINVAL;
    // The deadline is on the condition's clock; the native condition uses the matching native clock.
    struct timespec until = {(time_t)deadline->seconds, (long)deadline->nanoseconds};
    return linuxSyncErrno(pthread_cond_timedwait(native, lock, &until));
}
int linuxPthreadCondSignal(LinuxCondition *condition) {
    pthread_cond_t *native = condition ? nativeCond(condition) : NULL;
    return native ? linuxSyncErrno(pthread_cond_signal(native)) : LINUX_EINVAL;
}
int linuxPthreadCondBroadcast(LinuxCondition *condition) {
    pthread_cond_t *native = condition ? nativeCond(condition) : NULL;
    return native ? linuxSyncErrno(pthread_cond_broadcast(native)) : LINUX_EINVAL;
}
bool linuxSyncReset(void) { return true; }
