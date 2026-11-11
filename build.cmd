@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (c) 2026 Joaquin Villodre
rem
rem Builds everything: double-click it, or run it from a command prompt.
rem It runs build.ps1 without touching the PowerShell execution policy
rem (scripts unpacked from a downloaded ZIP are blocked by default).
rem
rem   build.cmd                 Release build, unit tests, dist\*.zip
rem   build.cmd -Clean          the same from scratch
rem   build.cmd -InstallTools   first install the Visual Studio Build Tools (winget)
rem   build.cmd -Config Debug   a debug build
rem
rem Everything it prints also goes to build-log.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
