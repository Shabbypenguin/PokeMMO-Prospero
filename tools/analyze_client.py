#!/usr/bin/env python3
"""Check a PokeMMO client release against what the PS5 loader supports.

Run it on every new client revision (scripts/ps5env make analyze). It reports the facts the loader
depends on and exits non-zero when the client needs something the loader has not been taught yet:

  imports       every libc/zlib symbol the client binds; new ones need a shim
  relocations   relocation types the ELF loader must apply
  tls           a PT_TLS segment would need a Linux TLS block behind %fs
  syscalls      raw `syscall` instructions bypass libc and would hit the PS5 kernel with Linux numbers
  segment regs  %fs/%gs accesses (only the %fs:0x28 stack canary is expected)
  libraries     shared objects the client dlopen()s at runtime (each needs a virtual library)

Needs binutils (readelf, objdump), present in the toolchain image.
"""

import argparse
import collections
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

MEMBER = "bin/linux/x64/PokeMMO"
BASELINE = Path(__file__).with_name("client-baseline.txt")
EXPECTED_RELOCATIONS = {"R_X86_64_64", "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT", "R_X86_64_RELATIVE"}
EXPECTED_SEGMENT_ACCESSES = {"%fs:0x28"}
LIBRARY_PATTERN = re.compile(rb"lib[A-Za-z0-9_+-]+\.so(?:\.[0-9]+)*")


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def load_baseline():
    if not BASELINE.exists():
        return set(), set()
    imports, libraries = set(), set()
    for line in BASELINE.read_text().splitlines():
        kind, _, value = line.partition(" ")
        (imports if kind == "import" else libraries if kind == "library" else set()).add(value)
    return imports, libraries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("client", type=Path, help="PokeMMO-Client.zip")
    parser.add_argument("--write-baseline", action="store_true", help="accept this revision's imports/libraries as supported")
    args = parser.parse_args()

    with zipfile.ZipFile(args.client) as archive, tempfile.TemporaryDirectory() as scratch:
        revision = archive.read("revision.txt").decode().strip() if "revision.txt" in archive.namelist() else "?"
        binary = Path(scratch) / "PokeMMO"
        binary.write_bytes(archive.read(MEMBER))
        data = binary.read_bytes()

        header = run("readelf", "-hlW", str(binary))
        dynamic = run("readelf", "-dW", str(binary))
        symbols = run("readelf", "-W", "--dyn-syms", str(binary))
        relocations = run("readelf", "-rW", str(binary))
        disassembly = run("objdump", "-d", "--no-show-raw-insn", str(binary))

    problems = []
    print(f"client revision {revision}, {MEMBER}: {len(data) / 2**20:.1f} MiB")
    kind = "PIE" if "DYN (Position-Independent" in header else "not PIE"
    if "Advanced Micro Devices X86-64" not in header:
        problems.append("binary is not x86-64")
    print(f"  type: {kind}")

    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic)
    print(f"  NEEDED: {', '.join(needed)}")
    unexpected_needed = set(needed) - {"libc.so.6", "libz.so.1", "libm.so.6", "libdl.so.2", "libpthread.so.0"}
    if unexpected_needed:
        problems.append(f"new NEEDED libraries: {sorted(unexpected_needed)}")

    imports = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[6] == "UND" and fields[7]:
            imports.add(fields[7].split("@")[0])
    known_imports, known_libraries = load_baseline()
    new_imports = sorted(imports - known_imports) if known_imports else []
    gone_imports = sorted(known_imports - imports) if known_imports else []
    print(f"  imports: {len(imports)} ({len(new_imports)} new, {len(gone_imports)} no longer used)")
    for name in new_imports:
        print(f"    NEW   {name}")
    for name in gone_imports:
        print(f"    gone  {name}")
    if new_imports:
        problems.append(f"{len(new_imports)} new imports need shims")

    types = collections.Counter(re.findall(r"\bR_X86_64_[A-Z0-9_]+", relocations))
    print("  relocations: " + ", ".join(f"{name}={count}" for name, count in sorted(types.items())))
    if set(types) - EXPECTED_RELOCATIONS:
        problems.append(f"new relocation types: {sorted(set(types) - EXPECTED_RELOCATIONS)}")

    has_tls = re.search(r"^\s+TLS\s", header, re.M) is not None
    print(f"  PT_TLS segment: {'yes' if has_tls else 'no'}")
    if has_tls:
        problems.append("client now has a PT_TLS segment (needs a Linux TLS block behind %fs)")

    syscalls = len(re.findall(r"\tsyscall\b", disassembly))
    print(f"  raw syscall instructions: {syscalls}")
    if syscalls:
        problems.append(f"{syscalls} raw syscall instructions")

    accesses = collections.Counter(re.findall(r"%[fg]s:[^,\s)]*", disassembly))
    print("  segment-register accesses: " + (", ".join(f"{name} x{count}" for name, count in accesses.most_common()) or "none"))
    if set(accesses) - EXPECTED_SEGMENT_ACCESSES:
        problems.append(f"new segment-register accesses: {sorted(set(accesses) - EXPECTED_SEGMENT_ACCESSES)}")

    libraries = {match.decode() for match in LIBRARY_PATTERN.findall(data)}
    new_libraries = sorted(libraries - known_libraries) if known_libraries else []
    print(f"  dlopen candidates: {len(libraries)} ({len(new_libraries)} new)")
    for name in new_libraries:
        print(f"    NEW   {name}")

    versions = collections.Counter(re.findall(rb"#version [0-9]+(?: core| es| compatibility)?", data))
    legacy = len(re.findall(rb"gl_FragColor|texture2D\(", data))
    print("  GLSL: " + ", ".join(f"{v.decode()} x{c}" for v, c in versions.items()) + f"; legacy-builtin uses: {legacy}")

    if args.write_baseline:
        lines = [f"import {name}" for name in sorted(imports)] + [f"library {name}" for name in sorted(libraries)]
        BASELINE.write_text("\n".join(lines) + "\n")
        print(f"baseline written: {BASELINE.name} ({len(imports)} imports, {len(libraries)} libraries)")
        return 0

    if problems:
        print("\nNEEDS LOADER WORK:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("\nOK: nothing the loader does not already know about")
    return 0


if __name__ == "__main__":
    sys.exit(main())
