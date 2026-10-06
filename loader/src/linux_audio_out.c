// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_audio_out.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: for now only the Switch port's silent fallback: a thread pulls the mixer at the real-time pace and throws the sound away,
// so the game's streaming sources keep moving. The PS5's audio output comes once its functions can be reached (loader-2: the module
// loads at run time but its symbols cannot be looked up).
#include "linux_audio.h"
#include "diagnostics.h"
#include "platform.h"
#include <pthread.h>
#include <stdatomic.h>

#define BLOCK_FRAMES 1024u
static pthread_t thread;
static atomic_bool running;
static bool thread_started;

static void *outputThread(void *unused) {
    (void)unused;
    static int16_t scratch[BLOCK_FRAMES * 2];
    const uint64_t period = (uint64_t)BLOCK_FRAMES * 1000000000u / LINUX_AUDIO_RATE;
    uint64_t next = platformMonotonicNs();
    diagnosticsTrace("audio.out=SILENT (no PS5 audio output yet)");
    while (atomic_load(&running)) {
        next += period;
        linuxAudioMix(scratch, BLOCK_FRAMES);
        uint64_t now = platformMonotonicNs();
        if (now < next)
            platformSleepNs(next - now);
        else
            next = now;
    }
    return NULL;
}
static bool outStart(void) {
    if (thread_started) return true;
    atomic_store(&running, true);
    if (pthread_create(&thread, NULL, outputThread, NULL)) {
        atomic_store(&running, false);
        return false;
    }
    thread_started = true;
    return true;
}
static void outStop(void) {
    if (!thread_started) return;
    atomic_store(&running, false);
    pthread_join(thread, NULL);
    thread_started = false;
}
static void outYield(void) { platformSleepNs(50000); }

void linuxAudioOutAttach(void) {
    static const LinuxAudioOutput hooks = {outStart, outStop, outYield};
    linuxAudioSetOutput(&hooks);
}
