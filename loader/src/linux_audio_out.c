// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_audio_out.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the platform's audio output (PS5: sceAudioOut, 48 kHz stereo) on a POSIX thread; when it cannot be opened, the Switch
// port's silent fallback keeps the mixer running at the real-time pace so the game's streaming sources keep moving.
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
    int handle = platformAudioOpen(BLOCK_FRAMES);
    if (handle >= 0) {
        unsigned blocks = 0;
        while (atomic_load(&running)) {
            linuxAudioMix(scratch, BLOCK_FRAMES);
            int rc = platformAudioWrite(handle, scratch);
            if (rc < 0) {
                diagnosticsTrace("audio.out=WRITE_FAILED rc=0x%x after %u blocks: continuing silently", rc, blocks);
                break;
            }
            if (++blocks == 1) diagnosticsTrace("audio.out=PLAYING");
        }
        platformAudioClose(handle);
    }
    uint64_t next = platformMonotonicNs();
    if (atomic_load(&running)) diagnosticsTrace("audio.out=SILENT open=0x%x", handle);
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
