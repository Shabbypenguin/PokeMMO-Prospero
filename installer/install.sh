#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# PokeMMO-Prospero installer launcher for Linux (and macOS from a terminal).
cd "$(dirname "$0")" || exit 1
if command -v python3 >/dev/null 2>&1; then
    exec python3 pokemmo_prospero_install.py "$@"
fi
echo "Python 3 is required. Install it with your package manager (e.g. sudo apt install python3) and run this again."
exit 1
