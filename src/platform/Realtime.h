// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Realtime.h
 * @brief   The operating-system pieces a real-time audio thread needs.
 *
 * Windows is the target: MMCSS "Pro Audio" priority, the high-resolution
 * waitable timer (Windows 10 1803 and later) and the console Ctrl+C
 * handler. Elsewhere the same interface falls back to POSIX equivalents so
 * the rest of the code builds and runs for development.
 */

#pragma once

#include "common/Clock.h"

#include <cstdint>
#include <string>

namespace ajaduplex::platform
{

/// Puts the calling thread in the MMCSS "Pro Audio" task with critical
/// priority for the lifetime of the object (Windows). Elsewhere it does
/// nothing and says so in description().
class ProAudioScope
{
public:
    ProAudioScope();
    ~ProAudioScope();
    ProAudioScope(const ProAudioScope&) = delete;
    ProAudioScope& operator=(const ProAudioScope&) = delete;

    bool active() const { return active_; }
    const std::string& description() const { return description_; }

private:
    [[maybe_unused]] void* handle_ = nullptr;   // Windows only
    bool active_ = false;
    std::string description_;
};

/// Sleeps until an absolute steady-clock time with the best timer at hand:
/// on Windows a high-resolution waitable timer when the system has one,
/// otherwise a normal waitable timer; clock_nanosleep elsewhere.
class WakeTimer
{
public:
    WakeTimer();
    ~WakeTimer();
    WakeTimer(const WakeTimer&) = delete;
    WakeTimer& operator=(const WakeTimer&) = delete;

    /// True when the Windows high-resolution timer is in use.
    bool highResolution() const { return highResolution_; }
    const char* kind() const;

    /// `targetNs` is std::chrono::steady_clock time in nanoseconds.
    void sleepUntil(std::int64_t targetNs);

private:
    [[maybe_unused]] void* timer_ = nullptr;    // Windows only
    bool highResolution_ = false;
};

/// steady_clock time in nanoseconds (the performance counter on Windows).
std::int64_t steadyNowNs();

/// The real clock: steady_clock for the time, WakeTimer for sleeping.
class SystemClock final : public Clock
{
public:
    std::int64_t nowNs() override { return steadyNowNs(); }
    void sleepUntilNs(std::int64_t targetNs) override { timer_.sleepUntil(targetNs); }
    const WakeTimer& timer() const { return timer_; }

private:
    WakeTimer timer_;
};

/// Makes Ctrl+C and Ctrl+Break set a flag instead of ending the process, so
/// a command can stop cleanly and put the card back the way it found it.
/// On Windows, closing the console window (or logging off) sets it too, and
/// the command gets the few seconds Windows allows to do the same.
/// Safe to call more than once.
void installInterruptHandler();

/// True once Ctrl+C or Ctrl+Break has been pressed, or the window closed.
bool interruptRequested();

/// Short description of the operating system, for report headers.
std::string systemDescription();

} // namespace ajaduplex::platform
