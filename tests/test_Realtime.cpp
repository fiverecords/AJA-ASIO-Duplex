// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// The platform layer on the machine running the tests. These use real time,
// so they only check what holds on any machine: order, not precision.

#include "test_framework.h"

#include "platform/Realtime.h"

using namespace ajaduplex::platform;

TEST_CASE(Realtime, SteadyClockMovesForward)
{
    const std::int64_t a = steadyNowNs();
    const std::int64_t b = steadyNowNs();
    EXPECT_TRUE(b >= a);
}

TEST_CASE(Realtime, WakeTimerDoesNotReturnEarly)
{
    WakeTimer timer;
    EXPECT_TRUE(timer.kind() != nullptr);
    for (int i = 0; i < 5; ++i)
    {
        const std::int64_t target = steadyNowNs() + 2000000;   // 2 ms
        timer.sleepUntil(target);
        const std::int64_t now = steadyNowNs();
        EXPECT_TRUE(now >= target);
        EXPECT_TRUE(now - target < 500000000);   // and not absurdly late
    }
    const std::int64_t before = steadyNowNs();
    timer.sleepUntil(before - 1000);   // in the past: returns at once
    EXPECT_TRUE(steadyNowNs() - before < 100000000);
}

TEST_CASE(Realtime, SystemClockSleeps)
{
    SystemClock clock;
    const std::int64_t t0 = clock.nowNs();
    clock.sleepForNs(1000000);
    EXPECT_TRUE(clock.nowNs() - t0 >= 1000000);
}

TEST_CASE(Realtime, ProAudioScopeDescribesItself)
{
    ProAudioScope scope;
    EXPECT_FALSE(scope.description().empty());
}

TEST_CASE(Realtime, NoInterruptByDefault)
{
    installInterruptHandler();
    installInterruptHandler();   // twice is fine
    EXPECT_FALSE(interruptRequested());
    EXPECT_FALSE(systemDescription().empty());
}
