// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// duplex-probe watch on the simulated card: the driver's set-up, read back;
// nothing to say when nobody touches the card; each change of another
// program once, at the check after it, and put back with --keep; Q9 in the
// report.

#include "test_framework.h"

#include "WatchProbe.h"
#include "sim/FakeCardAudio.h"
#include "sim/VirtualClock.h"

#include <string>

using namespace ajaduplex;
using namespace ajaduplex::probe;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

std::function<bool()> never()
{
    return [] { return false; };
}

/// Another program takes AES out 1-4 at `at` seconds.
std::function<void(double)> takesAesAt(FakeCardAudio& card, double at)
{
    return [&card, at, done = false](double seconds) mutable {
        if (done || seconds < at)
            return;
        device::CardRouting theirs = card.routing();
        theirs.aes[0] = device::OutputSource{1, 1};
        card.anotherProgramSets(theirs, device::routing::kAesGroup);
        done = true;
    };
}

} // namespace

TEST_CASE(WatchProbe, NothingHappensWhenNobodyTouchesTheCard)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel{});
    WatchSettings settings;
    settings.seconds = 90.0;
    const WatchRun run = runWatch(card, clock, settings, never(), {}, {});
    ASSERT_TRUE(run.started);
    EXPECT_TRUE(run.watched);
    EXPECT_TRUE(run.baseline.source == device::InputSource::Analog);
    EXPECT_EQ(run.baseline.xpandSelect, 1u);
    EXPECT_TRUE(run.baseline.aes[0] == (device::OutputSource{2, 1}));
    EXPECT_TRUE(run.checks >= 85 && run.checks <= 90);
    EXPECT_EQ(run.changes, 0u);
    EXPECT_TRUE(run.events.empty());
    EXPECT_EQ(run.stats.lateOutputs, std::uint64_t(0));

    Report report;
    addWatchReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "routing at start");
    EXPECT_CONTAINS(text, "AES out 1-4: audio system 2, channels 1-4");
    EXPECT_CONTAINS(text, "0 by other programs");
    EXPECT_CONTAINS(text, "AJA's services leave the audio routing alone");
    card.restore();
}

TEST_CASE(WatchProbe, AChangeIsReportedAtTheNextCheck)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel{});
    WatchSettings settings;
    settings.seconds = 20.0;
    std::vector<WatchEvent> seen;
    const WatchRun run = runWatch(card, clock, settings, never(), takesAesAt(card, 5.5),
                                  [&seen](const WatchEvent& e) { seen.push_back(e); });
    ASSERT_TRUE(run.started);
    ASSERT_EQ(run.events.size(), std::size_t(1));
    EXPECT_EQ(seen.size(), std::size_t(1));
    EXPECT_TRUE(run.events[0].seconds >= 5.5 && run.events[0].seconds < 7.1);
    EXPECT_CONTAINS(run.events[0].line, "another program changed AES out 1-4 to audio system 1, channels 1-4");
    EXPECT_CONTAINS(run.events[0].line, "left as it is (--keep is off)");
    EXPECT_EQ(run.changes, 1u);
    EXPECT_EQ(run.notOursAtEnd, device::routing::kAesGroup);
    EXPECT_FALSE(card.aesOutputsRouted());

    Report report;
    addWatchReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "not the driver's at the end");
    EXPECT_CONTAINS(text, "routing at the end");
    EXPECT_CONTAINS(text, "the routing changed 1 time in standard task mode");
    card.restore();
}

TEST_CASE(WatchProbe, KeepPutsItBack)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel{});
    WatchSettings settings;
    settings.seconds = 10.0;
    settings.keep = true;
    settings.analog = false;
    const WatchRun run = runWatch(card, clock, settings, never(), takesAesAt(card, 3.2), {});
    ASSERT_TRUE(run.started);
    EXPECT_TRUE(run.baseline.source == device::InputSource::Aes);
    EXPECT_EQ(run.changes, 1u);
    EXPECT_EQ(run.putBack, 1u);
    EXPECT_EQ(run.notOursAtEnd, 0u);
    ASSERT_EQ(run.events.size(), std::size_t(1));
    EXPECT_CONTAINS(run.events[0].line, "; put back");
    EXPECT_TRUE(card.aesOutputsRouted());
    card.restore();
}

TEST_CASE(WatchProbe, Q9NeedsStandardModeAndTime)
{
    VirtualClock clock;
    FakeCardModel model;
    model.taskMode = device::TaskMode::Oem;
    FakeCardAudio card(clock, model);
    WatchSettings settings;
    settings.seconds = 5.0;
    WatchRun run = runWatch(card, clock, settings, never(), {}, {});
    Report report;
    addWatchReport(run, report);
    EXPECT_CONTAINS(report.render(true), "not answered: the card was in OEM task mode");
    card.restore();

    VirtualClock clock2;
    FakeCardAudio standard(clock2, FakeCardModel{});
    run = runWatch(standard, clock2, settings, never(), {}, {});
    Report shortRun;
    addWatchReport(run, shortRun);
    EXPECT_CONTAINS(shortRun.render(true), "is short: run it for five minutes");
    standard.restore();
}

TEST_CASE(WatchProbe, StopsWhenAsked)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel{});
    WatchSettings settings;
    settings.seconds = 600.0;
    int asked = 0;
    const WatchRun run = runWatch(card, clock, settings, [&asked] { return ++asked > 1000; }, {}, {});
    EXPECT_TRUE(run.interrupted);
    EXPECT_TRUE(run.seconds < 10.0);
    card.restore();
}

TEST_CASE(WatchProbe, TheLevelIsReportedAndCountsForQ9)
{
    // watch leaves the Xpand's analog level as the card has it, as the
    // driver does by default: a change is reported, not put back, and it is
    // the services rewriting the audio all the same.
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel{});
    WatchSettings settings;
    settings.seconds = 70.0;
    settings.keep = true;
    auto meanwhile = [&card, done = false](double seconds) mutable {
        if (done || seconds < 10.0)
            return;
        device::CardRouting theirs = card.routing();
        theirs.analogLevelDbu = 18;
        card.anotherProgramSets(theirs, device::routing::kAnalogLevel);
        done = true;
    };
    const WatchRun run = runWatch(card, clock, settings, never(), meanwhile, {});
    ASSERT_TRUE(run.started);
    EXPECT_EQ(run.changes, 0u);
    EXPECT_EQ(run.otherChanges, 1u);
    EXPECT_EQ(run.putBack, 0u);
    ASSERT_EQ(run.events.size(), std::size_t(1));
    EXPECT_EQ(run.events[0].line, std::string("routing: Xpand analog level is now +18 dBu (it was +24 dBu)"));
    EXPECT_EQ(card.routing().analogLevelDbu, 18);
    Report report;
    addWatchReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "and 1 to the analog level, which it leaves as the card has it");
    EXPECT_CONTAINS(text, "the routing changed 1 time in standard task mode");
    card.restore();
}
