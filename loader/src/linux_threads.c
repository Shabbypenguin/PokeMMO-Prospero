// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_threads.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: native threads are POSIX threads (no Horizon cores or priorities); x86-64 glibc attr size (56 bytes); thread-local
// storage of loaded libraries is per thread and lazy (linux_tls.c), so threads no longer attach to it.
#include "linux_threads.h"
#include "linux_tls.h"
#include "linux_abi.h"
#include "linux_runtime.h"
#include "diagnostics.h"
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include <pthread.h>

// Native threads are the platform's POSIX threads (PS5: libkernel's; PC: glibc's).
typedef struct {
    pthread_t thread;
    bool created;
    void (*entry)(void *);
    void *argument;
    void *stack_low;  // recorded by the thread itself when it starts
    size_t stack_bytes, guard;
} NativeThread;
static pthread_mutex_t records_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t nativeHandle(void *token) { return token ? (uint32_t)(uintptr_t)token : 0; }
static void lockRecords(void) { pthread_mutex_lock(&records_lock); }
static void unlockRecords(void) { pthread_mutex_unlock(&records_lock); }
int linuxThreadBackgroundCore(void) { return 0; }
static void *nativeMain(void *argument) {
    NativeThread *native = argument;
    void *low = NULL;
    size_t bytes = 0;
    if (platformThreadStack(&low, &bytes)) {
        native->stack_low = low;
        native->stack_bytes = bytes;
    }
    native->entry(native->argument);
    return NULL;
}
static int nativeCreate(void **token, void (*entry)(void *), void *argument, size_t stack, size_t guard) {
    NativeThread *native = calloc(1, sizeof(*native));
    if (!native) return LINUX_EAGAIN;
    *token = native;
    native->entry = entry;
    native->argument = argument;
    native->guard = guard;
    native->stack_bytes = stack;
    return 0;
}
static int nativeStart(void *token) {
    NativeThread *native = token;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    size_t page = platformPageSize();
    pthread_attr_setstacksize(&attributes, (native->stack_bytes + page - 1) / page * page);
    int result = pthread_create(&native->thread, &attributes, nativeMain, native);
    pthread_attr_destroy(&attributes);
    if (result) {
        diagnosticsTrace("pthread.native.create=FAIL native_error=%d stack=%zu", result, native->stack_bytes);
        return LINUX_EAGAIN;
    }
    native->created = true;
    return 0;
}
static int nativeJoin(void *token) {
    int result = pthread_join(((NativeThread *)token)->thread, NULL);
    if (result) diagnosticsTrace("pthread.native.join=FAIL native_error=%d", result);
    return result ? LINUX_EIO : 0;
}
static int nativeClose(void *token) {
    free(token);
    return 0;
}
static int nativeStack(void *token, void **base, size_t *bytes, size_t *guard) {
    NativeThread *native = token;
    if (!native || !native->created || !native->stack_low) return LINUX_EIO;
    *base = native->stack_low;
    *bytes = native->stack_bytes;
    *guard = 0;
    return 0;
}
static int nativeCurrentStack(void **base, size_t *bytes, size_t *guard) {
    if (!platformThreadStack(base, bytes)) return LINUX_EIO;
    *guard = 0;
    return 0;
}
void linuxThreadYield(void) { platformYield(); }
_Static_assert(sizeof(LinuxPthreadAttr) == 56 && sizeof(LinuxPthread) == 8 && sizeof(LinuxPthreadKey) == 4, "x86-64 glibc pthread layouts");
// glibc x86-64 attr offsets: flags 8, guard 16, stack-top 24, size 32,
// extension 40. Never reinterpret an attr as a libnx/newlib object.
enum { ATTR_FLAGS = 8, ATTR_GUARD = 16, ATTR_TOP = 24, ATTR_SIZE = 32 };
typedef struct {
    LinuxPthread id;
    void *native;
    LinuxThreadEntry entry;
    void *argument, *result;
    bool started, exited, claimed, detached;
} Record;
typedef struct {
    uint64_t generation;
    bool used;
    void (*destructor)(void *);
} Key;
typedef struct {
    uint64_t generation;
    void *value;
} Value;
static Record records[LINUX_THREAD_MAX_THREADS];
static Key keys[LINUX_THREAD_MAX_KEYS];
static _Thread_local Value values[LINUX_THREAD_MAX_KEYS];
static _Thread_local LinuxPthread self_id;
static LinuxPthread next_id = 1;
static bool shutting_down;
static size_t created;
static uint64_t read64(const LinuxPthreadAttr *attr, unsigned offset) {
    uint64_t value;
    memcpy(&value, (const unsigned char *)attr + offset, sizeof(value));
    return value;
}
static void write64(LinuxPthreadAttr *attr, unsigned offset, uint64_t value) { memcpy((unsigned char *)attr + offset, &value, sizeof(value)); }
static uint32_t flags(const LinuxPthreadAttr *attr) {
    uint32_t result;
    memcpy(&result, (const unsigned char *)attr + ATTR_FLAGS, sizeof(result));
    return result;
}
static Record *find(LinuxPthread id) {
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
        if (records[i].id == id && id) return &records[i];
    return NULL;
}
static LinuxPthread newId(void) { return next_id && next_id != UINT64_MAX ? next_id++ : 0; }
int linuxPthreadAttrInit(LinuxPthreadAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    memset(attr, 0, sizeof(*attr));
    write64(attr, ATTR_GUARD, 4096);
    write64(attr, ATTR_SIZE, LINUX_THREAD_DEFAULT_STACK);
    return 0;
}
int linuxPthreadAttrDestroy(LinuxPthreadAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    if (read64(attr, 40)) return LINUX_ENOSYS;
    memset(attr, 0, sizeof(*attr));
    return 0;
}
int linuxPthreadAttrSetDetachState(LinuxPthreadAttr *attr, int state) {
    if (!attr || (state != 0 && state != 1)) return LINUX_EINVAL;
    if (state == 1) return LINUX_ENOSYS;
    uint32_t value = flags(attr) & ~1u;
    memcpy((unsigned char *)attr + ATTR_FLAGS, &value, sizeof(value));
    return 0;
}
int linuxPthreadAttrSetStackSize(LinuxPthreadAttr *attr, size_t bytes) {
    if (!attr || bytes < 16384 || bytes > SIZE_MAX - 4095) return LINUX_EINVAL;
    if (bytes > LINUX_THREAD_MAX_STACK) return LINUX_ENOSYS;
    write64(attr, ATTR_SIZE, bytes);
    return 0;
}
int linuxPthreadAttrGetStack(const LinuxPthreadAttr *attr, void **base, size_t *bytes) {
    if (!attr || !base || !bytes) return LINUX_EINVAL;
    uint64_t top = read64(attr, ATTR_TOP), size = read64(attr, ATTR_SIZE);
    if (top && top < size) return LINUX_EINVAL;
    *base = top ? (void *)(uintptr_t)(top - size) : NULL;
    *bytes = size;
    return 0;
}
int linuxPthreadAttrGetGuardSize(const LinuxPthreadAttr *attr, size_t *bytes) {
    if (!attr || !bytes) return LINUX_EINVAL;
    *bytes = read64(attr, ATTR_GUARD);
    return 0;
}
LinuxPthread linuxPthreadSelf(void) {
    if (!self_id) {
        lockRecords();
        self_id = newId();
        unlockRecords();
    }
    return self_id;
}
bool linuxPthreadManaged(LinuxPthread thread) {
    if (!thread) return false;
    lockRecords();
    bool managed = find(thread) != NULL;
    unlockRecords();
    return managed;
}
bool linuxPthreadExists(LinuxPthread thread) { return thread && (thread == linuxPthreadSelf() || linuxPthreadManaged(thread)); }
int linuxPthreadGetattr(LinuxPthread thread, LinuxPthreadAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    LinuxPthread self = linuxPthreadSelf();
    lockRecords();
    Record *record = find(thread);
    void *base = NULL;
    size_t size = 0, guard = 0;
    int error = record ? nativeStack(record->native, &base, &size, &guard) : (thread == self ? nativeCurrentStack(&base, &size, &guard) : LINUX_ESRCH);
    if (!error) {
        memset(attr, 0, sizeof(*attr));
        write64(attr, ATTR_TOP, (uintptr_t)base + size);
        write64(attr, ATTR_SIZE, size);
        write64(attr, ATTR_GUARD, guard);
        uint32_t value = 8;
        memcpy((unsigned char *)attr + ATTR_FLAGS, &value, sizeof(value));
    }
    unlockRecords();
    return error;
}
static void destructValues(void) {
    for (unsigned pass = 0; pass < 4; ++pass) {
        bool called = false;
        for (unsigned i = 0; i < LINUX_THREAD_MAX_KEYS; ++i) {
            lockRecords();
            void *value = values[i].value;
            void (*destructor)(void *) = NULL;
            if (keys[i].used && values[i].generation == keys[i].generation && value) destructor = keys[i].destructor;
            values[i].value = NULL;
            unlockRecords();
            if (destructor) {
                called = true;
                destructor(value);
            }
        }
        if (!called) break;
    }
    memset(values, 0, sizeof(values));
}
static void entryWrapper(void *argument) {
    Record *record = argument;
    self_id = record->id;
    linuxThreadsCurrentStarted();
    // exit()/abort() inside the entry unwind to here: the thread still runs its
    // TLS destructors and publishes a NULL result, so it can be joined normally.
    void *result = NULL;
    linuxRuntimeRun(record->entry, record->argument, &result);
    destructValues();
    lockRecords();
    record->result = result;
    record->exited = true;
    unlockRecords();
}
// Detached threads have no joiner: once they ended, the creator of the next thread joins and frees them.
static void reapDetached(void) {
    for (;;) {
        void *native = NULL;
        Record *record = NULL;
        lockRecords();
        for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
            if (records[i].id && records[i].detached && records[i].exited && records[i].started && !records[i].claimed) {
                record = &records[i];
                record->claimed = true;
                native = record->native;
                break;
            }
        unlockRecords();
        if (!record) return;
        int error = nativeJoin(native);
        if (!error) error = nativeClose(native);
        lockRecords();
        if (!error)
            memset(record, 0, sizeof(*record));
        else
            record->claimed = false;
        unlockRecords();
        if (error) return;
    }
}
int linuxPthreadDetach(LinuxPthread thread) {
    lockRecords();
    Record *record = find(thread);
    int error = !record ? LINUX_ESRCH : ((!record->started || record->claimed || record->detached) ? LINUX_EINVAL : 0);
    if (!error) record->detached = true;
    unlockRecords();
    return error;
}
int linuxPthreadCreate(LinuxPthread *thread, const LinuxPthreadAttr *attr, LinuxThreadEntry entry, void *argument) {
    if (!thread || !entry) return LINUX_EINVAL;
    reapDetached();
    LinuxPthreadAttr defaults;
    if (!attr) {
        linuxPthreadAttrInit(&defaults);
        attr = &defaults;
    }
    if (flags(attr) || read64(attr, 0) || read64(attr, 40) || read64(attr, ATTR_TOP)) return LINUX_ENOSYS;
    size_t stack = read64(attr, ATTR_SIZE), guard = read64(attr, ATTR_GUARD);
    if (!stack) stack = LINUX_THREAD_DEFAULT_STACK;
    if (stack < 16384 || stack > SIZE_MAX - 4095) return LINUX_EINVAL;
    if (stack > LINUX_THREAD_MAX_STACK || (guard != 0 && guard != 4096)) return LINUX_ENOSYS;
    stack = (stack + 4095) & ~(size_t)4095;
    lockRecords();
    Record *record = NULL;
    if (!shutting_down)
        for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
            if (!records[i].id) {
                record = &records[i];
                break;
            }
    int error = LINUX_EAGAIN;
    LinuxPthread id = record ? newId() : 0;
    if (id) {
        *record = (Record){.id = id, .entry = entry, .argument = argument};
        error = nativeCreate(&record->native, entryWrapper, record, stack, guard);
        if (!error) {
            error = nativeStart(record->native);
            if (!error) {
                record->started = true;
                *thread = id;
                ++created;
            }
        }
        if (error && (!record->native || !nativeClose(record->native))) memset(record, 0, sizeof(*record));
    }
    unlockRecords();
    return error;
}
int linuxPthreadJoin(LinuxPthread thread, void **result) {
    if (thread == linuxPthreadSelf()) return LINUX_EDEADLK;
    lockRecords();
    Record *record = find(thread);
    int error = !record ? LINUX_ESRCH : ((!record->started || record->claimed || record->detached) ? LINUX_EINVAL : 0);
    if (error) {
        unlockRecords();
        return error;
    }
    record->claimed = true;
    void *native = record->native;
    unlockRecords();
    error = nativeJoin(native);
    lockRecords();
    if (!error) error = nativeClose(native);
    if (!error) {
        if (result) *result = record->result;
        memset(record, 0, sizeof(*record));
    } else
        record->claimed = false;
    unlockRecords();
    return error;
}
int linuxPthreadKeyCreate(LinuxPthreadKey *key, void (*destructor)(void *)) {
    if (!key) return LINUX_EINVAL;
    lockRecords();
    int error = LINUX_EAGAIN;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_KEYS; ++i)
        if (!keys[i].used && keys[i].generation != UINT64_MAX) {
            ++keys[i].generation;
            keys[i].used = true;
            keys[i].destructor = destructor;
            *key = i;
            error = 0;
            break;
        }
    unlockRecords();
    return error;
}
int linuxPthreadKeyDelete(LinuxPthreadKey key) {
    lockRecords();
    int error = key >= LINUX_THREAD_MAX_KEYS || !keys[key].used ? LINUX_EINVAL : 0;
    if (!error) {
        keys[key].used = false;
        keys[key].destructor = NULL;
    }
    unlockRecords();
    return error;
}
void *linuxPthreadGetSpecific(LinuxPthreadKey key) {
    lockRecords();
    void *value = key < LINUX_THREAD_MAX_KEYS && keys[key].used && values[key].generation == keys[key].generation ? values[key].value : NULL;
    unlockRecords();
    return value;
}
int linuxPthreadSetSpecific(LinuxPthreadKey key, const void *value) {
    lockRecords();
    int error = key >= LINUX_THREAD_MAX_KEYS || !keys[key].used ? LINUX_EINVAL : 0;
    if (!error) values[key] = (Value){keys[key].generation, (void *)value};
    unlockRecords();
    return error;
}
int linuxSchedYield(void) {
    linuxThreadYield();
    return 0;
}
unsigned linuxThreadsLiveUnlocked(void) {
    unsigned live = 0;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i) {
        volatile const Record *record = &records[i];
        if (record->id && record->started && !record->exited) ++live;
    }
    return live;
}
unsigned linuxThreadsSnapshot(LinuxThreadSnapshot *out, unsigned max) {
    unsigned count = 0;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS && count < max; ++i) {
        volatile const Record *record = &records[i];
        if (!record->id || !record->started || record->exited || !record->native) continue;
        out[count++] = (LinuxThreadSnapshot){record->id, nativeHandle(record->native), true, false};
    }
    return count;
}
LinuxThreadStats linuxThreadsStats(void) {
    lockRecords();
    LinuxThreadStats stats = {.created = created};
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
        if (records[i].id) {
            ++stats.threads;
            stats.live_threads += records[i].started && !records[i].exited;
        }
    unlockRecords();
    return stats;
}
bool linuxThreadsReset(void) {
    LinuxPthread self = linuxPthreadSelf();
    lockRecords();
    if (shutting_down) {
        unlockRecords();
        return false;
    }
    shutting_down = true;
    bool ok = true;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
        if (records[i].id) {
            Record *record = &records[i];
            if (record->claimed || record->id == self) {
                ok = false;
                continue;
            }
            record->claimed = true;
            void *native = record->native;
            bool started = record->started;
            unlockRecords();
            int error = started ? nativeJoin(native) : 0;
            lockRecords();
            if (!error) error = nativeClose(native);
            if (error) {
                record->claimed = false;
                ok = false;
            } else
                memset(record, 0, sizeof(*record));
        }
    if (ok) {
        for (unsigned i = 0; i < LINUX_THREAD_MAX_KEYS; ++i) {
            keys[i].used = false;
            keys[i].destructor = NULL;
        }
        memset(values, 0, sizeof(values));
        created = 0;
    }
    shutting_down = false;
    unlockRecords();
    return ok;
}
__attribute__((weak)) void linuxThreadsCurrentStarted(void) {}
