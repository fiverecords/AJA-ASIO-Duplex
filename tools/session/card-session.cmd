@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (c) 2026 Joaquin Villodre
rem
rem The session with the card, step by step (CARD_SESSION.md): before each
rem step it says what to cable and set up, runs it, and at the end zips the
rem reports and the driver's log. Double-click it, or from a command prompt
rem in this folder:
rem
rem   card-session              every step, from the first (it asks where to start)
rem   card-session 5c           from step 5c on
rem   card-session collect      only the ZIP of what there is
rem   card-session -Simulate    a short try on the simulated card
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0card-session.ps1" %*
exit /b %ERRORLEVEL%
