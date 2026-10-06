// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
// Interface after PokeMMO-NX's include/linux_semaphore.h (Petit_Prince, MIT).
#pragma once
#include "linux_abi.h"
#include <stdint.h>
typedef struct {
    uint64_t words[4];
} LinuxSemaphore;  // glibc x86-64 sem_t: 32 bytes; offset 0 holds this layer's native object
#define LINUX_SEM_VALUE_MAX UINT32_C(2147483647)
int linuxSemInit(LinuxSemaphore *, int shared, unsigned value);
int linuxSemDestroy(LinuxSemaphore *);
int linuxSemWait(LinuxSemaphore *);
int linuxSemPost(LinuxSemaphore *);
bool linuxSemReset(void);
