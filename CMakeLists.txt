# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Joaquin Villodre
# =============================================================================
#  AJA-Duplex-ASIO - full-duplex ASIO driver for AJA cards (KONA X + Xpand)
#
#  A user-mode ASIO driver built on AJA's open-source NTV2 SDK (libajantv2).
#  It reads and writes the card's audio buffers directly, so inputs and
#  outputs run in one driver, on the card's clock, with a selectable buffer
#  size. It does not replace AJA's driver: it uses it, like any SDK program.
#
#  This tree builds the driver (AJADuplexASIO.dll), its settings for hosts
#  with no settings button (AJADuplexASIOSettings.exe), a small host that
#  tries it (duplex-asiotest), the measurement tool duplex-probe, the
#  simulated card they can run against, and the unit tests. See README.md
#  and docs/ROADMAP.md.
#
#  Requirements:
#    - CMake 3.15 or newer
#    - C++17 toolchain: MSVC 2022 for the real target (Windows x64); GCC or
#      Clang build everything that is not Windows-specific, for development
#    - libajantv2 checked out next to this folder, at tag ntv2_18_0_0
# =============================================================================

cmake_minimum_required(VERSION 3.15)

project(AJADuplexASIO
    VERSION 1.0.1
    DESCRIPTION "Full-duplex ASIO driver for AJA cards"
    HOMEPAGE_URL "https://github.com/fiverecords/AJA-ASIO-Duplex"
    LANGUAGES CXX
)

# The date of this version (the one CHANGELOG.md gives it) and whose the
# project is: with the address above, what the driver's settings dialog
# shows (src/common/DuplexVersion.h.in).
set(AJADUPLEX_RELEASE_DATE "2026-11-18")
set(AJADUPLEX_AUTHOR "fiverecords")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for editors and clang tools (Ninja/Makefiles)

# -----------------------------------------------------------------------------
# MSVC runtime: static (/MT, /MTd) for every target, libajantv2 included.
# -----------------------------------------------------------------------------
# The driver DLL is loaded into other programs' processes and must not depend
# on a VC++ redistributable being installed. Set before add_subdirectory() so
# libajantv2 inherits it (policy CMP0091, NEW since CMake 3.15).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# -----------------------------------------------------------------------------
# libajantv2 (AJA NTV2 SDK, open-source distribution, MIT)
# -----------------------------------------------------------------------------
# Expected as a sibling folder named "libajantv2", at the tag the project is
# tested with (the same one AJA-Input-FFGL uses). Override with
# -DAJANTV2_ROOT=<path>. It is added as a subdirectory and linked statically,
# with everything we do not need switched off.
set(AJANTV2_ROOT "${CMAKE_SOURCE_DIR}/../libajantv2"
    CACHE PATH "Path to the libajantv2 (AJA NTV2 SDK) checkout")

if(NOT EXISTS "${AJANTV2_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "libajantv2 not found at '${AJANTV2_ROOT}'.\n"
        "Clone it next to this folder with:\n"
        "  git clone --branch ntv2_18_0_0 https://github.com/aja-video/libajantv2.git\n"
        "or pass -DAJANTV2_ROOT=<path> to cmake (on Windows, build.cmd fetches it).")
endif()

# FORCE so these win over the defaults of libajantv2's own option() calls.
# Plugin loading stays off: it would also pull mbedtls from the network.
set(AJANTV2_BUILD_OPENSOURCE     ON  CACHE BOOL "" FORCE)
set(AJANTV2_BUILD_SHARED         OFF CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_DEMOS        ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_DRIVER       ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_TESTS        ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_TOOLS        ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_PLUGIN_LOAD  ON  CACHE BOOL "" FORCE)
set(AJA_DISABLE_QT               ON  CACHE BOOL "" FORCE)

add_subdirectory(${AJANTV2_ROOT} ${CMAKE_BINARY_DIR}/aja-build EXCLUDE_FROM_ALL)

if(NOT TARGET ajantv2)
    message(FATAL_ERROR "libajantv2 did not define the 'ajantv2' target.")
endif()

# The SDK predates some modern warnings; silence them on its own target only.
if(MSVC)
    target_compile_options(ajantv2 PRIVATE
        /wd5033   # 'register' is no longer a supported storage class
        /wd4244   # conversion from 'X' to 'Y', possible loss of data
        /wd4267   # conversion from 'size_t' to 'X', possible loss of data
        /wd4996)  # use of deprecated function (libajantv2 internal)
endif()

# SDK headers are included as SYSTEM so they do not flood our warning level.
get_target_property(AJANTV2_INCLUDE_DIRS ajantv2 INTERFACE_INCLUDE_DIRECTORIES)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Common settings for our own targets
# -----------------------------------------------------------------------------
add_library(duplex_settings INTERFACE)
if(MSVC)
    target_compile_options(duplex_settings INTERFACE
        /W4 /permissive- /utf-8
        # Headers reached through SYSTEM include directories are "external"
        # and their warnings are dropped.
        /external:W0 /external:anglebrackets)
    target_compile_definitions(duplex_settings INTERFACE
        NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(duplex_settings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Version header generated from project(VERSION ...).
configure_file(src/common/DuplexVersion.h.in
               ${CMAKE_BINARY_DIR}/generated/DuplexVersion.h @ONLY)

# -----------------------------------------------------------------------------
# duplex_common: helpers with no SDK and no Windows dependency
# -----------------------------------------------------------------------------
add_library(duplex_common STATIC
    src/common/AlignedBuffer.h
    src/common/Clock.h
    src/common/DriverSettings.cpp
    src/common/DriverSettings.h
    src/common/Log.cpp
    src/common/Log.h
    src/common/Report.cpp
    src/common/Report.h
    src/common/Stats.cpp
    src/common/Stats.h
    src/common/TextFormat.cpp
    src/common/TextFormat.h
)
target_include_directories(duplex_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(duplex_common PUBLIC Threads::Threads PRIVATE duplex_settings)

# -----------------------------------------------------------------------------
# duplex_platform: thread priority, timers, Ctrl+C, and the marks that say
# which program has an audio system in hand (Windows; POSIX fallbacks)
# -----------------------------------------------------------------------------
add_library(duplex_platform STATIC
    src/platform/OwnerMark.cpp
    src/platform/OwnerMark.h
    src/platform/Realtime.cpp
    src/platform/Realtime.h
)
target_link_libraries(duplex_platform
    PUBLIC  duplex_common Threads::Threads
    PRIVATE duplex_settings)
if(WIN32)
    target_link_libraries(duplex_platform PUBLIC avrt advapi32)   # MMCSS; the registry, for the marks
endif()

# -----------------------------------------------------------------------------
# duplex_device: the only code that calls libajantv2
# -----------------------------------------------------------------------------
# CardAudio.h, the interface the engine and the probe use, has no SDK types.
add_library(duplex_device STATIC
    src/device/CardAudio.h
    src/device/CardInspector.cpp
    src/device/CardInspector.h
    src/device/DeviceList.cpp
    src/device/DeviceList.h
    src/device/Ntv2CardAudio.cpp
    src/device/Ntv2CardAudio.h
)
target_include_directories(duplex_device SYSTEM PRIVATE ${AJANTV2_INCLUDE_DIRS})
target_link_libraries(duplex_device
    PUBLIC  duplex_common
    PRIVATE duplex_settings duplex_platform ajantv2 Threads::Threads)

# -----------------------------------------------------------------------------
# duplex_engine: the audio engine (no SDK, no ASIO, no Windows)
# -----------------------------------------------------------------------------
add_library(duplex_engine STATIC
    src/engine/AudioEngine.cpp
    src/engine/AudioEngine.h
    src/engine/ClockModel.cpp
    src/engine/ClockModel.h
    src/engine/RingMath.cpp
    src/engine/RingMath.h
    src/engine/RoutingWatch.cpp
    src/engine/RoutingWatch.h
    src/engine/SampleConvert.cpp
    src/engine/SampleConvert.h
)
target_link_libraries(duplex_engine
    PUBLIC  duplex_common
    PRIVATE duplex_settings)

# -----------------------------------------------------------------------------
# duplex_sim: the simulated card (tests and duplex-probe --simulate)
# -----------------------------------------------------------------------------
add_library(duplex_sim STATIC
    src/sim/FakeCardAudio.cpp
    src/sim/FakeCardAudio.h
    src/sim/VirtualClock.h
)
target_link_libraries(duplex_sim
    PUBLIC  duplex_common
    PRIVATE duplex_settings)

# -----------------------------------------------------------------------------
# duplex_asio: what IASIO does, without COM (so the tests run it anywhere)
# -----------------------------------------------------------------------------
# The ASIO SDK's headers (third_party/asiosdk, GPLv3 option) come in as SYSTEM
# includes: their old-style code is not ours to warn about.
set(ASIOSDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common)
add_library(duplex_asio STATIC
    src/asio/AsioSdk.h
    src/asio/ChannelMap.cpp
    src/asio/ChannelMap.h
    src/asio/DriverCore.cpp
    src/asio/DriverCore.h
    src/asio/InstallState.h
    src/asio/SettingsChange.cpp
    src/asio/SettingsChange.h
)
target_include_directories(duplex_asio SYSTEM PUBLIC ${ASIOSDK_DIR})
target_link_libraries(duplex_asio
    PUBLIC  duplex_engine duplex_platform duplex_common
    PRIVATE duplex_settings)

# -----------------------------------------------------------------------------
# AJADuplexASIO.dll: the driver (Windows); AJADuplexASIOSettings.exe, its
# settings outside a host; and duplex-asiotest, a small host
# -----------------------------------------------------------------------------
if(WIN32)
    add_library(AJADuplexASIO SHARED
        src/asio/CardOpeners.cpp
        src/asio/CardOpeners.h
        src/asio/win/AJADuplexASIO.def
        src/asio/win/AsioDriver.cpp
        src/asio/win/AsioDriver.h
        src/asio/win/DriverIdentity.h
        src/asio/win/DriverModule.cpp
        src/asio/win/InstanceLock.cpp
        src/asio/win/InstanceLock.h
        src/asio/win/RegistryStore.cpp
        src/asio/win/RegistryStore.h
        src/ui/SettingsDialog.cpp
        src/ui/SettingsDialog.h
    )
    target_compile_definitions(AJADuplexASIO PRIVATE AJADUPLEX_WITH_NTV2=1)
    target_link_libraries(AJADuplexASIO PRIVATE
        duplex_asio duplex_device duplex_sim duplex_platform duplex_settings
        ole32 uuid advapi32 winmm user32 shell32)
    # Hosts look for the file by its registered path; no "lib" prefix.
    set_target_properties(AJADuplexASIO PROPERTIES PREFIX "")

    add_executable(duplex-asiotest tools/asiotest/main.cpp src/asio/InstallState.h)
    target_include_directories(duplex-asiotest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(duplex-asiotest SYSTEM PRIVATE ${ASIOSDK_DIR})
    target_link_libraries(duplex-asiotest PRIVATE duplex_settings ole32 uuid advapi32)   # the registry, for --installed
    if(MINGW)
        target_link_options(duplex-asiotest PRIVATE -municode)
    endif()

    # The driver's own settings dialog, asked of the installed driver the way
    # a host asks, for hosts with no settings button (vMix); and the Start
    # menu's entry for it, which install-driver.cmd makes.
    add_executable(AJADuplexASIOSettings WIN32 tools/settings/main.cpp)
    if(MSVC)
        # Version 6 of the common controls (merged into the manifest the
        # linker makes); MinGW builds go without.
        target_sources(AJADuplexASIOSettings PRIVATE tools/settings/AJADuplexASIOSettings.manifest)
    endif()
    target_include_directories(AJADuplexASIOSettings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(AJADuplexASIOSettings SYSTEM PRIVATE ${ASIOSDK_DIR})
    target_link_libraries(AJADuplexASIOSettings PRIVATE duplex_settings ole32 uuid shell32 comctl32 user32)
    if(MINGW)
        target_link_options(AJADuplexASIOSettings PRIVATE -municode)
    endif()
endif()

# -----------------------------------------------------------------------------
# duplex-probe: phase 0 measurement tool (console)
# -----------------------------------------------------------------------------
# The measurement procedures only use CardAudio and Clock; they live in a
# library of their own so the unit tests run them against the simulated
# card, without the SDK.
add_library(probe_core STATIC
    tools/probe/CostProbe.cpp
    tools/probe/CostProbe.h
    tools/probe/HeadsProbe.cpp
    tools/probe/HeadsProbe.h
    tools/probe/ToneLoop.cpp
    tools/probe/ToneLoop.h
    tools/probe/LoopProbe.cpp
    tools/probe/LoopProbe.h
    tools/probe/OutputFile.cpp
    tools/probe/OutputFile.h
    tools/probe/ProbeOptions.cpp
    tools/probe/ProbeOptions.h
    tools/probe/RoutingProbe.cpp
    tools/probe/RoutingProbe.h
    tools/probe/WatchProbe.cpp
    tools/probe/WatchProbe.h
)
target_include_directories(probe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools/probe)
target_link_libraries(probe_core
    PUBLIC  duplex_common duplex_engine
    PRIVATE duplex_settings)

add_executable(duplex-probe
    tools/probe/main.cpp
    tools/probe/CommandSupport.cpp
    tools/probe/CommandSupport.h
    tools/probe/MeasureCommands.cpp
    tools/probe/MeasureCommands.h
    tools/probe/InfoCommand.cpp
    tools/probe/InfoCommand.h
    tools/probe/ListCommand.cpp
    tools/probe/ListCommand.h
)
target_link_libraries(duplex-probe PRIVATE
    probe_core duplex_device duplex_sim duplex_platform duplex_settings)
if(WIN32)
    # The driver's lock (one program at a time on a card's audio system).
    target_sources(duplex-probe PRIVATE src/asio/win/InstanceLock.cpp src/asio/win/InstanceLock.h)
endif()

# -----------------------------------------------------------------------------
# Unit tests: no card, no SDK
# -----------------------------------------------------------------------------
enable_testing()

add_executable(duplex-tests
    tests/test_framework.h
    tests/test_main.cpp
    tests/test_ToneLoop.cpp
    tests/test_AudioEngine.cpp
    tests/test_CostProbe.cpp
    tests/test_DriverCore.cpp
    tests/test_DriverSettings.cpp
    tests/test_EngineCore.cpp
    tests/test_FakeCardAudio.cpp
    tests/test_HeadsProbe.cpp
    tests/test_InstallState.cpp
    tests/test_Log.cpp
    tests/test_LoopProbe.cpp
    tests/test_OutputFile.cpp
    tests/test_OwnerMark.cpp
    tests/test_ProbeOptions.cpp
    tests/test_Realtime.cpp
    tests/test_Report.cpp
    tests/test_RoutingProbe.cpp
    tests/test_RoutingWatch.cpp
    tests/test_Stats.cpp
    tests/test_TextFormat.cpp
    tests/test_WatchProbe.cpp
    # The simulated opener, as the driver uses it (the real one says it has no SDK).
    src/asio/CardOpeners.cpp
)
if(WIN32)
    # The driver's Windows pieces that can be tested on their own.
    target_sources(duplex-tests PRIVATE
        tests/test_InstanceLock.cpp src/asio/win/InstanceLock.cpp
        tests/test_SettingsDialog.cpp src/ui/SettingsDialog.cpp)
    target_link_libraries(duplex-tests PRIVATE user32 shell32)
endif()
target_include_directories(duplex-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(duplex-tests PRIVATE
    probe_core duplex_asio duplex_engine duplex_sim duplex_platform duplex_common duplex_settings)

add_test(NAME unit COMMAND duplex-tests)

message(STATUS "AJA-Duplex-ASIO ${PROJECT_VERSION}: libajantv2 at ${AJANTV2_ROOT}")
