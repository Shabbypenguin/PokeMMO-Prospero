// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_trap_stubs.s and
// source/linux_virtual_stub_table.s; Copyright (c) Petit_Prince, MIT license, LICENSES/PokeMMO-NX-MIT.txt).
// PS5 changes (PokeMMO-Prospero contributors): rewritten for x86-64 (System V ABI), as top-level assembly in a C file because the
// title build compiles C/C++ only; plus the __tls_get_addr entry.
//
// Trap stubs: one 16-byte stub per import without an adapter: mov $index, %r11d ; jmp linuxTrapCommon. r11 is a scratch register
// that carries no argument, so the caller's rdi..r9, rax (vector-argument count) and stack reach the common code untouched.
// Virtual stubs: the same for functions a virtual library is asked for but does not implement (they return 0).
#include "linux_trap.h"
#include "linux_virtual_stubs.h"

__asm__(".text\n"
        ".balign 16\n"
        ".globl linuxTrapStubs\n"
        "linuxTrapStubs:\n"
        ".set trap_index, 0\n"
        ".rept 512\n"
        "  movl $trap_index, %r11d\n"
        "  jmp linuxTrapCommon\n"
        "  .balign 16, 0xcc\n"
        "  .set trap_index, trap_index + 1\n"
        ".endr\n"
        // linuxTrapReport(index, caller) returns the refusal value in rax (or never returns). The return address of the client's call
        // sits at 8(%rbp) once the frame is built; the frame also realigns the stack to 16 bytes.
        "linuxTrapCommon:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  andq $-16, %rsp\n"
        "  movl %r11d, %edi\n"
        "  movq 8(%rbp), %rsi\n"
        "  call linuxTrapReport\n"
        "  movq %rbp, %rsp\n"
        "  popq %rbp\n"
        "  ret\n"
        ".balign 16\n"
        ".globl linuxVirtualStubTable\n"
        "linuxVirtualStubTable:\n"
        ".set stub_slot, 0\n"
        ".rept 1024\n"
        "  movl $stub_slot, %r11d\n"
        "  jmp linuxVirtualStubCommon\n"
        "  .balign 16, 0xcc\n"
        "  .set stub_slot, stub_slot + 1\n"
        ".endr\n"
        "linuxVirtualStubCommon:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  andq $-16, %rsp\n"
        "  movl %r11d, %edi\n"
        "  call linuxVirtualStubCalled\n"
        "  movq %rbp, %rsp\n"
        "  popq %rbp\n"
        "  ret\n"
        // __tls_get_addr: some compilers call it with a stack that is not 16-byte aligned (GCC bug 58066); glibc realigns, so do we.
        ".balign 16\n"
        ".globl linuxTlsGetAddrEntry\n"
        "linuxTlsGetAddrEntry:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  andq $-16, %rsp\n"
        "  call linuxTlsGetAddr\n"
        "  movq %rbp, %rsp\n"
        "  popq %rbp\n"
        "  ret\n");
