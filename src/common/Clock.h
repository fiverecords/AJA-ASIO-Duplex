// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Clock.h
 * @brief   Time source for code that measures or waits.
 *
 * Measurement and engine code asks a Clock for the time and sleeps through
 * it, never through std::chrono directly. On the real card that is the
 * system clock (platform::SystemClock: the steady clock, which is the
 * performance counter on Windows); against the simulated card it is a
 * virtual clock that only moves when the simulation says time passes, so
 * tests run fast and give the same result every time.
 */

#pragma once

#include <cstdint>

namespace ajaduplex
{

class Clock
{
public:
    virtual ~Clock() = default;

    /// Monotonic time in nanoseconds. Only differences mean anything.
    virtual std::int64_t nowNs() = 0;

    /// Returns at or after `targetNs` (same time base as nowNs). Returns at
    /// once when the target is already in the past.
    virtual void sleepUntilNs(std::int64_t targetNs) = 0;

    void sleepForNs(std::int64_t durationNs) { sleepUntilNs(nowNs() + durationNs); }
};

} // namespace ajaduplex
