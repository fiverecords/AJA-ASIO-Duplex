# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Joaquin Villodre
#
# The session with the card (CARD_SESSION.md) as one guided run. Before
# each step it says what to cable and what to set up, then runs it; the
# reports go to reports\, and the last step zips them with the driver's
# log: the session's record. Run it with card-session.cmd, from the folder
# with duplex-probe.exe:
#
#   card-session              every step, from the first
#   card-session 5c           from step 5c on
#   card-session collect      only the ZIP of what there is
#   card-session help         the steps
#   card-session -Simulate    every step that can run on the simulated card,
#                             with short runs: to try the script out
#
# At each step Enter runs it, S skips it and Q stops. Ctrl+C stops the run
# in progress (the card is put back as it was) and the script with it:
# card-session <step> goes on from there.
#
# Hosts load the driver from Program Files\AJA Duplex ASIO, where step 6
# (install-driver.cmd) copies it, not from this folder: the session says at
# its start, after step 6 and before the steps with a host whether that copy
# is this folder's build (duplex-asiotest --installed).
#
# Every test of the session is a step here: installing the driver (6),
# vMix (6-vmix), what to try with AJA Input while Resolume plays (8) and
# the forced close (10): a program ended by force while it plays, and the
# next one taking the card over. Only if that leaves the card not clean is
# the PC restarted, and card-session 10-restart looks at the card again
# and makes the ZIP.

param(
    [string]$From = '',
    [switch]$Simulate
)

$here = $PSScriptRoot
$probe = Join-Path $here 'duplex-probe.exe'
$asiotest = Join-Path $here 'duplex-asiotest.exe'
$reports = Join-Path $here 'reports'
$notes = Join-Path $reports 'notes.txt'
$settingsKey = 'HKCU:\Software\AJA Duplex ASIO'
$logFolder = Join-Path $env:LOCALAPPDATA 'AJA Duplex ASIO'

$cableText = @{
    aes = 'the four BNC cables, AES out to AES in: 1-2 to 1-2, 3-4 to 3-4, 5-6 to 5-6, 7-8 to 7-8'
    xlr = 'the two XLR cables on the Xpand: XLR out L to XLR in L, XLR out R to XLR in R'
    sdi = 'an SDI cable from SDI 2 to SDI 1'
}
$script:connected = @{}
$script:restartNeeded = $false   # set by step 10 while the card is not left clean: the run stops there
$script:leftClean = $false       # set by step 10 when the takeover left the card clean: no 10-restart

$probeNote = 'Resolume closed, or at least not using AJA Duplex ASIO: the probe needs audio system 2 free.'
$loud = 'Monitors down: tones at +4 dBu on the outputs.'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Wait-Close {
    Write-Host ''
    Read-Host 'Press Enter to close this window' | Out-Null
}

function Get-Seconds([int]$Real, [int]$Simulated) {
    if ($Simulate) { return $Simulated }
    return $Real
}

function New-ReportFolder {
    if (-not (Test-Path $reports)) { New-Item -ItemType Directory -Path $reports | Out-Null }
}

# duplex-probe with its report in reports\ (--simulate with -Simulate); its
# output goes straight to the window. -Bare: the arguments as they are.
function Invoke-Probe([string[]]$Arguments, [switch]$Bare) {
    $all = @($Arguments)
    if (-not $Bare) {
        $all += @('--out', 'reports')
        if ($Simulate) { $all += '--simulate' }
    }
    Write-Host ('  > duplex-probe ' + ($all -join ' ')) -ForegroundColor DarkGray
    & $probe @all
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  duplex-probe ended with code $LASTEXITCODE (see above)." -ForegroundColor Yellow
    }
}

function Write-Ends([int]$Seconds) {
    $now = Get-Date
    Write-Host ('  Started ' + $now.ToString('HH:mm') + ', ends about ' + $now.AddSeconds($Seconds + 30).ToString('HH:mm') +
                '. Ctrl+C stops it.')
}

# The smallest G the sweep with this label found clean at N = 128, from
# its newest report.
function Get-SweepGuard([string]$Label) {
    if (-not (Test-Path $reports)) { return $null }
    $report = Get-ChildItem -Path $reports -Filter ('loop-' + $Label + '-*.txt') |
              Sort-Object LastWriteTime | Select-Object -Last 1
    if ($null -eq $report) { return $null }
    $text = Get-Content -Path $report.FullName -Raw
    $match = [regex]::Match($text, '(?m)^N 128\s*=\s*clean from G (\d+)')
    if ($match.Success) { return [int]$match.Groups[1].Value }
    return $null
}

# The G of the long runs: the driver's own, or the sweep's smallest clean
# one if that is larger. A sweep's runs last seconds: they say which guards
# fail, not which one holds for an hour, and with video being captured an
# audio DMA waited 2.8 ms on the KONA X (2026-11-11), where G = 8 gives a
# block 2.8 ms in all.
$driverGuard = 256

function Read-Guard([string]$SweepLabel) {
    $found = Get-SweepGuard $SweepLabel
    $default = $driverGuard
    if ($null -ne $found) {
        if ($found -gt $driverGuard) { $default = $found }
        Write-Host ('  The sweep was clean from G = ' + $found + ' at N = 128, in runs of seconds. A long run takes')
        Write-Host ("  the driver's own guard, " + $driverGuard + ', which is what hosts run with: one slow block in')
        Write-Host '  an hour fails a smaller one.'
    }
    else {
        Write-Host ('  No G at N = 128 from the sweep (not run, or nothing clean at N = 128); ' + $driverGuard +
                    " is the driver's default.")
    }
    while ($true) {
        $answer = Read-Host "  G to use (Enter for $default)"
        if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
        $value = 0
        if ([int]::TryParse($answer.Trim(), [ref]$value) -and $value -ge 0 -and $value -le 1024) { return $value }
        Write-Host '  A number from 0 to 1024, please.'
    }
}

# A line in reports\notes.txt, with the step it is for.
function Save-Note([string]$Step, [string]$Text) {
    New-ReportFolder
    Add-Content -Path $notes -Value ((Get-Date -Format 'yyyy-MM-dd HH:mm') + '  step ' + $Step + ': ' + $Text) -Encoding UTF8
}

# One line of the user's, kept there.
function Add-Note([string]$Step, [string]$Question) {
    $text = Read-Host ('  ' + $Question + ' (Enter for nothing)')
    if ([string]::IsNullOrWhiteSpace($text)) { return }
    Save-Note $Step $text
}

# The newest report of a command with this label, as text ('' if none).
function Get-ReportText([string]$Command, [string]$Label) {
    if (-not (Test-Path $reports)) { return '' }
    $report = Get-ChildItem -Path $reports -Filter ($Command + '-' + $Label + '-*.txt') |
              Sort-Object LastWriteTime | Select-Object -Last 1
    if ($null -eq $report) { return '' }
    $text = Get-Content -Path $report.FullName -Raw
    if ($null -eq $text) { return '' }
    return $text
}

# What the newest info report with this label says the card is left with:
# its task mode and the audio systems still running. Said in one line, kept
# in notes.txt, and returned (assign it, or it is printed twice).
function Write-CardState([string]$Label, [string]$Step) {
    $text = Get-ReportText 'info' $Label
    if (-not $text) { return '' }
    $mode = 'unknown'
    $match = [regex]::Match($text, '(?m)^task mode\s*=\s*(\S+)')
    if ($match.Success) { $mode = $match.Groups[1].Value }
    $pattern = '(?ms)^\[audio system (\d+)\]\s*$.*?^input running\s*=\s*(\w+)\s*$.*?^output running\s*=\s*(\w+)\s*$'
    $running = @([regex]::Matches($text, $pattern) |
                 Where-Object { $_.Groups[2].Value -eq 'yes' -or $_.Groups[3].Value -eq 'yes' } |
                 ForEach-Object { $_.Groups[1].Value })
    $line = 'task mode ' + $mode
    if ($running.Count -gt 0) { $line += ', audio systems running: ' + ($running -join ' ') }
    else { $line += ', no audio system running' }
    Write-Host ('  The card now: ' + $line + '.') -ForegroundColor Green
    Save-Note $Step ('the card afterwards: ' + $line)
    return $line
}

# Said when nothing uses the card and it is still in OEM task mode.
function Write-OemLeft([string]$State, [string]$Who) {
    if ($State -notmatch 'OEM') { return }
    Write-Host ('  OEM task mode with ' + $Who + " closed: it put back the driver's OEM mode, or nothing put standard back.") -ForegroundColor Yellow
    Write-Host '  The steps after this one work the same, but the AJA Control Panel does nothing until the PC'
    Write-Host '  restarts (step 9 needs it). It is in the notes.'
}

# The driver hosts load, in one line (duplex-asiotest --installed): since
# 0.1.22 install-driver.cmd copies the driver to Program Files and registers
# that copy, so a new build is not what hosts run until step 6 has run
# again, and a deleted or rebuilt folder no longer leaves hosts with a name
# they cannot open. Said, kept in notes.txt, and returned: $false only when
# hosts would not load this folder's build ($true also when it cannot be
# told). Assign it, or it is printed.
function Write-Installed([string]$Step) {
    if (-not (Test-Path $asiotest)) { return $true }
    $lines = @()
    $code = 0
    try {
        $lines = @(& $asiotest '--installed')
        $code = $LASTEXITCODE
    }
    catch {
        $lines = @()
    }
    if ($lines.Count -eq 0) { return $true }
    $line = ('' + $lines[0]).Trim()
    if ($line -notmatch '^installed\s') { return $true }   # not that program's answer
    $line = $line -replace '^installed\s+', ''
    if ($code -eq 0) {
        Write-Host ('  Installed driver: ' + $line) -ForegroundColor Green
    }
    else {
        Write-Host ('  Installed driver: ' + $line) -ForegroundColor Yellow
    }
    Save-Note $Step ('installed driver: ' + $line)
    return ($code -eq 0)
}

# Before a step in which a host loads the driver: it loads the installed
# copy, so say so when that is not this build.
function Test-HostLoadsThisBuild([string]$Step) {
    if (Write-Installed $Step) { return }
    Write-Host '  A host would not be running this build. To install it: Ctrl+C here, then card-session 6' -ForegroundColor Yellow
    Write-Host '  (every host closed), and this step again.' -ForegroundColor Yellow
}

# Step 6: install-driver.cmd as administrator (Windows asks), then what is
# registered, as hosts will list it, and the copy they load.
function Install-Driver {
    $installer = Join-Path $here 'install-driver.cmd'
    if (-not (Test-Path $installer)) {
        Write-Host '  install-driver.cmd is not beside this script.' -ForegroundColor Red
        return
    }
    try {
        Start-Process -FilePath $installer -Verb RunAs -Wait
    }
    catch {
        Write-Host ('  It did not run: ' + $_.Exception.Message) -ForegroundColor Yellow
        Write-Host '  Right-click install-driver.cmd, Run as administrator, and go on with the next step.'
    }
    $names = @()
    try {
        $names = @(Get-ChildItem -Path 'HKLM:\SOFTWARE\ASIO' -ErrorAction Stop |
                   ForEach-Object { $_.PSChildName } | Where-Object { $_ -like 'AJA Duplex*' })
    }
    catch { }
    if ($names.Count -eq 0) {
        Write-Host '  No AJA Duplex driver is registered.' -ForegroundColor Yellow
    }
    elseif ($names.Count -eq 1 -and $names[0] -eq 'AJA Duplex ASIO') {
        Write-Host '  Registered, as hosts will list it: AJA Duplex ASIO.' -ForegroundColor Green
    }
    else {
        # Since 0.1.20 the driver is the only one: installing takes away
        # the simulated driver that earlier versions registered.
        Write-Host ('  Registered: ' + ($names -join ', ') + '.') -ForegroundColor Yellow
        Write-Host '  Only AJA Duplex ASIO was expected: it is in the notes.' -ForegroundColor Yellow
    }
    Save-Note '6' ('registered: ' + ($names -join ', '))
    if (-not (Write-Installed '6')) {
        Write-Host "  The installer's window said why. If a host had the driver open: close it, and step 6 again" -ForegroundColor Yellow
        Write-Host '  (card-session 6).' -ForegroundColor Yellow
    }
}

# A program started and, so many seconds later, ended by force, as a host
# that crashes ends. False if it did not start or had ended by itself.
function Stop-ByForce([string]$FilePath, [string[]]$Arguments, [string]$Name, [int]$After) {
    Write-Host ('  > ' + $Name + ' ' + ($Arguments -join ' ') + '   (ended by force after ' + $After + ' s)') -ForegroundColor DarkGray
    $process = $null
    try {
        $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -WorkingDirectory $here -NoNewWindow -PassThru
    }
    catch {
        Write-Host ('  ' + $Name + ' did not start: ' + $_.Exception.Message) -ForegroundColor Red
        return $false
    }
    Start-Sleep -Seconds $After
    if ($process.HasExited) {
        Write-Host ('  ' + $Name + ' had ended by itself (see above): nothing was ended by force.') -ForegroundColor Yellow
        return $false
    }
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    $null = $process.WaitForExit(5000)
    return $true
}

# True when a line of Write-CardState says the card is as nothing left it:
# standard task mode, and audio system 2 (the KONA X's last, which the probe
# and the driver take) not running. Audio system 1 may run: AJA's own ASIO
# driver leaves it so.
function Test-CardLeftClean([string]$State) {
    if ($State -notlike '*NTV2_STANDARD_TASKS*') { return $false }
    if ($State -match 'running:.*\b2\b') { return $false }
    return $true
}

# Audio system 2's own settings in the newest info report with this label,
# by name ('capture enable' is 'no', and so on): what a takeover puts back
# as the program that was ended had found them (since 1.0.0). Empty if
# there is no such report.
$ownSettingNames = @('capture enable', 'channels', 'rate', 'buffer size', 'loopback', 'input source',
                     'embedded audio clock', 'output erase mode', 'input delay', 'output delay')

function Get-OwnSettings([string]$Label) {
    $found = @{}
    $text = Get-ReportText 'info' $Label
    if (-not $text) { return $found }
    $section = [regex]::Match($text, '(?ms)^\[audio system 2\]\s*$(.*?)(?:^\[|\z)')
    if (-not $section.Success) { return $found }
    foreach ($name in $ownSettingNames) {
        $match = [regex]::Match($section.Groups[1].Value, '(?m)^' + [regex]::Escape($name) + '\s*=\s*(.*?)\s*$')
        if ($match.Success) { $found[$name] = $match.Groups[1].Value }
    }
    return $found
}

# Says, and keeps in notes.txt, whether audio system 2's own settings are
# after a takeover as they were before the forced close.
function Write-OwnSettings([string]$Before, [string]$After, [string]$Step) {
    $was = Get-OwnSettings $Before
    $now = Get-OwnSettings $After
    if ($was.Count -eq 0 -or $now.Count -eq 0) { return }
    $changed = @()
    foreach ($name in $ownSettingNames) {
        if ($was.ContainsKey($name) -and $now.ContainsKey($name) -and $was[$name] -ne $now[$name]) {
            $changed += ($name + ' ' + $now[$name] + ' (it was ' + $was[$name] + ')')
        }
    }
    if ($changed.Count -eq 0) {
        Write-Host "  Audio system 2's own settings are as they were before the forced close." -ForegroundColor Green
        Save-Note $Step "audio system 2's own settings: as before the forced close"
    }
    else {
        Write-Host ("  Audio system 2's own settings are NOT as before the forced close: " + ($changed -join '; ') + '.') -ForegroundColor Yellow
        Save-Note $Step ("audio system 2's own settings NOT as before the forced close: " + ($changed -join '; '))
    }
}

# True when audio system 2 has capture enable and erase mode on in the
# newest info report with this label. With nothing using the card that is
# what a takeover of 0.1.21 or 0.1.22 left, and it stays until the PC
# restarts: found so, it is what this version puts back, and the step
# cannot tell whether a takeover puts the two back.
function Test-LeftByAnEarlierTakeover([string]$Label) {
    $found = Get-OwnSettings $Label
    return ($found['capture enable'] -eq 'yes' -and $found['output erase mode'] -eq 'yes')
}

# Step 10: a program ended by force while it plays, as a host that crashes
# ends, and whether the next one takes the card over by itself (since
# 0.1.21) and leaves audio system 2's own settings as the ended program had
# found them (since 1.0.0). Twice: duplex-probe in the middle of a loop,
# then the driver itself in duplex-asiotest. Only if the card is not left
# clean at the end does the PC need a restart.
function Invoke-ForcedClose {
    Write-Host '  The card before it:'
    Invoke-Probe @('info', '--label', 'before-kill')
    $null = Write-CardState 'before-kill' '10'
    if (Test-LeftByAnEarlierTakeover 'before-kill') {
        Write-Host '  Audio system 2 has capture enable and erase mode on with nothing using it: a takeover of' -ForegroundColor Yellow
        Write-Host '  0.1.21 or 0.1.22 left them so, and they stay until the PC restarts. This step still tells' -ForegroundColor Yellow
        Write-Host '  whether the takeover works; whether it puts those two back shows only after a restart.' -ForegroundColor Yellow
        Save-Note '10' 'audio system 2 had capture enable and erase mode on before the forced close (left by an earlier takeover)'
        $answer = Read-Host '  Enter: go on anyway   S: leave this step for after a restart (card-session 10)'
        if (("$answer").Trim().ToUpper() -eq 'S') { return }
    }
    $arguments = @('loop', '--frames', '128', '--guard', '64', '--seconds', '60', '--label', 'killed', '--out', 'reports')
    if (-not (Stop-ByForce $probe $arguments 'duplex-probe' 10)) { return }
    $script:restartNeeded = $true
    Write-Host ''
    Write-Host '  duplex-probe was ended by force in the middle of the loop. The card now:'
    Save-Note '10' 'duplex-probe ended by force 10 s into a loop'
    Invoke-Probe @('info', '--label', 'after-kill')
    $null = Write-CardState 'after-kill' '10'
    Write-Host '  And a loop, as the next program to open the card. It takes the card over ("taken over" in'
    Write-Host '  its first lines) and runs; up to 0.1.20 "in use" was the answer.'
    Invoke-Probe @('loop', '--frames', '128', '--guard', '64', '--seconds', '5', '--label', 'takeover')
    $took = [bool]((Get-ReportText 'loop' 'takeover') -match 'taken over')
    Save-Note '10' ('the loop after it: ' + $(if ($took) { 'took the card over and ran' } else { 'did NOT take the card over (see the window)' }))
    Invoke-Probe @('info', '--label', 'after-takeover')
    $state = Write-CardState 'after-takeover' '10'
    Write-OwnSettings 'before-kill' 'after-takeover' '10'
    $clean = $took -and (Test-CardLeftClean $state)

    if ($clean -and (Test-Path $asiotest)) {
        Write-Host ''
        Write-Host '  The same with the driver: duplex-asiotest ended by force while the driver plays.'
        if (Stop-ByForce $asiotest @('--card', '--seconds', '60') 'duplex-asiotest' 6) {
            Save-Note '10' 'duplex-asiotest ended by force 6 s in, the driver playing'
            Invoke-Probe @('info', '--label', 'after-driver-kill')
            $null = Write-CardState 'after-driver-kill' '10'
            Write-Host '  And the driver again, as the host started anew ("start: ASE_OK" is the takeover working;'
            Write-Host '  the round trip needs XLR out L cabled to XLR in L, as in step 7):'
            $null = Invoke-AsioTest 'asiotest-takeover.txt'
            $text = ''
            $path = Join-Path $reports 'asiotest-takeover.txt'
            if (Test-Path $path) { $text = Get-Content -Path $path -Raw }
            $started = [bool]($text -match 'start: ASE_OK')
            Save-Note '10' ('the driver after it: ' + $(if ($started) { 'opened the card and started' } else { 'did NOT start (see the window)' }))
            Invoke-Probe @('info', '--label', 'after-driver-takeover')
            $state = Write-CardState 'after-driver-takeover' '10'
            Write-OwnSettings 'before-kill' 'after-driver-takeover' '10'
            $clean = $started -and (Test-CardLeftClean $state)
        }
    }

    if ($clean) {
        $script:restartNeeded = $false
        $script:leftClean = $true
        Write-Host '  The card is as nothing left it: no restart needed.' -ForegroundColor Green
        Save-Note '10' 'the card left clean: no restart needed'
    }
    else {
        Save-Note '10' 'the card NOT left clean: the PC needs a restart'
    }
}

# The output latency the driver reports from: the OutputLatency saved in
# its settings, or with none saved the card's own for the driver's input,
# as DriverSettings.h has it (208 frames with the analog input, 130 with
# AES, 0 with the embedded ones). The round trip of step 7 is added to it.
function Get-OutputLatency {
    $value = $null
    try { $value = (Get-ItemProperty -Path $settingsKey -Name OutputLatency -ErrorAction Stop).OutputLatency } catch { }
    if ($null -ne $value) { return [int]$value }
    $choice = $null
    try { $choice = (Get-ItemProperty -Path $settingsKey -Name Input -ErrorAction Stop).Input } catch { }
    $number = 0
    if ($null -ne $choice) { $number = [int]$choice }
    if ($number -eq 1) { return 130 }
    if ($number -eq 2 -or $number -eq 3) { return 0 }
    return 208   # analog, which is also what the driver takes a value out of range for
}

function Set-OutputLatency([int]$Frames) {
    if (-not (Test-Path $settingsKey)) { New-Item -Path $settingsKey -Force | Out-Null }
    New-ItemProperty -Path $settingsKey -Name OutputLatency -PropertyType DWord -Value $Frames -Force | Out-Null
}

# duplex-asiotest, its output kept in reports\<Name>; returns how many frames
# the round trip is from the latencies reported, or $null if none was measured.
function Invoke-AsioTest([string]$Name) {
    $arguments = @('--seconds', '10')
    if (-not $Simulate) { $arguments = @('--card') + $arguments }
    Write-Host ('  > duplex-asiotest ' + ($arguments -join ' ') + '   (about 15 s: a click on output 1 once a second)') -ForegroundColor DarkGray
    $lines = @(& $asiotest @arguments)
    New-ReportFolder
    Set-Content -Path (Join-Path $reports $Name) -Value $lines -Encoding UTF8
    foreach ($line in $lines) { Write-Host ('  ' + $line) }
    $match = [regex]::Match(($lines -join "`n"), '\((-?\d+) frames apart\)')
    if ($match.Success) { return [int]$match.Groups[1].Value }
    return $null
}

# From one start to the next the round trip moves by a few frames, with
# the offset between the card's two heads, which step 8 frames at a time:
# 718 to 727 frames in six starts on the KONA X (2026-11-11), with 720
# reported. Within two steps of the heads there is nothing to set: a run
# that comes out 5 frames long says nothing about the next one.
$startSpread = 16

function Invoke-RoundTrip {
    if (-not (Test-Path $asiotest)) {
        Write-Host '  duplex-asiotest.exe is not beside this script.' -ForegroundColor Red
        return
    }
    $apart = Invoke-AsioTest 'asiotest-1.txt'
    if ($null -eq $apart) {
        Write-Host '  No round trip measured: see above. Resolume closed? XLR out L to XLR in L?' -ForegroundColor Yellow
        return
    }
    $current = Get-OutputLatency
    $wanted = $current + $apart
    if ($wanted -lt 0) { $wanted = 0 }
    if ($wanted -gt 48000) { $wanted = 48000 }
    $outcome = 'stays ' + $current
    if ($wanted -eq $current) {
        Write-Host "  The round trip is what the driver reports: OutputLatency stays $current frames."
    }
    elseif ([Math]::Abs($apart) -le $startSpread) {
        Write-Host "  The round trip is $apart frames from what the driver reports, and it moves by up to $startSpread from one"
        Write-Host "  start to the next (the card's heads step 8 frames at a time): nothing to set."
        Write-Host "  OutputLatency stays $current frames."
    }
    elseif ($Simulate) {
        Write-Host "  OutputLatency would go from $current to $wanted frames; with the simulated card it is left as it is."
    }
    else {
        Write-Host "  The round trip is $apart frames from the latencies reported: OutputLatency from $current to $wanted frames."
        $answer = Read-Host '  Enter: set it   N: leave it'
        if (("$answer").Trim().ToUpper() -ne 'N') {
            Set-OutputLatency $wanted
            Write-Host "  OutputLatency is $wanted frames now."
            $outcome = 'set to ' + $wanted + ' (it was ' + $current + ')'
        }
    }
    Save-Note '7' ('the round trip: ' + $apart + ' frames from the latencies reported; OutputLatency ' + $outcome)
    Write-Host '  Once more, to check:'
    $again = Invoke-AsioTest 'asiotest-2.txt'
    if ($null -ne $again) { Save-Note '7' ('once more: ' + $again + ' frames from the latencies reported') }
}

function Save-Session {
    Add-Note 'collect' 'Anything seen or heard that the files will not say?'
    $stamp = Get-Date -Format 'yyyyMMdd-HHmm'
    $zip = Join-Path $here ('session-' + $stamp + '.zip')
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ('aja-duplex-session-' + $stamp)
    try {
        if (Test-Path $stage) { Remove-Item -Path $stage -Recurse -Force }
        New-Item -ItemType Directory -Path $stage | Out-Null
        if (Test-Path $reports) { Copy-Item -Path $reports -Destination $stage -Recurse }
        foreach ($name in @('driver.log', 'driver.log.old', 'simulated.log', 'simulated.log.old')) {
            $file = Join-Path $logFolder $name
            if (Test-Path $file) { Copy-Item -Path $file -Destination $stage }
        }
        if (-not (Get-ChildItem -Path $stage)) {
            Write-Host '  Nothing to collect yet.'
            return
        }
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
        Write-Host ('  The session, in one file: ' + $zip) -ForegroundColor Green
    }
    catch {
        Write-Host ('  The ZIP could not be made: ' + $_.Exception.Message) -ForegroundColor Red
        Write-Host '  Close Resolume (it may hold driver.log) and run: card-session collect'
    }
    finally {
        if (Test-Path $stage) { Remove-Item -Path $stage -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Show-Step($Step) {
    Write-Host ''
    Write-Host ('=== Step ' + $Step.Id + ': ' + $Step.Title + ' ===') -ForegroundColor Cyan
    $needs = @($Step.Cables | Where-Object { $_ })
    if ($needs.Count -eq 0) { Write-Host '  Cables: none needed (any already in can stay).' }
    foreach ($need in $needs) {
        if ($script:connected[$need]) { Write-Host ('  Cables: ' + $cableText[$need] + ' (in already).') }
        else { Write-Host ('  CONNECT NOW: ' + $cableText[$need] + '.') -ForegroundColor Yellow }
    }
    foreach ($line in @($Step.Setup)) { Write-Host ('  ' + $line) }
    while ($true) {
        $answer = Read-Host '  Enter: run it   S: skip it   Q: stop here'
        switch (("$answer").Trim().ToUpper()) {
            '' {
                foreach ($need in $needs) { $script:connected[$need] = $true }
                return 'run'
            }
            'S' { return 'skip' }
            'Q' { return 'quit' }
        }
    }
}

# ---------------------------------------------------------------------------
# The steps, as CARD_SESSION.md lists them. Card: needs the real card,
# skipped with -Simulate.
# ---------------------------------------------------------------------------
$steps = @(
    @{ Id = 'list'; Title = 'the AJA devices'; Card = $true
       Setup = @('The KONA X must be in the list, index 0.')
       Run = { Invoke-Probe @('list') -Bare } },

    @{ Id = '1a'; Title = 'info, nothing using the card'; Card = $true
       Setup = @("Resolume, the AJA Control Panel and AJA's programs closed. Read-only, half a second.")
       Run = { Invoke-Probe @('info', '--label', 'idle') } },

    @{ Id = '1b'; Title = "info, with AJA's own drivers playing"; Card = $true
       Setup = @("A program recording through AJA's own ASIO driver, and Windows sound playing through",
                 'the AJA WDM device, both at once (S if that cannot be set up now).')
       Run = { Invoke-Probe @('info', '--label', 'aja-drivers') } },

    @{ Id = '1c'; Title = 'info, with AJA Input capturing'; Card = $true
       Setup = @('Resolume with AJA Input capturing the formats of a show;',
                 "Resolume's audio device anything but AJA Duplex ASIO.")
       Run = { Invoke-Probe @('info', '--label', 'plugin') } },

    @{ Id = '1d'; Title = 'info, with Low Latency Mode'; Card = $true
       Setup = @("As 1c, with AJA Input's Low Latency Mode on. Only if shows use it (S otherwise).")
       Run = { Invoke-Probe @('info', '--label', 'plugin-lowlatency') } },

    @{ Id = '2'; Title = 'heads and cost'
       Setup = @($probeNote, 'About 40 seconds.')
       Run = {
           Invoke-Probe @('heads', '--label', 'idle')
           Invoke-Probe @('cost', '--label', 'idle')
       } },

    @{ Id = '3'; Title = 'the AES loop: sweep'; Cables = @('aes')
       Setup = @($probeNote, $loud, 'About 3 minutes: N 256/128/64/32 against G 64/32/16/8.')
       Run = {
           $arguments = @('loop', '--channels', '1-8', '--label', 'sweep')
           if ($Simulate) { $arguments += @('--seconds', '3') }
           Invoke-Probe $arguments
           $g = Get-SweepGuard 'sweep'
           if ($null -ne $g) {
               Write-Host ('  Smallest clean G at N = 128: ' + $g + " (the long runs take the driver's own, " +
                           $driverGuard + ').') -ForegroundColor Green
           }
       } },

    @{ Id = '3-exit'; Title = "the AES loop: one hour, phase 0's exit test"; Cables = @('aes')
       Setup = @($probeNote, $loud, 'One hour at N = 128; PASSED at the end.')
       Run = {
           $g = Read-Guard 'sweep'
           $s = Get-Seconds 3600 30
           Write-Ends $s
           Invoke-Probe @('loop', '--frames', '128', '--guard', "$g", '--seconds', "$s", '--channels', '1-8', '--label', 'exit')
       } },

    @{ Id = '4a'; Title = 'the analog loop: sweep'; Cables = @('xlr')
       Setup = @($probeNote, $loud, "About 3 minutes, through the Xpand's converters. The BNC cables can stay.")
       Run = {
           $arguments = @('loop', '--path', 'analog', '--label', 'analog-sweep')
           if ($Simulate) { $arguments += @('--seconds', '3') }
           Invoke-Probe $arguments
           $g = Get-SweepGuard 'analog-sweep'
           if ($null -ne $g) {
               Write-Host ('  Smallest clean G at N = 128: ' + $g + " (the long runs take the driver's own, " +
                           $driverGuard + ').') -ForegroundColor Green
           }
       } },

    @{ Id = '4a-exit'; Title = 'the analog loop: one hour'; Cables = @('xlr')
       Setup = @($probeNote, $loud, 'One hour at N = 128; PASSED at the end.')
       Run = {
           $g = Read-Guard 'analog-sweep'
           $s = Get-Seconds 3600 30
           Write-Ends $s
           Invoke-Probe @('loop', '--path', 'analog', '--frames', '128', '--guard', "$g", '--seconds', "$s", '--label', 'analog-exit')
       } },

    @{ Id = '4b'; Title = 'routing'; Cables = @('aes', 'xlr')
       Setup = @($probeNote, $loud, '8 seconds, a tone on each output.')
       Run = { Invoke-Probe @('routing', '--scan-select', '--label', 'routing') } },

    @{ Id = '5a'; Title = "watch, AJA's services left alone"
       Setup = @('In the AJA Control Panel, the card in standard task mode (Standard Tasks).',
                 'Then close the AJA Control Panel, Resolume and every program that uses the card,',
                 "and touch nothing for 5 minutes (Q9: do AJA's services rewrite the audio?).")
       Run = { Invoke-Probe @('watch', '--seconds', "$(Get-Seconds 300 10)", '--label', 'services') } },

    @{ Id = '5b'; Title = 'watch, while the AJA Control Panel changes things'
       Setup = @('Open the AJA Control Panel. While this runs (2 minutes), change its audio settings:',
                 "the AES outputs' source, the analog/AES input, the task mode.")
       Run = { Invoke-Probe @('watch', '--seconds', "$(Get-Seconds 120 10)", '--label', 'panel') } },

    @{ Id = '5c'; Title = 'the AES loop with video (Q3)'; Cables = @('aes')
       Setup = @('Resolume with AJA Input capturing as in a show (as in 1c), every format and source a show',
                 "uses at once; Resolume's audio device anything but AJA Duplex ASIO.", $loud,
                 "Ten minutes at the driver's guard. The report's max DMA says how long audio waited for video.")
       Run = {
           $g = Read-Guard 'sweep'
           $s = Get-Seconds 600 20
           Write-Ends $s
           Invoke-Probe @('loop', '--frames', '128', '--guard', "$g", '--seconds', "$s", '--channels', '1-8', '--label', 'plugin')
       } },

    @{ Id = '6'; Title = 'install the driver'; Card = $true
       Setup = @('Every host closed (Resolume, vMix). install-driver.cmd runs as administrator (Windows asks):',
                 'it copies the driver to Program Files\AJA Duplex ASIO and registers it there, and a key of',
                 'its window closes it. Hosts load that copy: this folder can be moved, replaced or deleted',
                 'afterwards, and a new build is what hosts run only once this step has run again.',
                 'S only if the installed driver is this build already (said when the session started).')
       Run = { Install-Driver } },

    @{ Id = '6-vmix'; Title = 'vMix'; Card = $true
       Setup = @('S if vMix is not on this PC. In vMix, "AJA Duplex ASIO" must be in Add Input, Audio Input',
                 '(it was missing there while the simulated driver was registered beside it); pick the same',
                 'driver in Settings, Audio Outputs, and play and record something through it. Then close vMix.')
       Run = {
           Test-HostLoadsThisBuild '6-vmix'
           Read-Host '  When vMix is closed again, press Enter' | Out-Null
           Add-Note '6-vmix' 'Was AJA Duplex ASIO among the audio inputs? Did it play and record?'
           Invoke-Probe @('info', '--label', 'after-vmix')
           $state = Write-CardState 'after-vmix' '6-vmix'
           Write-OemLeft $state 'vMix'
       } },

    @{ Id = '7'; Title = "the driver's round trip"; Cables = @('xlr')
       Setup = @('The driver installed (step 6), its input the analog one.',
                 'Resolume closed: the driver takes one program at a time.', $loud,
                 "The driver reports the card's own latency with nothing set (208 frames with the analog",
                 'input, 130 with AES). This measures the round trip on XLR out L to XLR in L, which',
                 'should be what the driver reports, give or take the few frames it moves from one',
                 'start to the next; only if it is further off does it offer to set OutputLatency',
                 'from it. Then it measures again.')
       Run = { Invoke-RoundTrip } },

    @{ Id = '8'; Title = 'Resolume, as long as it can run'; Card = $true
       Setup = @('Resolume with AJA Duplex ASIO as its audio device, at the defaults (buffer 128, guard 256),',
                 'and AJA Input capturing as in a show. Something on XLR in L/R shows in its input;',
                 'a clip comes out of AES 1-2 and the analog output. Then as long as it can run: the target',
                 "is 8 hours without a dropout. The driver's settings (ASIO settings in Resolume) say how",
                 'many late outputs there have been so far.')
       Run = {
           Test-HostLoadsThisBuild '8'
           Read-Host '  When it plays and records, press Enter' | Out-Null
           Write-Host '  For Q19, with the sound playing: take every AJA Input source out of the composition,'
           Write-Host '  put one back, and change deck. Listen for a gap in the sound at each.'
           Read-Host '  When that is tried, press Enter' | Out-Null
           Add-Note '8' 'Q19: did the sound stop when the AJA Input sources went, came back, or the deck changed?'
           Write-Host '  Now for as long as it can run.'
           Read-Host '  When it is done, close Resolume and press Enter' | Out-Null
           Add-Note '8' 'How did it go (how long, any dropout heard)?'
           Invoke-Probe @('info', '--label', 'after-resolume')
           $state = Write-CardState 'after-resolume' '8'
           Write-OemLeft $state 'Resolume'
       } },

    @{ Id = '9'; Title = 'the SDI outputs (Q17)'; Card = $true; Cables = @('sdi')
       Setup = @('Only with an SDI cable (S otherwise). In the AJA Control Panel: SDI 1 an input, SDI 2',
                 "an output. In the driver's settings: input SDI 1, and outputs also to SDI 2.",
                 'In Resolume, outputs 1-16 should come back on inputs 1-16, with no video played on SDI 2.')
       Run = {
           Read-Host '  When it is tried, press Enter' | Out-Null
           Add-Note '9' 'Did outputs 1-16 come back on inputs 1-16?'
       } },

    @{ Id = '10'; Title = 'a forced close (Q18)'; Card = $true
       Setup = @('A program ended by force while it plays, as a host that crashes ends, and the next one',
                 'taking the card over by itself: first duplex-probe in a loop, then the driver in',
                 'duplex-asiotest (XLR out L to XLR in L, as in step 7). The card should end as it was,',
                 "audio system 2's own settings with it; if the card is not left clean, the PC needs a",
                 'restart, so this is the last test. About a minute.',
                 $probeNote, $loud)
       Run = { Invoke-ForcedClose } },

    @{ Id = '10-restart'; Title = 'the card after the restart (Q18)'; Card = $true
       Setup = @('Only if step 10 said to restart the PC, and after that restart (S otherwise): what the',
                 'restart cleared.')
       Run = {
           Invoke-Probe @('info', '--label', 'after-restart')
           $null = Write-CardState 'after-restart' '10-restart'
       } },

    @{ Id = 'collect'; Title = 'the ZIP of the session'
       Setup = @("The reports and the driver's log, in one ZIP beside this script.")
       Run = { Save-Session } }
)
$ids = @($steps | ForEach-Object { $_.Id })

# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------
Write-Host 'AJA Duplex ASIO: the session with the card, step by step' -ForegroundColor Cyan
Write-Host ('  The steps: ' + ($ids -join ' '))
Write-Host '  Enter runs a step, S skips it, Q stops; card-session <step> goes on from a step later.'
if ($From -eq 'help') {
    Write-Host '  card-session collect: only the ZIP; card-session -Simulate: a short try on the simulated card.'
    Wait-Close
    exit 0
}
if (-not (Test-Path $probe)) {
    Write-Host ''
    Write-Host "duplex-probe.exe is not beside this script: run it from the folder of the ZIP that build.cmd makes" -ForegroundColor Red
    Write-Host '(dist\AJA-Duplex-ASIO-<version>-win64), unzipped on the PC with the card.' -ForegroundColor Red
    Wait-Close
    exit 1
}
Write-Host ('  Reports: ' + $reports)
Write-Host ('  ' + $loud)
if ($Simulate) { Write-Host '  Simulated card: short runs, and the steps that need the card are skipped.' -ForegroundColor Yellow }
else {
    # What hosts load is the copy step 6 installs, not this folder's DLL.
    if (-not (Write-Installed 'start')) {
        Write-Host '  Step 6 installs this build (card-session 6, every host closed).' -ForegroundColor Yellow
    }
}

if (-not $From) {
    # Started with a double-click: where to start is asked instead.
    Write-Host ''
    $From = ("" + (Read-Host '  Step to start from (Enter for the first)')).Trim()
}
$start = 0
if ($From) {
    $start = [array]::IndexOf($ids, $From.ToLower())
    if ($start -lt 0) {
        Write-Host ''
        Write-Host ("There is no step '" + $From + "'. The steps: " + ($ids -join ' ')) -ForegroundColor Red
        Wait-Close
        exit 2
    }
}

Push-Location $here
try {
    for ($i = $start; $i -lt $steps.Count; $i++) {
        $step = $steps[$i]
        if ($Simulate -and $step.Card) {
            Write-Host ''
            Write-Host ('Step ' + $step.Id + ' (' + $step.Title + '): needs the card, skipped.') -ForegroundColor DarkGray
            continue
        }
        if ($step.Id -eq '10-restart' -and $script:leftClean) { continue }   # step 10 needed no restart
        $choice = Show-Step $step
        if ($choice -eq 'quit') {
            Write-Host ''
            Write-Host ('Stopped before step ' + $step.Id + '. To go on from there: card-session ' + $step.Id)
            break
        }
        if ($choice -eq 'skip') { continue }
        & $step.Run
        if ($script:restartNeeded) {
            # Step 10 left the card as a crashed host would, and the
            # takeover did not clear it: nothing more can be measured
            # until the PC has restarted.
            Write-Host ''
            Write-Host 'Restart the PC now. Then, to see what the restart cleared and make the ZIP:' -ForegroundColor Yellow
            Write-Host '    card-session 10-restart' -ForegroundColor Yellow
            break
        }
    }
}
finally {
    Pop-Location
}
Wait-Close
