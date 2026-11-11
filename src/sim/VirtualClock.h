// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    VirtualClock.h
 * @brief   A clock that only moves when told to: the simulated card's time.
 *
 * Sleeping jumps straight to the target, and the simulated card advances
 * the clock by what each register read or DMA would cost. A polling loop
 * against the simulated card therefore makes progress, and a run of
 * minutes of simulated time takes milliseconds.
 */

#pragma once

#include "common/Clock.h"

#include <cstdint>

namespace ajaduplex::sim
{

/// How late a sleep returns, to make the simulation look like a real
/// scheduler: uniformly up to twice `typicalNs`, plus `tailNs` once in a
/// while (`tailProbability` per sleep). All zero by default: exact wake-ups.
struct SleepLateness
{
    std::int64_t typicalNs = 0;
    double tailProbability = 0.0;
    std::int64_t tailNs = 0;
    std::uint32_t seed = 7;
};

class VirtualClock final : public Clock
{
public:
    explicit VirtualClock(std::int64_t startNs = 1000000000) : now_(startNs) {}

    std::int64_t nowNs() override { return now_; }

    void sleepUntilNs(std::int64_t targetNs) override
    {
        if (targetNs > now_)
            now_ = targetNs + lateness();
    }

    void advance(std::int64_t ns)
    {
        if (ns > 0)
            now_ += ns;
    }

    void setSleepLateness(const SleepLateness& lateness)
    {
        lateness_ = lateness;
        rng_ = lateness.seed ? lateness.seed : 7u;
    }

private:
    std::uint32_t nextRandom()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    std::int64_t lateness()
    {
        std::int64_t late = 0;
        if (lateness_.typicalNs > 0)
            late += static_cast<std::int64_t>(nextRandom() % static_cast<std::uint32_t>(2 * lateness_.typicalNs + 1));
        if (lateness_.tailProbability > 0.0 &&
            static_cast<double>(nextRandom()) / 4294967296.0 < lateness_.tailProbability)
            late += lateness_.tailNs;
        return late;
    }

    std::int64_t now_;
    SleepLateness lateness_;
    std::uint32_t rng_ = 7;
};

} // namespace ajaduplex::sim
