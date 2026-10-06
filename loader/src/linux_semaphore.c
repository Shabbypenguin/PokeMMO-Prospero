// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// Unnamed POSIX semaphores of the guest on a native mutex + condition. Interface after PokeMMO-NX (MIT).
#include "linux_semaphore.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(LinuxSemaphore) == 32, "glibc x86-64 sem_t");
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint32_t value;
} Native;
static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}
static Native *native(LinuxSemaphore *semaphore) {
    Native *value;
    memcpy(&value, semaphore, sizeof(value));
    return value;
}
int linuxSemInit(LinuxSemaphore *semaphore, int shared, unsigned value) {
    if (!semaphore || value > LINUX_SEM_VALUE_MAX) return fail(LINUX_EINVAL);
    if (shared) return fail(LINUX_ENOSYS);  // one process only
    Native *object = calloc(1, sizeof(*object));
    if (!object) return fail(LINUX_ENOMEM);
    pthread_mutex_init(&object->lock, NULL);
    pthread_cond_init(&object->ready, NULL);
    object->value = value;
    memset(semaphore, 0, sizeof(*semaphore));
    memcpy(semaphore, &object, sizeof(object));
    return 0;
}
int linuxSemDestroy(LinuxSemaphore *semaphore) {
    Native *object = semaphore ? native(semaphore) : NULL;
    if (!object) return fail(LINUX_EINVAL);
    pthread_cond_destroy(&object->ready);
    pthread_mutex_destroy(&object->lock);
    free(object);
    memset(semaphore, 0, sizeof(*semaphore));
    return 0;
}
int linuxSemWait(LinuxSemaphore *semaphore) {
    Native *object = semaphore ? native(semaphore) : NULL;
    if (!object) return fail(LINUX_EINVAL);
    pthread_mutex_lock(&object->lock);
    while (!object->value) pthread_cond_wait(&object->ready, &object->lock);
    --object->value;
    pthread_mutex_unlock(&object->lock);
    return 0;
}
int linuxSemPost(LinuxSemaphore *semaphore) {
    Native *object = semaphore ? native(semaphore) : NULL;
    if (!object) return fail(LINUX_EINVAL);
    pthread_mutex_lock(&object->lock);
    bool overflow = object->value == LINUX_SEM_VALUE_MAX;
    if (!overflow) ++object->value;
    pthread_cond_signal(&object->ready);
    pthread_mutex_unlock(&object->lock);
    return overflow ? fail(LINUX_EOVERFLOW) : 0;
}
bool linuxSemReset(void) { return true; }
