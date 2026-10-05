@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem PokeMMO-Prospero installer launcher for Windows: double-click this file.
setlocal
cd /d "%~dp0"
set POKEMMO_PROSPERO_PAUSE=1
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 pokemmo_prospero_install.py %*
    exit /b
)
where python >nul 2>nul
if %errorlevel%==0 (
    python pokemmo_prospero_install.py %*
    exit /b
)
echo Python 3 is required. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH" in the installer), then double-click this file again.
pause
exit /b 1
