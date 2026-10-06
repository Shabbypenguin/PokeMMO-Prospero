#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PokeMMO-Prospero contributors
# The C++ runtime the client's native libraries need (libgdx's sound and emulator libraries link libstdc++ and libgcc_s, which a
# Linux system provides). Copied from the build image's Ubuntu 24.04 x86-64 packages (libstdc++6, libgcc-s1; GCC 14), unmodified.
# License: GPL-3.0 with the GCC Runtime Library Exception (redistribution allowed); source: the Ubuntu gcc-14 source package.
# PokeMMO-NX does the same on the Switch with Debian's ARM64 builds (tools/fetch_runtime_libs.py).
set -euo pipefail
out=${1:?output folder}
mkdir -p "$out"
for library in libstdc++.so.6 libgcc_s.so.1; do
    source=$(readlink -f "/usr/lib/x86_64-linux-gnu/$library")
    [[ -s $source ]] || { echo "missing $library in the build image" >&2; exit 2; }
    cp "$source" "$out/$library"
done
cat > "$out/NOTICE.txt" <<'TEXT'
libstdc++.so.6 and libgcc_s.so.1: unmodified binaries from Ubuntu 24.04 (packages libstdc++6 and libgcc-s1, GCC 14).
Copyright Free Software Foundation, Inc. GPL-3.0 with the GCC Runtime Library Exception 3.1.
Source: https://launchpad.net/ubuntu/+source/gcc-14
TEXT
ls -la "$out"
