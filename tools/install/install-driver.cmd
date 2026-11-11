@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (c) 2026 Joaquin Villodre
rem
rem Installs AJA Duplex ASIO: copies the driver to
rem "Program Files\AJA Duplex ASIO" and registers that copy, so hosts list
rem "AJA Duplex ASIO" and go on finding it whatever happens to this folder
rem afterwards. (Up to 0.1.21 the driver was registered where this folder
rem was: with the folder deleted, moved or made again, hosts still listed
rem the driver and could not open it.) Run it again to update the driver,
rem with every host closed: Windows does not let a DLL in use be replaced.
rem Registering also takes away the second driver that versions up to 0.1.19
rem registered for trying the driver without the card ("AJA Duplex
rem Simulator", and "AJA Duplex ASIO (simulated)" up to 0.1.16, with which
rem vMix left the card's driver out of its inputs).
rem Needs an administrator prompt: right-click, Run as administrator.
rem It runs next to the DLL, in the folder of the ZIP that build.cmd makes
rem (dist\AJA-Duplex-ASIO-*-win64); the copy in the source tree has no DLL
rem beside it.
rem (Paths are only used outside parentheses: one with a ")" in it, as
rem "Program Files (x86)" has, would end the block early.)

setlocal
net session >nul 2>&1
if errorlevel 1 goto notadmin

set "source=%~dp0AJADuplexASIO.dll"
set "folder=%ProgramW6432%"
if not defined folder set "folder=%ProgramFiles%"
set "folder=%folder%\AJA Duplex ASIO"
set "target=%folder%\AJADuplexASIO.dll"

if not exist "%source%" goto nodll
rem Run from the installed folder itself: nothing to copy.
if /i "%source%"=="%target%" goto register
if not exist "%folder%\" mkdir "%folder%"
if not exist "%folder%\" goto nofolder
copy /y /b "%source%" "%target%" >nul 2>&1
if not errorlevel 1 goto copied
rem Not replaced: a host has the installed driver open. If what is there is
rem this very file already, there was nothing to replace.
if not exist "%target%" goto nocopy
"%SystemRoot%\System32\fc.exe" /b "%source%" "%target%" >nul 2>&1
if errorlevel 1 goto nocopy
goto register

:copied
if exist "%~dp0LICENSE.txt" copy /y "%~dp0LICENSE.txt" "%folder%\LICENSE.txt" >nul 2>&1
if exist "%~dp0THIRD_PARTY_NOTICES.md" copy /y "%~dp0THIRD_PARTY_NOTICES.md" "%folder%\THIRD_PARTY_NOTICES.md" >nul 2>&1

:register
regsvr32 /s "%target%"
set "code=%errorlevel%"
if not "%code%"=="0" goto failed
echo AJA Duplex ASIO is installed in %folder%
echo Hosts list "AJA Duplex ASIO" and load it from there: this folder can be
echo moved, replaced or deleted. To update the driver, close every host and run
echo the new version's install-driver.cmd; to take it away, uninstall-driver.cmd.
pause
exit /b 0

:notadmin
echo This needs administrator rights: right-click install-driver.cmd, Run as administrator.
pause
exit /b 1

:nodll
echo AJADuplexASIO.dll is not in %~dp0
echo Run install-driver.cmd from the folder of the ZIP that build.cmd makes
echo (dist\AJA-Duplex-ASIO-^<version^>-win64), where the DLL is beside it.
pause
exit /b 1

:nofolder
echo The folder %folder% could not be made.
pause
exit /b 1

:nocopy
echo The driver could not be copied to %folder%
echo A host has the installed driver open (Resolume, vMix, a DAW): close every
echo host and run install-driver.cmd again. The driver installed before is
echo still there, as it was.
pause
exit /b 1

:failed
echo Registering %target% failed: regsvr32 exit code %code%
echo (3: Windows could not load the DLL; 5: the DLL could not register itself).
pause
exit /b 1
