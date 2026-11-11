@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (c) 2026 Joaquin Villodre
rem
rem Takes AJA Duplex ASIO away: its registration, the copy that
rem install-driver.cmd put in "Program Files\AJA Duplex ASIO", and what
rem versions up to 0.1.19 registered for their simulated driver. The
rem settings under HKEY_CURRENT_USER\Software\AJA Duplex ASIO and the
rem driver's log are left alone. Close every host first.
rem Needs an administrator prompt: right-click, Run as administrator.
rem Any copy of this file does it, from any folder: it asks the installed
rem DLL to take its registration away, or the DLL beside it (what versions
rem up to 0.1.21 registered where they were), and then deletes the driver's
rem two registry keys by name, so that it also works with no DLL left
rem anywhere.
rem (Paths are only used outside parentheses: one with a ")" in it, as
rem "Program Files (x86)" has, would end the block early.)

setlocal
net session >nul 2>&1
if errorlevel 1 goto notadmin

set "folder=%ProgramW6432%"
if not defined folder set "folder=%ProgramFiles%"
set "folder=%folder%\AJA Duplex ASIO"
set "target=%folder%\AJADuplexASIO.dll"
set "dll=%target%"
if not exist "%dll%" set "dll=%~dp0AJADuplexASIO.dll"

rem The DLL takes away its own registration and the former simulated
rem driver's with it.
if exist "%dll%" regsvr32 /u /s "%dll%"
rem Whatever is left of the driver's class and of its key in the hosts' list
rem (src\asio\win\DriverIdentity.h): no DLL to ask, or one that did not load.
rem In the 64-bit registry, where 64-bit hosts look, also from a 32-bit prompt.
reg delete "HKLM\SOFTWARE\Classes\CLSID\{1423D042-ACD6-4FF8-AD72-A1A7AD036043}" /f /reg:64 >nul 2>&1
reg delete "HKLM\SOFTWARE\ASIO\AJA Duplex ASIO" /f /reg:64 >nul 2>&1

if not exist "%target%" goto removed
del /f /q "%target%" >nul 2>&1
if exist "%target%" goto inuse

:removed
if exist "%folder%\LICENSE.txt" del /f /q "%folder%\LICENSE.txt" >nul 2>&1
if exist "%folder%\THIRD_PARTY_NOTICES.md" del /f /q "%folder%\THIRD_PARTY_NOTICES.md" >nul 2>&1
if exist "%folder%\" rmdir "%folder%" >nul 2>&1
echo AJA Duplex ASIO is no longer installed.
pause
exit /b 0

:notadmin
echo This needs administrator rights: right-click uninstall-driver.cmd, Run as administrator.
pause
exit /b 1

:inuse
echo AJA Duplex ASIO is no longer registered, but this file could not be deleted:
echo %target%
echo A host still has it open. Close every host and run uninstall-driver.cmd again.
pause
exit /b 1
