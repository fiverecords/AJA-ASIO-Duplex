<#
.SYNOPSIS
    Builds AJA-Duplex-ASIO on Windows: the driver, its test host,
    duplex-probe and the unit tests, and a ZIP of them ready to take to the
    PC with the KONA X.

.DESCRIPTION
    Double-click build.cmd, or run it from a command prompt. The first run
    also fetches the AJA SDK (libajantv2, tag ntv2_18_0_0) into the folder
    next to this one: with git if it is installed, otherwise as a ZIP from
    GitHub.

        <workspace>\AJA-ASIO-Duplex   (this folder, whatever its name)
        <workspace>\libajantv2        (fetched when missing)

    It needs Visual Studio 2022, or only its free Build Tools, with
    "Desktop development with C++", which brings the compiler and CMake.
    build.cmd -InstallTools installs the Build Tools with winget when they
    are missing.

    What it leaves:
        build\Release\AJADuplexASIO.dll, duplex-asiotest.exe,
            duplex-probe.exe and duplex-tests.exe
        dist\AJA-Duplex-ASIO-<version>-win64.zip  (all but the tests, the
            install scripts, card-session.cmd, CARD_SESSION.md, DRIVER.md,
            PROBE.md, licences)
        dist\AJA-Duplex-ASIO-<version>-win64\     (the same files, unpacked:
            on a PC that builds and has the card, card-session.cmd runs
            from there; what a session left in it, reports\ and its ZIPs,
            is kept from one build to the next)

    Hosts do not load what this builds: they load the copy that
    install-driver.cmd put in Program Files\AJA Duplex ASIO. The last lines
    say whether that copy is this build.

    Everything it prints also goes to build-log.txt in this folder: if a
    step fails, that file says why.

.PARAMETER Config
    Release (default), Debug or RelWithDebInfo.

.PARAMETER Clean
    Delete the build folder first (full rebuild).

.PARAMETER SkipTests
    Do not run the unit tests, nor the driver on the simulated card.

.PARAMETER NoPackage
    Do not make the ZIP in dist\.

.PARAMETER InstallTools
    When Visual Studio with C++ is not found, install the Visual Studio
    2022 Build Tools with the C++ workload using winget. Downloads a few GB
    and asks for administrator rights.

.PARAMETER Workspace
    Folder that holds (or will hold) libajantv2. Default: the folder that
    contains this one.

.PARAMETER Generator
    CMake generator override, e.g. "Ninja". Default: the Visual Studio
    found.

.PARAMETER Setup
    Accepted for older instructions; the SDK is fetched anyway.

.EXAMPLE
    build.cmd
    Release build, unit tests and the ZIP.

.EXAMPLE
    build.cmd -Clean
    The same, from scratch.
#>
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Joaquin Villodre
# (Kept below the help block: Get-Help only reads a help block that is the
#  first thing in the file. Windows PowerShell 5.1 runs this, through
#  build.cmd, so nothing newer than 5.1 is used.)

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$NoPackage,
    [switch]$InstallTools,
    [string]$Workspace = '',
    [string]$Generator = '',
    [switch]$Setup
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
# Windows PowerShell's progress bar slows downloads to a crawl.
$ProgressPreference = 'SilentlyContinue'

# Pinned version, kept in step with CMakeLists.txt and the CI workflow.
$AjaSdkTag = 'ntv2_18_0_0'
$AjaSdkRepo = 'https://github.com/aja-video/libajantv2'

$SourceDir = $PSScriptRoot
$LogPath = Join-Path $SourceDir 'build-log.txt'

# ---------------------------------------------------------------------------
# Output: console and build-log.txt
# ---------------------------------------------------------------------------
$script:Log = $null
try {
    $script:Log = New-Object System.IO.StreamWriter($LogPath, $false, (New-Object System.Text.UTF8Encoding($false)))
    $script:Log.AutoFlush = $true
}
catch {
    Write-Warning "Cannot write $LogPath; going on without a log."
}

# MSBuild prints in UTF-8 when Windows is not in English, but PowerShell
# reads what programs print in the console's old code page unless told
# otherwise, and every accent came out garbled. Put back on exit, so a
# command prompt this ran from keeps its own.
$script:ConsoleEncoding = $null
try {
    $script:ConsoleEncoding = [Console]::OutputEncoding
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
}
catch {
    $script:ConsoleEncoding = $null
}

function Write-Log([string]$Text) {
    if ($script:Log) { $script:Log.WriteLine($Text) }
}

function Write-Line([string]$Text, [string]$Color = '') {
    if ($Color) { Write-Host $Text -ForegroundColor $Color } else { Write-Host $Text }
    Write-Log $Text
}

function Write-Step([string]$Text) {
    Write-Line ''
    Write-Line "==> $Text" 'Cyan'
}

# Started with a double-click: Explorer runs cmd.exe /c "...\build.cmd",
# which runs this. Then the window must stay open to be read.
function Test-StartedFromExplorer {
    if ($env:CI) { return $false }
    try {
        $me = Get-CimInstance Win32_Process -Filter "ProcessId = $PID" -ErrorAction Stop
        $cmd = Get-CimInstance Win32_Process -Filter "ProcessId = $($me.ParentProcessId)" -ErrorAction Stop
        if (-not $cmd -or $cmd.Name -ne 'cmd.exe') { return $false }
        if ("$($cmd.CommandLine)" -notmatch '/c\s.*build\.cmd') { return $false }
        $shell = Get-CimInstance Win32_Process -Filter "ProcessId = $($cmd.ParentProcessId)" -ErrorAction Stop
        return [bool]($shell -and $shell.Name -eq 'explorer.exe')
    }
    catch {
        return $false
    }
}
$script:KeepWindowOpen = Test-StartedFromExplorer

function Close-Script([int]$Code) {
    if ($script:Log) {
        $script:Log.Dispose()
        $script:Log = $null
    }
    if ($script:ConsoleEncoding) {
        try { [Console]::OutputEncoding = $script:ConsoleEncoding } catch { }
        $script:ConsoleEncoding = $null
    }
    if ($script:KeepWindowOpen) {
        Write-Host ''
        Read-Host 'Press Enter to close this window' | Out-Null
    }
    exit $Code
}

function Fail([string]$Text) {
    Write-Line ''
    Write-Line "ERROR: $Text" 'Red'
    if ($script:Log) { Write-Host "Everything above is also in $LogPath" -ForegroundColor Yellow }
    Close-Script 1
}

# Anything unexpected (a cmdlet failing, say) ends here too, instead of the
# window closing with the error unread.
trap {
    Write-Line ''
    Write-Line "ERROR: $($_.Exception.Message)" 'Red'
    if ($_.InvocationInfo) { Write-Line $_.InvocationInfo.PositionMessage }
    if ($script:Log) { Write-Host "Everything above is also in $LogPath" -ForegroundColor Yellow }
    Close-Script 1
}

# Runs a native program; its output goes to the console and the log, and a
# non-zero exit code stops the script. Windows PowerShell 5.1 turns a
# program's stderr lines into error records: they are only text here.
function Invoke-Native([string]$What, [string]$Exe, [string[]]$Arguments) {
    Write-Log ">> `"$Exe`" $($Arguments -join ' ')"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = -1   # stays so if the program cannot even start
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            $text = "$_"
            Write-Host $text
            Write-Log $text
        }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) { Fail "$What failed (exit code $code)." }
}

# Runs a native program and returns its stdout, whatever its exit code.
function Get-NativeOutput([string]$Exe, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>$null
        return [pscustomobject]@{ Output = ($out -join "`n").Trim(); ExitCode = $LASTEXITCODE }
    }
    catch {
        return [pscustomobject]@{ Output = ''; ExitCode = -1 }
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

function Find-Program([string]$Name) {
    $found = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.Path }
    return $null
}

# ---------------------------------------------------------------------------
# Visual Studio
# ---------------------------------------------------------------------------
function Find-VisualStudio {
    $programFiles = ${env:ProgramFiles(x86)}
    if (-not $programFiles) { $programFiles = $env:ProgramFiles }
    if (-not $programFiles) { return $null }
    $vswhere = Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $common = @('-latest', '-products', '*',
                '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
    $path    = (Get-NativeOutput $vswhere ($common + @('-property', 'installationPath'))).Output
    $version = (Get-NativeOutput $vswhere ($common + @('-property', 'installationVersion'))).Output
    if (-not $path -or -not $version) { return $null }
    return [pscustomobject]@{ Path = $path; Major = [int]($version.Split('.')[0]) }
}

function Install-BuildTools {
    $winget = Find-Program 'winget'
    if (-not $winget) {
        Fail ("winget was not found (Windows 10/11 has it as 'App Installer' in the Microsoft Store). " +
              "Install the Build Tools by hand instead: https://visualstudio.microsoft.com/downloads/ , " +
              "'Build Tools for Visual Studio 2022', with 'Desktop development with C++'.")
    }
    Write-Step 'Installing the Visual Studio 2022 Build Tools with C++ (a few GB; it asks for administrator rights)'
    $override = '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools ' +
                '--add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended'
    Invoke-Native 'Installing the Build Tools' $winget @(
        'install', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--exact', '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements', '--override', $override)
}

# ---------------------------------------------------------------------------
# AJA SDK
# ---------------------------------------------------------------------------
function Get-AjaSdk([string]$Destination) {
    $git = Find-Program 'git'
    if ($git) {
        Write-Line "  cloning $AjaSdkRepo ($AjaSdkTag)"
        Invoke-Native 'Cloning libajantv2' $git @(
            'clone', '--depth', '1', '--branch', $AjaSdkTag, '--config', 'advice.detachedHead=false',
            "$AjaSdkRepo.git", $Destination)
        return
    }

    # No git: the tag's archive from GitHub, unpacked next to the destination
    # (a move across drives would fail).
    $url = "$AjaSdkRepo/archive/refs/tags/$AjaSdkTag.zip"
    $parent = Split-Path $Destination -Parent
    $zip = Join-Path $parent "libajantv2-$AjaSdkTag.zip"
    $unpack = Join-Path $parent "libajantv2-$AjaSdkTag.unpack"
    Write-Line "  git not found: downloading $url"
    $problem = ''
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        Write-Line '  unpacking'
        if (Test-Path $unpack) { Remove-Item -Recurse -Force $unpack }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $unpack)
        $top = Get-ChildItem -Path $unpack -Directory | Select-Object -First 1
        if (-not $top) {
            $problem = 'the archive is empty'
        }
        else {
            Move-Item -Path $top.FullName -Destination $Destination
            # No git history to ask later which tag this is.
            Set-Content -Path (Join-Path $Destination '.sdk-tag') -Value $AjaSdkTag -Encoding ASCII
        }
    }
    catch {
        $problem = $_.Exception.Message
    }
    Remove-Item -Force -ErrorAction SilentlyContinue $zip
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $unpack
    if ($problem) {
        Fail ("Downloading the AJA SDK failed: $problem. Install Git for Windows (git-scm.com) " +
              "and run build.cmd again, or clone $AjaSdkRepo (tag $AjaSdkTag) into $Destination.")
    }
}

function Get-AjaSdkTag([string]$Dir) {
    $marker = Join-Path $Dir '.sdk-tag'
    if (Test-Path $marker) { return (Get-Content $marker -TotalCount 1).Trim() }
    $git = Find-Program 'git'
    if ($git -and (Test-Path (Join-Path $Dir '.git'))) {
        return (Get-NativeOutput $git @('-C', $Dir, 'describe', '--tags', '--exact-match')).Output
    }
    return ''
}

# ---------------------------------------------------------------------------
# The ZIP for the PC with the card
# ---------------------------------------------------------------------------
function New-Package([string[]]$Programs, [string]$Version) {
    $distDir = Join-Path $SourceDir 'dist'
    $name = "AJA-Duplex-ASIO-$Version-win64"
    $folder = Join-Path $distDir $name
    $zip = Join-Path $distDir "$name.zip"
    # Staged apart, zipped, and then copied over the folder of that name:
    # on a PC that builds and has the card the session runs from that
    # folder, and what it left there (reports\, its ZIPs) is not the
    # build's to delete. Up to 0.1.21 the folder was made anew by each
    # build, with all that was in it.
    $stageRoot = Join-Path $distDir '.stage'
    $stage = Join-Path $stageRoot $name
    if (Test-Path $stageRoot) { Remove-Item -Recurse -Force $stageRoot }
    if (Test-Path $zip) { Remove-Item -Force $zip }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    foreach ($program in $Programs) { Copy-Item $program $stage }
    Copy-Item (Join-Path $SourceDir 'tools\install\install-driver.cmd') $stage
    Copy-Item (Join-Path $SourceDir 'tools\install\uninstall-driver.cmd') $stage
    Copy-Item (Join-Path $SourceDir 'tools\session\card-session.cmd') $stage
    Copy-Item (Join-Path $SourceDir 'tools\session\card-session.ps1') $stage
    Copy-Item (Join-Path $SourceDir 'docs\CARD_SESSION.md') $stage
    Copy-Item (Join-Path $SourceDir 'docs\DRIVER.md') $stage
    Copy-Item (Join-Path $SourceDir 'docs\PROBE.md') $stage
    Copy-Item (Join-Path $SourceDir 'LICENSE') (Join-Path $stage 'LICENSE.txt')
    Copy-Item (Join-Path $SourceDir 'THIRD_PARTY_NOTICES.md') $stage
    $built = Get-Date -Format 'yyyy-MM-dd HH:mm'
    $readme = @(
        "AJA Duplex ASIO $Version, for the PC with the KONA X and AJA's driver",
        'https://github.com/fiverecords/AJA-ASIO-Duplex',
        '',
        'Nothing else is needed: every program carries its own C++ runtime.',
        'Unzip this folder anywhere you can write to, for example',
        'C:\AJA-Duplex-ASIO: duplex-probe writes its reports beside it (so not',
        'under Program Files, which needs administrator rights).',
        '',
        'CARD_SESSION.md: the driver checked on the card, step by step, and',
        'card-session.cmd, which walks through it: before each step it says what to',
        'cable and set up, then runs it, and at the end it zips the reports and the',
        "driver's log.",
        '',
        'The driver (DRIVER.md):',
        '  install-driver.cmd     right-click, Run as administrator: it copies the',
        '                         driver to Program Files\AJA Duplex ASIO and',
        '                         registers it there, and hosts then list "AJA Duplex',
        '                         ASIO" (card-session runs it too). Hosts load that',
        '                         copy: this folder can be moved, replaced or deleted',
        '                         afterwards. Run it again, every host closed, to',
        '                         update the driver',
        '  uninstall-driver.cmd   the same, to take it away',
        '  duplex-asiotest        tries the driver as a host would; without options',
        '                         on a simulated card inside the DLL, which no host',
        '                         lists, so it needs no card at all',
        '  duplex-asiotest --installed',
        '                         which DLL hosts load, and whether it is this build',
        '',
        'The measurement tool of phase 0 (PROBE.md):',
        '  duplex-probe list      the AJA devices it sees',
        '  duplex-probe help      every command and option',
        '',
        'Open a command prompt or PowerShell in this folder for the programs',
        '(PowerShell needs .\ in front: .\duplex-asiotest).',
        '',
        "Built $built ($Config, AJA SDK $AjaSdkTag)."
    )
    Set-Content -Path (Join-Path $stage 'README.txt') -Value $readme -Encoding ASCII
    Compress-Archive -Path $stage -DestinationPath $zip
    try {
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
        Copy-Item -Path (Join-Path $stage '*') -Destination $folder -Force
    }
    catch {
        $why = $_.Exception.Message
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageRoot
        Fail ("The ZIP is made, but its files could not be copied to $folder" + ' (' + $why + '). ' +
              'Close what runs from that folder (a card-session window, duplex-probe, duplex-asiotest) ' +
              'and build again.')
    }
    Remove-Item -Recurse -Force $stageRoot
    return $zip
}

# ---------------------------------------------------------------------------
# Start
# ---------------------------------------------------------------------------
if ($env:OS -ne 'Windows_NT') {
    Fail 'This script builds the Windows version. Elsewhere use cmake directly (README.md).'
}

if (-not $Workspace) { $Workspace = Split-Path $SourceDir -Parent }
$Workspace = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Workspace)
$AjaDir   = Join-Path $Workspace 'libajantv2'
$BuildDir = Join-Path $SourceDir 'build'

$versionLine = Select-String -Path (Join-Path $SourceDir 'CMakeLists.txt') `
                             -Pattern '^\s*VERSION\s+([0-9.]+)' | Select-Object -First 1
$ProjectVersion = if ($versionLine) { $versionLine.Matches[0].Groups[1].Value } else { '0.0.0' }

Write-Line "AJA-Duplex-ASIO $ProjectVersion  ($Config)" 'White'
Write-Line "  source    : $SourceDir"
Write-Line "  workspace : $Workspace"
Write-Line "  log       : $LogPath"
if ($Setup) { Write-Line '  (-Setup is no longer needed: the SDK is fetched when missing)' }
if ($SourceDir.Length -gt 120) {
    Write-Line ("  warning: this folder's path is long ($($SourceDir.Length) characters). Visual Studio can fail " +
                'on paths over 260; if the build fails for that, move the folders closer to the drive root, ' +
                'e.g. C:\dev.') 'Yellow'
}

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
Write-Step 'Looking for Visual Studio and CMake'

$vs = Find-VisualStudio
if (-not $vs -and $InstallTools) {
    Install-BuildTools
    $vs = Find-VisualStudio
}
if (-not $vs) {
    Fail ("Visual Studio with C++ was not found. Either run build.cmd -InstallTools (installs the free " +
          "Visual Studio 2022 Build Tools with winget), or install Visual Studio 2022 (Community is free) " +
          "with 'Desktop development with C++'. If Visual Studio is there without C++: Visual Studio " +
          "Installer, Modify, tick 'Desktop development with C++'.")
}
Write-Line "  Visual Studio $($vs.Major): $($vs.Path)"

# Visual Studio's own CMake first: it always knows that Visual Studio's
# generator, which an older CMake on PATH may not.
$cmake = Join-Path $vs.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = Find-Program 'cmake' }
if (-not $cmake) {
    Fail ("CMake was not found, neither inside Visual Studio nor on PATH. Visual Studio Installer, Modify, " +
          "add 'C++ CMake tools for Windows' (or install CMake from cmake.org).")
}
$cmakeVersion = (Get-NativeOutput $cmake @('--version')).Output.Split("`n")[0]
Write-Line "  CMake: $cmake ($cmakeVersion)"

if (-not $Generator) {
    switch ($vs.Major) {
        16      { $Generator = 'Visual Studio 16 2019' }
        17      { $Generator = 'Visual Studio 17 2022' }
        18      { $Generator = 'Visual Studio 18 2026' }
        default { $Generator = 'Visual Studio 17 2022' }
    }
}
$IsVsGenerator = $Generator -like 'Visual Studio*'
$IsMultiConfig = $IsVsGenerator -or ($Generator -like '*Multi-Config')
Write-Line "  generator: $Generator"

# Single-config generators (Ninja, NMake) need the MSVC environment.
if (-not $IsVsGenerator -and -not (Find-Program 'cl.exe')) {
    $devShell = Join-Path $vs.Path 'Common7\Tools\Launch-VsDevShell.ps1'
    if (-not (Test-Path $devShell)) {
        Fail "Generator '$Generator' needs the MSVC environment and Launch-VsDevShell.ps1 was not found."
    }
    Write-Line '  entering the Visual Studio developer environment (x64)'
    $here = Get-Location
    & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    Set-Location $here
}

# ---------------------------------------------------------------------------
# AJA SDK
# ---------------------------------------------------------------------------
Write-Step 'Checking the AJA SDK (libajantv2)'

if (-not (Test-Path (Join-Path $AjaDir 'CMakeLists.txt'))) {
    if (Test-Path $AjaDir) {
        Fail "$AjaDir exists but does not look like libajantv2 (no CMakeLists.txt). Move it away and run again."
    }
    New-Item -ItemType Directory -Path $Workspace -Force | Out-Null
    Get-AjaSdk $AjaDir
}
$ajaTag = Get-AjaSdkTag $AjaDir
if ($ajaTag -and $ajaTag -ne $AjaSdkTag) {
    Write-Line "  warning: libajantv2 is at '$ajaTag'; this project is tested with '$AjaSdkTag'." 'Yellow'
}
Write-Line "  $AjaDir $(if ($ajaTag) { "($ajaTag)" })"

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step 'Removing the build folder'
    Remove-Item -Recurse -Force $BuildDir
}
elseif (Test-Path $cache) {
    # A build folder made with another generator cannot be reused.
    $prev = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
    if ($prev -and $prev.Matches[0].Groups[1].Value -ne $Generator) {
        Write-Step "The build folder was made with '$($prev.Matches[0].Groups[1].Value)'; recreating it"
        Remove-Item -Recurse -Force $BuildDir
    }
}

Write-Step 'Configuring'
$configureArgs = @('-S', $SourceDir, '-B', $BuildDir, '-G', $Generator, "-DAJANTV2_ROOT=$AjaDir")
if ($IsVsGenerator)      { $configureArgs += @('-A', 'x64') }
if (-not $IsMultiConfig) { $configureArgs += @("-DCMAKE_BUILD_TYPE=$Config") }
Invoke-Native 'Configuring' $cmake $configureArgs

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Write-Step "Building ($Config; the AJA SDK takes a few minutes the first time)"
$buildArgs = @('--build', $BuildDir, '--config', $Config, '--parallel')
if ($IsVsGenerator) { $buildArgs += @('--', '/nologo', '/verbosity:minimal') }
Invoke-Native 'The build' $cmake $buildArgs

$outDir = if ($IsMultiConfig) { Join-Path $BuildDir $Config } else { $BuildDir }
$probe    = Join-Path $outDir 'duplex-probe.exe'
$tests    = Join-Path $outDir 'duplex-tests.exe'
$driver   = Join-Path $outDir 'AJADuplexASIO.dll'
$asioTest = Join-Path $outDir 'duplex-asiotest.exe'
foreach ($file in @($probe, $driver, $asioTest)) {
    if (-not (Test-Path $file)) { Fail "The build finished but $file is not there." }
}

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if (-not $SkipTests) {
    Write-Step 'Running the unit tests'
    if (-not (Test-Path $tests)) { Fail "$tests is not there." }
    Invoke-Native 'The unit tests' $tests @()
    # The DLL itself, loaded the way a host loads it, on the simulated card:
    # no card and no registration needed.
    Write-Step 'Trying the driver on the simulated card'
    Invoke-Native 'The driver test' $asioTest @('--seconds', '2')
}

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------
$zipPath = ''
if (-not $NoPackage) {
    Write-Step 'Packing the driver and the probe for the PC with the card'
    $zipPath = New-Package @($driver, $asioTest, $probe) $ProjectVersion
    Write-Line "  $zipPath"
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Line ''
Write-Line ("OK  AJA Duplex ASIO $ProjectVersion ($Config)" +
            "$(if (-not $SkipTests) { ', unit tests and the driver test passed' })") 'Green'
foreach ($file in @($driver, $asioTest, $probe)) {
    $info = Get-Item $file
    Write-Line ("    {0}  {1:N0} bytes" -f $info.FullName, $info.Length)
}
if ($zipPath) {
    Write-Line "    $zipPath"
    Write-Line '    That ZIP is what goes to a PC with the KONA X: unzip it anywhere, then card-session.cmd,'
    Write-Line '    the session with the card, step by step. On this PC the same files are in the folder'
    Write-Line '    of that name beside the ZIP; what a session left there (reports\, its ZIPs) is kept.'
}
# Hosts load the copy that install-driver.cmd put in Program Files, not what
# was just built: whether that copy is this build.
$installed = Get-NativeOutput $asioTest @('--installed')
if ($installed.Output) {
    $installedLine = '    Installed driver: ' + ($installed.Output -replace '^installed\s+', '')
    if ($installed.ExitCode -eq 0) {
        Write-Line $installedLine
    }
    else {
        Write-Line $installedLine 'Yellow'
        Write-Line ("    To install this build, every host closed: install-driver.cmd in dist\AJA-Duplex-ASIO-$ProjectVersion-win64" +
                    ' (card-session step 6 runs it).') 'Yellow'
    }
}
Close-Script 0
