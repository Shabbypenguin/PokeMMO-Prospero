#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# pokemmo-ps5 installer launcher for macOS: double-click this file in Finder.
cd "$(dirname "$0")" || exit 1
if /usr/bin/python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
    POKEMMO_PS5_PAUSE=1 exec /usr/bin/python3 pokemmo_ps5_install.py "$@"
elif command -v python3 >/dev/null 2>&1; then
    POKEMMO_PS5_PAUSE=1 exec python3 pokemmo_ps5_install.py "$@"
fi
echo "Python 3 is required. macOS offers to install it (Command Line Tools) the first time you run python3,"
echo "or get it from https://www.python.org/downloads/ . Then double-click this file again."
read -r _
exit 1
