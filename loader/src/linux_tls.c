// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// Design after PokeMMO-NX's source/linux_tls.c (Petit_Prince, MIT); rewritten for the x86-64 general-dynamic model.
#include "linux_tls.h"
#include "diagnostics.h"
#include "platform.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t file_bytes, memory_bytes, alignment;
    unsigned char *image;
} Module;
static Module modules[LINUX_TLS_MAX_MODULES + 1];  // index = module number; 0 unused
static _Atomic unsigned module_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Per thread: one block pointer per module (blocks are allocated on first use).
typedef struct {
    void *blocks[LINUX_TLS_MAX_MODULES + 1];
    void *raw[LINUX_TLS_MAX_MODULES + 1];  // what malloc returned (blocks may be aligned inside)
} ThreadBlocks;
static pthread_key_t thread_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static void freeBlocks(void *value) {
    ThreadBlocks *blocks = value;
    if (!blocks) return;
    for (unsigned i = 0; i <= LINUX_TLS_MAX_MODULES; ++i) free(blocks->raw[i]);
    free(blocks);
}
static void makeKey(void) {
    if (pthread_key_create(&thread_key, freeBlocks)) platformFatal("tls: pthread_key_create failed");
}

bool linuxTlsRegisterModule(const void *image, size_t file_bytes, size_t memory_bytes, size_t alignment, size_t *module) {
    if (!module || file_bytes > memory_bytes || (file_bytes && !image)) return false;
    if (alignment < 16) alignment = 16;
    if (alignment & (alignment - 1) || alignment > 4096) return false;
    unsigned char *copy = NULL;
    if (file_bytes) {
        copy = malloc(file_bytes);
        if (!copy) return false;
        memcpy(copy, image, file_bytes);
    }
    pthread_mutex_lock(&lock);
    unsigned count = atomic_load(&module_count);
    bool ok = count < LINUX_TLS_MAX_MODULES;
    if (ok) {
        modules[count + 1] = (Module){file_bytes, memory_bytes, alignment, copy};
        atomic_store(&module_count, count + 1);
        *module = count + 1;
    }
    pthread_mutex_unlock(&lock);
    if (!ok) free(copy);
    return ok;
}

void *linuxTlsGetAddr(const LinuxTlsIndex *index) {
    pthread_once(&key_once, makeKey);
    ThreadBlocks *blocks = pthread_getspecific(thread_key);
    if (!blocks) {
        blocks = calloc(1, sizeof(*blocks));
        if (!blocks || pthread_setspecific(thread_key, blocks)) platformFatal("tls: no memory for a thread's blocks");
    }
    uint64_t number = index->module;
    if (!number || number > atomic_load(&module_count)) platformFatal("tls: unknown module");
    if (!blocks->blocks[number]) {
        const Module *module = &modules[number];
        size_t bytes = module->memory_bytes ? module->memory_bytes : 1;
        unsigned char *raw = calloc(1, bytes + module->alignment);
        if (!raw) platformFatal("tls: no memory for a block");
        unsigned char *block = (unsigned char *)(((uintptr_t)raw + module->alignment - 1) & ~(uintptr_t)(module->alignment - 1));
        if (module->file_bytes) memcpy(block, module->image, module->file_bytes);
        blocks->raw[number] = raw;
        blocks->blocks[number] = block;
    }
    return (unsigned char *)blocks->blocks[number] + index->offset;
}

bool linuxTlsReset(void) {
    pthread_mutex_lock(&lock);
    unsigned count = atomic_load(&module_count);
    for (unsigned i = 1; i <= count; ++i) {
        free(modules[i].image);
        memset(&modules[i], 0, sizeof(modules[i]));
    }
    atomic_store(&module_count, 0);
    pthread_mutex_unlock(&lock);
    return true;
}
unsigned linuxTlsModuleCount(void) { return atomic_load(&module_count); }
