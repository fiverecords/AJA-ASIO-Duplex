// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "platform/Realtime.h"

#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#   include <windows.h>
#   include <avrt.h>
#   ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#       define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // Windows 10 1803 SDK
#   endif
#else
#   include <csignal>
#   include <ctime>
#   include <sys/utsname.h>
#endif

namespace ajaduplex::platform
{

namespace
{

std::atomic<bool> gInterrupted{false};

} // namespace

std::int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool interruptRequested()
{
    return gInterrupted.load(std::memory_order_relaxed);
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

namespace
{

/// THREAD_POWER_THROTTLING_STATE and its constants, as the Windows 11 SDK
/// has them (older headers, MinGW's among them, lack them).
struct PowerThrottlingState
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
constexpr ULONG kPowerThrottlingVersion = 1;            // THREAD_POWER_THROTTLING_CURRENT_VERSION
constexpr ULONG kPowerThrottlingExecutionSpeed = 0x1;   // THREAD_POWER_THROTTLING_EXECUTION_SPEED

/// Windows 11 may run the threads of a program it takes to be in the
/// background (its window behind others, say) slower and on efficiency
/// cores (EcoQoS). An audio thread must never be: this one opts out. Says
/// how it went, for the description.
std::string runAtFullSpeed()
{
    PowerThrottlingState state{kPowerThrottlingVersion, kPowerThrottlingExecutionSpeed, 0};
    if (SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state)) != FALSE)
        return "; power throttling off";
    return "; power throttling could not be turned off (error " + std::to_string(GetLastError()) + ")";
}

} // namespace

ProAudioScope::ProAudioScope()
{
    const std::string speed = runAtFullSpeed();
    DWORD taskIndex = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (handle == nullptr)
    {
        description_ = "MMCSS Pro Audio not available (error " + std::to_string(GetLastError()) + ")" + speed;
        return;
    }
    handle_ = handle;
    active_ = true;
    const bool critical = AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL) != FALSE;
    description_ = (critical ? "MMCSS Pro Audio, critical priority"
                             : "MMCSS Pro Audio, default priority (critical refused)") +
                   speed;
}

ProAudioScope::~ProAudioScope()
{
    if (handle_ != nullptr)
        AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle_));
}

WakeTimer::WakeTimer()
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr)
        highResolution_ = true;
    else
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    timer_ = timer;
}

WakeTimer::~WakeTimer()
{
    if (timer_ != nullptr)
        CloseHandle(static_cast<HANDLE>(timer_));
}

const char* WakeTimer::kind() const
{
    if (timer_ == nullptr)
        return "std::this_thread::sleep_for (no waitable timer)";
    return highResolution_ ? "high-resolution waitable timer" : "waitable timer (no high-resolution timer on this system)";
}

void WakeTimer::sleepUntil(std::int64_t targetNs)
{
    const std::int64_t remaining = targetNs - steadyNowNs();
    if (remaining <= 0)
        return;
    if (timer_ != nullptr)
    {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((remaining + 99) / 100);   // relative, in 100 ns units
        if (SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
}

namespace
{

BOOL WINAPI onConsoleControl(DWORD type)
{
    switch (type)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            gInterrupted.store(true);
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            // The console window is closing, or the session ending: Windows
            // ends the process when this returns, or about five seconds
            // later whatever it does. The command is asked to stop, and this
            // waits so that it has that long to put the card back as it
            // found it (left running, an audio system counts as in use for
            // the next program); the process ends, and this wait with it,
            // when the command returns from main.
            gInterrupted.store(true);
            Sleep(10000);
            return TRUE;
        default:
            return FALSE;
    }
}

} // namespace

void installInterruptHandler()
{
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true))
        SetConsoleCtrlHandler(onConsoleControl, TRUE);
}

std::string systemDescription()
{
    // GetVersionEx reports the version the program is manifested for;
    // RtlGetVersion reports the real one.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                                     reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                               : nullptr;
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion == nullptr || rtlGetVersion(&info) != 0)
        return "Windows (version unknown)";
    return "Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
           " build " + std::to_string(info.dwBuildNumber);
}

#else

// ---------------------------------------------------------------------------
// POSIX fallbacks, for development builds
// ---------------------------------------------------------------------------

ProAudioScope::ProAudioScope()
    : description_("no MMCSS on this system (normal thread priority)")
{
}

ProAudioScope::~ProAudioScope() = default;

WakeTimer::WakeTimer() = default;
WakeTimer::~WakeTimer() = default;

const char* WakeTimer::kind() const
{
    return "clock_nanosleep (CLOCK_MONOTONIC, absolute)";
}

void WakeTimer::sleepUntil(std::int64_t targetNs)
{
    // libstdc++ and libc++ base steady_clock on CLOCK_MONOTONIC, so the
    // target is an absolute CLOCK_MONOTONIC time.
    if (targetNs <= steadyNowNs())
        return;
    timespec ts;
    ts.tv_sec = static_cast<time_t>(targetNs / 1000000000);
    ts.tv_nsec = static_cast<long>(targetNs % 1000000000);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0)
    {
        if (targetNs <= steadyNowNs())
            break;
    }
}

namespace
{

void onInterruptSignal(int)
{
    gInterrupted.store(true);   // lock-free atomic: safe in a signal handler
}

} // namespace

void installInterruptHandler()
{
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true))
    {
        std::signal(SIGINT, onInterruptSignal);
        std::signal(SIGTERM, onInterruptSignal);
    }
}

std::string systemDescription()
{
    utsname names{};
    if (uname(&names) != 0)
        return "POSIX (unknown)";
    return std::string(names.sysname) + " " + names.release + " " + names.machine;
}

#endif

} // namespace ajaduplex::platform
