// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_jit.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: one page per closure, made read+execute once written (no pool switching between writable and executable views,
// so a closure that is running is never made non-executable under another thread).
#include "linux_jit.h"
#include "diagnostics.h"
#include "platform.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned allocations, preparations;
typedef int (*PrepClosure)(void *closure, void *cif, void *function, void *user_data, void *code);
typedef int (*PrepClosureJni)(void *env, void *clazz, intptr_t closure, intptr_t cif, intptr_t function, intptr_t user_data, intptr_t code);
static PrepClosure original_prepare;
static PrepClosureJni original_prepare_jni;

static void *closureAlloc(size_t size, void **code) {
    size_t page = platformPageSize();
    if (!size || !code || size > page) return NULL;
    void *memory = platformAllocatePages(page);
    pthread_mutex_lock(&lock);
    ++allocations;
    if (allocations <= 8 || !memory) diagnosticsTrace("jit.alloc=%s size=%zu page=%p", memory ? "PASS" : "FAIL", size, memory);
    pthread_mutex_unlock(&lock);
    if (memory) *code = memory;  // x86-64: the closure runs where it is written
    return memory;
}
static void closureFree(void *closure) { (void)closure; }
static int seal(void *closure, int status) {
    enum { FFI_BAD_ABI = 2 };
    int error = platformProtect((void *)((uintptr_t)closure & ~(uintptr_t)(platformPageSize() - 1)), platformPageSize(),
                                PLATFORM_PROT_READ | PLATFORM_PROT_EXEC);
    pthread_mutex_lock(&lock);
    ++preparations;
    if (preparations <= 8 || error) diagnosticsTrace("jit.prepare status=%d closure=%p seal_error=%d", status, closure, error);
    pthread_mutex_unlock(&lock);
    return error ? FFI_BAD_ABI : status;
}
static int prepareClosure(void *closure, void *cif, void *function, void *user_data, void *code) {
    enum { FFI_BAD_ABI = 2 };
    if (!original_prepare) return FFI_BAD_ABI;
    return seal(closure, original_prepare(closure, cif, function, user_data, code));
}
// LWJGL links libffi statically and does not export it: its Java side reaches libffi through these JNI entry points (env, class, ...).
static intptr_t closureAllocJni(void *env, void *clazz, size_t size, void **code) {
    (void)env;
    (void)clazz;
    return (intptr_t)closureAlloc(size, code);
}
static void closureFreeJni(void *env, void *clazz, intptr_t closure) {
    (void)env;
    (void)clazz;
    (void)closure;
}
static int prepareClosureJni(void *env, void *clazz, intptr_t closure, intptr_t cif, intptr_t function, intptr_t user_data, intptr_t code) {
    enum { FFI_BAD_ABI = 2 };
    if (!original_prepare_jni) return FFI_BAD_ABI;
    return seal((void *)closure, original_prepare_jni(env, clazz, closure, cif, function, user_data, code));
}

uintptr_t linuxJitOverride(const char *name, uintptr_t original) {
    if (!name || !original) return original;
    if (strstr(name, "closure") && (!strncmp(name, "ffi_", 4) || !strncmp(name, "Java_org_lwjgl_system_libffi_", 29)))
        diagnosticsTrace("jit.override=%s", name);
    if (!strcmp(name, "ffi_closure_alloc")) return (uintptr_t)closureAlloc;
    if (!strcmp(name, "ffi_closure_free")) return (uintptr_t)closureFree;
    if (!strcmp(name, "ffi_prep_closure_loc")) {
        original_prepare = (PrepClosure)original;
        return (uintptr_t)prepareClosure;
    }
    if (!strcmp(name, "Java_org_lwjgl_system_libffi_LibFFI_nffi_1closure_1alloc")) return (uintptr_t)closureAllocJni;
    if (!strcmp(name, "Java_org_lwjgl_system_libffi_LibFFI_nffi_1closure_1free")) return (uintptr_t)closureFreeJni;
    if (!strcmp(name, "Java_org_lwjgl_system_libffi_LibFFI_nffi_1prep_1closure_1loc")) {
        original_prepare_jni = (PrepClosureJni)original;
        return (uintptr_t)prepareClosureJni;
    }
    return original;
}
void linuxJitReset(void) {
    pthread_mutex_lock(&lock);
    original_prepare = NULL;
    original_prepare_jni = NULL;
    allocations = preparations = 0;
    pthread_mutex_unlock(&lock);
}
