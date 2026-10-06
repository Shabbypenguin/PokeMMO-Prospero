// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/game.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: platform-neutral (folders, mounts and arguments come from the caller); the Switch's applet loop, docking, watchdog
// register dumps and process-exit sequence are not carried over. The virtual SDL3/EGL/OpenAL/GTK libraries come in a later
// milestone on a PC: there the client loads the real ones it carries, and the run ends where they need a display.
#include "game.h"
#include "diagnostics.h"
#include "elf_executable.h"
#include "linux_dl.h"
#include "linux_directories.h"
#include "linux_files.h"
#include "linux_net.h"
#include "linux_runtime.h"
#include "linux_stdio.h"
#include "linux_threads.h"
#include "linux_tls.h"
#include "linux_trap.h"
#include "linux_vm.h"
#include "platform.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

#define STACK_BYTES (16u * 1024u * 1024u)
#define LINE_BYTES 240u
#define MAX_ARGUMENTS 16u

typedef int (*MainFunction)(int argc, char **argv, char **environment);
static struct {
    uintptr_t main_entry;
    _Atomic bool returned;
    int rc;
    int argc;
    char *argv[MAX_ARGUMENTS + 2];
} run;
static ElfExecutable mapped;
static const char *failure = "";
static LinuxRuntimeExport exports[ELF_EXPORT_MAX];

// ---- the client's stdout/stderr: whole lines go to the log ----------------------------------------------------------------------
static char pending[3][LINE_BYTES + 1];
static size_t pending_bytes[3];
static atomic_flag capture_lock = ATOMIC_FLAG_INIT;
static void emitLine(int fd) {
    diagnosticsTrace("client.%s %.*s", fd == 1 ? "out" : "err", (int)pending_bytes[fd], pending[fd]);
    pending_bytes[fd] = 0;
}
static int64_t sink(void *context, int fd, const void *bytes, size_t count, int *error) {
    (void)context;
    if (fd < 1 || fd > 2) {
        *error = LINUX_EBADF;
        return -1;
    }
    while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) linuxThreadYield();
    const unsigned char *p = bytes;
    for (size_t i = 0; i < count; ++i) {
        if (p[i] == '\n' || pending_bytes[fd] == LINE_BYTES) {
            emitLine(fd);
            if (p[i] == '\n') continue;
        }
        pending[fd][pending_bytes[fd]++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    }
    atomic_flag_clear_explicit(&capture_lock, memory_order_release);
    return (int64_t)count;
}
static void flushCapture(void) {
    while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) linuxThreadYield();
    for (int fd = 1; fd <= 2; ++fd)
        if (pending_bytes[fd]) emitLine(fd);
    atomic_flag_clear_explicit(&capture_lock, memory_order_release);
}

static void logState(const char *when) {
    LinuxVmStats v = linuxVmStats();
    LinuxThreadStats t = linuxThreadsStats();
    diagnosticsTrace("game.state when=%s vm_arenas=%zu vm_active_pages=%zu vm_backing_mib=%zu threads=%zu live=%zu created=%zu", when, v.arenas,
                     v.active_pages, v.backing_bytes >> 20, t.threads, t.live_threads, t.created);
}

// Every import the client really called, with its first caller as an offset in the image, then the totals of the adapters.
static void summary(void) {
    LinuxTrapStats trap_stats = linuxTrapStats();
    for (unsigned i = 0; i < trap_stats.count; ++i) {
        unsigned n = linuxTrapCalls(i);
        if (!n) continue;
        uintptr_t caller = linuxTrapFirstCaller(i);
        diagnosticsTrace("game.trap.called name=%s calls=%u first_caller=0x%llx client_offset=0x%llx", linuxTrapName(i), n, (unsigned long long)caller,
                         (unsigned long long)(caller - (uintptr_t)mapped.base));
    }
    LinuxSyscallRecord syscalls[8];
    size_t unsupported = linuxRuntimeUnsupportedSyscalls(syscalls, 8);
    for (size_t i = 0; i < unsupported; ++i)
        diagnosticsTrace("game.syscall.unsupported number=%lld calls=%llu", (long long)syscalls[i].number, (unsigned long long)syscalls[i].calls);
    LinuxDlStats dl = linuxDlStats();
    diagnosticsTrace("game.dl libraries=%u virtual=%u fallback_stub_bindings=%u tls_modules=%u", dl.libraries, dl.virtual_libraries, dl.unresolved_imports,
                     dl.tls_modules);
    LinuxNetStats net = linuxNetStats();
    diagnosticsTrace("game.net sockets=%u open=%u connects=%u polls=%u resolves=%u failures=%u", net.sockets_created, net.sockets_open, net.connects, net.polls,
                     net.resolves, net.failures);
    diagnosticsTrace("game.trap.summary refused_calls=%llu requests=%u", (unsigned long long)trap_stats.refused, trap_stats.requests);
}

static void *clientEntry(void *argument) {
    (void)argument;
    run.rc = ((MainFunction)run.main_entry)(run.argc, run.argv, linuxRuntimeEnviron);
    atomic_store(&run.returned, true);
    return NULL;
}

const char *gameFailure(void) { return failure; }

static bool makeFolder(const char *root, const char *guest) {
    char path[LINUX_FILE_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", root, guest);
    return !mkdir(path, 0700) || errno == EEXIST;
}

bool gameRun(const GameConfig *config) {
    failure = "";
    memset(&run, 0, sizeof(run));
    memset(pending_bytes, 0, sizeof(pending_bytes));
    linuxStdioSetConsoleSink(sink, NULL);
    linuxRuntimeReset();
    linuxTrapReset();
    bool ok = linuxFilesSetRoot(config->root);
    for (unsigned i = 0; ok && i < config->mount_count; ++i) ok = linuxFilesAddMount(config->mounts[i].guest, config->mounts[i].native);
    // A running client expects a home and a /tmp, and starts in its install directory.
    static const char *const folders[] = {"/tmp", "/home", "/home/ps5"};
    for (unsigned i = 0; ok && i < sizeof(folders) / sizeof(folders[0]); ++i) ok = makeFolder(config->root, folders[i]);
    if (!ok) failure = "The client's folders could not be prepared.";
    if (ok && linuxAbiChdir("/game")) {
        failure = "The client's install folder (/game) is missing.";
        ok = false;
    }
    if (ok) ok = elfExecutableOpenResolved(config->client_path, &mapped, linuxTrapResolve, NULL);
    if (!ok || mapped.unresolved || mapped.unsupported) {
        if (!*failure) failure = "The PokeMMO client cannot be read (missing, corrupted or not enough memory).";
        return false;
    }
    linuxDlSetVirtualLibraries(config->virtual_libraries, config->virtual_count);
    linuxDlSetSearchDirectory("/lib");
    linuxDlSetFallbackResolver(linuxTrapResolve, NULL);
    linuxDlSetMainImage(&mapped);
    linuxRuntimeSetDynamicLoader(linuxDlLoaderHooks());
    run.main_entry = elfExecutableFindExport(&mapped, "main");
    unsigned count = 0;
    for (unsigned i = 0; i < mapped.export_count; ++i) {
        exports[count].name = mapped.exports[i].name;
        exports[count].address = mapped.exports[i].address;
        ++count;
    }
    linuxRuntimeSetExports(exports, count);
    diagnosticsTrace("game.mapping base=%p span=0x%llx slots_bound=%u trapped_imports=%u weak_null=%u exports=%u main=0x%llx", mapped.base,
                     (unsigned long long)mapped.layout.span, mapped.resolved, linuxTrapStats().count, mapped.weak_null, mapped.export_count,
                     (unsigned long long)run.main_entry);
    logState("before_start");
    if (!run.main_entry) {
        failure = "The PokeMMO client has no entry point.";
        return false;
    }

    static char program[] = "PokeMMO";
    run.argv[run.argc++] = program;
    for (unsigned i = 0; config->arguments && config->arguments[i] && run.argc < (int)MAX_ARGUMENTS + 1; ++i) run.argv[run.argc++] = (char *)config->arguments[i];
    run.argv[run.argc] = NULL;
    for (int i = 0; i < run.argc; ++i) diagnosticsTrace("game.argument index=%d value=%s", i, run.argv[i]);

    LinuxPthreadAttr attr;
    LinuxPthread worker;
    linuxPthreadAttrInit(&attr);
    linuxPthreadAttrSetStackSize(&attr, STACK_BYTES);
    if (linuxPthreadCreate(&worker, &attr, clientEntry, NULL)) {
        failure = "The game's thread could not be created.";
        return false;
    }

    const uint64_t begin = platformMonotonicNs();
    bool finished = false;
    unsigned ticks = 0;
    for (;;) {
        platformSleepNs(50u * 1000000u);
        ++ticks;
        if (ticks % 100 == 0) logState("waiting");
        // Done when main returned or when the client was unwound (exit/abort/trap) and its thread ended.
        finished = atomic_load(&run.returned) || linuxThreadsLiveUnlocked() == 0 || linuxRuntimeTerminationKindUnlocked() != LINUX_TERMINATION_NONE;
        if (finished) break;
        if (config->timeout_seconds && platformMonotonicNs() - begin > (uint64_t)config->timeout_seconds * 1000000000u) break;
    }
    flushCapture();
    summary();
    if (!finished) {
        diagnosticsTrace("game.result=TIMEOUT elapsed_ms=%llu", (unsigned long long)((platformMonotonicNs() - begin) / 1000000u));
        logState("timeout");
        failure = "The game did not finish in time.";
        return false;
    }
    LinuxTermination termination = linuxRuntimeTermination();
    bool exited_cleanly = termination.kind == LINUX_TERMINATION_EXIT && termination.status == 0;
    ok = (atomic_load(&run.returned) && run.rc == 0 && termination.kind == LINUX_TERMINATION_NONE) || exited_cleanly;
    logState("after_main");
    diagnosticsTrace("game.result=%s main_rc=%d termination_kind=%d status=%d elapsed_ms=%llu", ok ? "MAIN_RETURNED" : "ENDED_WITH_ERROR", run.rc,
                     (int)termination.kind, termination.status, (unsigned long long)((platformMonotonicNs() - begin) / 1000000u));
    if (!ok) failure = "The game stopped with an error.";
    return ok;
}
