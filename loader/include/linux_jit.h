// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_jit.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the PS5 mechanism (a page is written, then switched to read+execute; it is never writable and executable at once).
#pragma once
#include <stdint.h>

// Closures for libffi (LWJGL upcalls: SDL log output, event filter, GL debug output ...).
//
// libffi builds a small piece of machine code for every closure and wants memory that is writable and executable. A PS5 title
// cannot run such memory (probe: exec.rwx faults), but a page that is written and then switched to read+execute works
// (exec.mprotect). The loader therefore replaces three of libffi's functions when the client looks them up:
//   ffi_closure_alloc     hands out a fresh, writable page per closure
//   ffi_prep_closure_loc  lets libffi write its trampoline, then switches the page to read+execute for good
//   ffi_closure_free      does nothing (pages are not reused)
// LWJGL links libffi statically, so its JNI entry points are replaced the same way. Everything else of libffi stays untouched.
uintptr_t linuxJitOverride(const char *name, uintptr_t original);  // the replacement, or `original` when `name` is not replaced
void linuxJitReset(void);
