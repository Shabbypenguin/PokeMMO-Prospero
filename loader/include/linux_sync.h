// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// Interface after PokeMMO-NX's include/linux_sync.h (Petit_Prince, MIT); x86-64 layouts and a different implementation.
#pragma once
#include "linux_abi.h"
#include <stdint.h>

// glibc x86-64 objects. The guest owns their memory; the native POSIX object behind each one is created on first use and its
// address kept inside the guest object (static initializers are all zero, so a zero slot means "not created yet").
typedef struct {
    uint64_t words[5];
} LinuxMutex;  // 40 bytes; glibc keeps the kind (normal/recursive/errorcheck) at offset 16
typedef struct {
    uint64_t words[6];
} LinuxCondition;  // 48 bytes
typedef struct {
    uint32_t value;
} LinuxMutexAttr;  // 4 bytes: the kind
typedef struct {
    uint32_t value;
} LinuxConditionAttr;  // 4 bytes: bit 0 shared, the clock above it (this layer's own encoding)
int linuxPthreadMutexInit(LinuxMutex *, const LinuxMutexAttr *);
int linuxPthreadMutexDestroy(LinuxMutex *);
int linuxPthreadMutexLock(LinuxMutex *);
int linuxPthreadMutexTryLock(LinuxMutex *);
int linuxPthreadMutexUnlock(LinuxMutex *);
int linuxPthreadCondAttrInit(LinuxConditionAttr *);
int linuxPthreadCondAttrDestroy(LinuxConditionAttr *);
int linuxPthreadCondAttrSetClock(LinuxConditionAttr *, int);
int linuxPthreadCondInit(LinuxCondition *, const LinuxConditionAttr *);
int linuxPthreadCondDestroy(LinuxCondition *);
int linuxPthreadCondWait(LinuxCondition *, LinuxMutex *);
int linuxPthreadCondTimedWait(LinuxCondition *, LinuxMutex *, const LinuxTimespec *);
int linuxPthreadCondSignal(LinuxCondition *);
int linuxPthreadCondBroadcast(LinuxCondition *);
bool linuxSyncReset(void);
// The lock of the registries of the adapters (descriptors, streams). Short critical sections only.
void linuxSyncLock(void);
void linuxSyncUnlock(void);
// Linux error number for a native one (shared by the adapters that call POSIX functions).
int linuxSyncErrno(int native_error);
