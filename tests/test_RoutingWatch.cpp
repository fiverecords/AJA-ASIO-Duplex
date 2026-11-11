// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// The routing watch on the simulated card: what configure() routes is read
// back as ours; what another program changes is found at the next check,
// once, and put back when asked; the task mode is only reported; and
// restore() leaves another program's routing as it found it.

#include "test_framework.h"

#include "engine/RoutingWatch.h"
#include "sim/FakeCardAudio.h"
#include "sim/VirtualClock.h"

#include <string>
#include <vector>

using namespace ajaduplex;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;
namespace routing = device::routing;

namespace
{

constexpr std::int64_t kMs = 1000000;

/// The driver's set-up: the input, the select following it, AES out 1-8
/// and the monitor on this audio system.
device::AudioConfig driverConfig(device::InputSource source = device::InputSource::Analog)
{
    device::AudioConfig config;
    config.source = source;
    config.keepXpandSelect = false;
    config.routeAesOutputs = true;
    config.routeMonitor = true;
    config.monitorPair = 0;
    return config;
}

/// The KONA X after power-up: AES outputs and monitor on audio system 1.
FakeCardModel poweredUp()
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.aesOutputsOnThisSystem = false;
    model.monitorOnThisSystem = false;
    return model;
}

struct Rig
{
    VirtualClock clock;
    FakeCardAudio card;
    engine::RoutingWatch watch;

    explicit Rig(const FakeCardModel& model = poweredUp(), const device::AudioConfig& config = driverConfig())
        : card(clock, model), watch(card)
    {
        std::string error;
        configured = card.configure(config, error);
        watching = configured && watch.takeBaseline();
    }

    /// Another program sets `parts`, changed by `change` from what the card holds.
    template <typename Change>
    bool another(unsigned parts, Change change)
    {
        device::CardRouting theirs = card.routing();
        change(theirs);
        return card.anotherProgramSets(theirs, parts);
    }

    bool configured = false;
    bool watching = false;
};

std::string part(const device::CardRouting& r, unsigned bit)
{
    char text[96];
    return engine::describeRoutingPart(r, bit, text, sizeof(text));
}

} // namespace

TEST_CASE(RoutingWatch, ReadsBackWhatConfigureRouted)
{
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());
    device::CardRouting before;
    ASSERT_TRUE(card.readRouting(before));
    EXPECT_TRUE(before.aes[0] == (device::OutputSource{1, 1}));
    EXPECT_TRUE(before.monitor == (device::OutputSource{1, 1}));

    std::string error;
    ASSERT_TRUE(card.configure(driverConfig(), error));
    device::CardRouting r;
    ASSERT_TRUE(card.readRouting(r));
    EXPECT_TRUE(r.source == device::InputSource::Analog);
    EXPECT_EQ(r.xpandSelect, 1u);
    EXPECT_EQ(r.aesGroups, 2u);
    EXPECT_TRUE(r.aes[0] == (device::OutputSource{2, 1}));
    EXPECT_TRUE(r.aes[1] == (device::OutputSource{2, 5}));
    EXPECT_TRUE(r.monitor == (device::OutputSource{2, 1}));
    EXPECT_TRUE(r.taskMode == device::TaskMode::Standard);
    EXPECT_EQ(r.analogLevelDbu, 24);
    EXPECT_EQ(device::routingParts(r), routing::kSource | routing::kXpandSelect | routing::kAnalogLevel |
                                           routing::kAesGroup | (routing::kAesGroup << 1) | routing::kMonitor |
                                           routing::kTaskMode);
    EXPECT_EQ(device::routingDifferences(before, r), routing::kSource | routing::kXpandSelect | routing::kAesGroup |
                                                         (routing::kAesGroup << 1) | routing::kMonitor);
    EXPECT_EQ(engine::describeRouting(r),
              std::string("input source: analog; Xpand input select: 1 (analog); Xpand analog level: +24 dBu; AES "
                          "out 1-4: audio system 2, channels 1-4; AES out 5-8: audio system 2, channels 5-8; "
                          "monitor: audio system 2, channels 1-2; task mode: standard"));
}

TEST_CASE(RoutingWatch, NamesAndValues)
{
    device::CardRouting r;
    EXPECT_EQ(part(r, routing::kSource), std::string("unreadable"));
    EXPECT_EQ(engine::describeRouting(r), std::string("nothing could be read"));
    r.haveSource = true;
    r.source = device::InputSource::Embedded;
    r.embeddedInput = 2;
    EXPECT_EQ(part(r, routing::kSource), std::string("SDI 3 embedded"));
    r.source = device::InputSource::Other;
    r.sourceCode = 11;
    EXPECT_EQ(part(r, routing::kSource), std::string("another source (code 11)"));
    r.haveXpandSelect = true;
    r.xpandSelect = 5;
    EXPECT_EQ(part(r, routing::kXpandSelect), std::string("5"));
    r.xpandSelect = 0;
    EXPECT_EQ(part(r, routing::kXpandSelect), std::string("0 (AES)"));
    EXPECT_EQ(part(r, routing::kSource | routing::kXpandSelect), std::string("?"));   // one part at a time
    r.haveAnalogLevel = true;
    r.analogLevelDbu = 15;
    EXPECT_EQ(part(r, routing::kAnalogLevel), std::string("+15 dBu"));
    r.analogLevelDbu = 0;
    r.analogLevelCode = 7;
    EXPECT_EQ(part(r, routing::kAnalogLevel), std::string("a value without a name (code 7)"));
    EXPECT_EQ(std::string(engine::routingPartName(routing::kAnalogLevel)), std::string("Xpand analog level"));

    EXPECT_EQ(std::string(engine::routingPartName(routing::kAesGroup << 1)), std::string("AES out 5-8"));
    EXPECT_EQ(std::string(engine::routingPartName(routing::kSdiOut)), std::string("SDI out 1"));
    EXPECT_EQ(std::string(engine::routingPartName(routing::kSdiOut << 7)), std::string("SDI out 8"));
    EXPECT_EQ(std::string(engine::routingPartName(routing::kHdmiOut)), std::string("HDMI out"));
    r.sdiOutputs = 2;
    r.sdiOut[1] = {2, true};
    EXPECT_EQ(part(r, routing::kSdiOut << 1), std::string("audio system 2"));
    r.sdiOut[1] = {1, false};
    EXPECT_EQ(part(r, routing::kSdiOut << 1), std::string("no audio (its embedder is off; audio system 1)"));
    EXPECT_EQ(part(r, routing::kSdiOut), std::string("unreadable"));   // SDI out 1 was not read
    r.haveHdmiOut = true;
    r.hdmiOut = {2, 8, 1};
    EXPECT_EQ(part(r, routing::kHdmiOut), std::string("audio system 2, channels 1-8"));
    r.hdmiOut = {1, 2, 3};
    EXPECT_EQ(part(r, routing::kHdmiOut), std::string("audio system 1, channels 3-4"));
    EXPECT_EQ(engine::routingPartNames(routing::kAesGroup | routing::kMonitor | routing::kTaskMode),
              std::string("AES out 1-4, monitor, task mode"));
    EXPECT_EQ(engine::routingPartNames(0), std::string());
}

TEST_CASE(RoutingWatch, AChangeIsSeenOnceAndLeftAloneUnlessKept)
{
    Rig rig;
    ASSERT_TRUE(rig.watching);
    engine::RoutingWatch::Check c = rig.watch.check(false);
    EXPECT_TRUE(c.read);
    EXPECT_EQ(c.changed, 0u);
    EXPECT_EQ(c.notOurs, 0u);

    // Another program takes AES out 1-4 for audio system 1.
    ASSERT_TRUE(rig.another(routing::kAesGroup, [](device::CardRouting& r) { r.aes[0] = {1, 1}; }));
    c = rig.watch.check(false);
    EXPECT_EQ(c.changed, routing::kAesGroup);
    EXPECT_EQ(c.notOurs, routing::kAesGroup);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_EQ(part(c.before, routing::kAesGroup), std::string("audio system 2, channels 1-4"));
    EXPECT_EQ(part(c.now, routing::kAesGroup), std::string("audio system 1, channels 1-4"));
    EXPECT_FALSE(rig.card.aesOutputsRouted());

    // Still theirs at the next check: no change, still not ours.
    c = rig.watch.check(false);
    EXPECT_EQ(c.changed, 0u);
    EXPECT_EQ(c.notOurs, routing::kAesGroup);

    // Kept: put back, and seen to take.
    c = rig.watch.check(true);
    EXPECT_EQ(c.putBack, routing::kAesGroup);
    EXPECT_EQ(c.putBackFailed, 0u);
    EXPECT_TRUE(rig.card.aesOutputsRouted());
    c = rig.watch.check(true);
    EXPECT_EQ(c.changed, 0u);
    EXPECT_EQ(c.notOurs, 0u);
    EXPECT_EQ(c.putBack, 0u);
}

TEST_CASE(RoutingWatch, BackToOursByItselfIsAChangeToo)
{
    Rig rig;
    ASSERT_TRUE(rig.another(routing::kMonitor, [](device::CardRouting& r) { r.monitor = {1, 3}; }));
    engine::RoutingWatch::Check c = rig.watch.check(false);
    EXPECT_EQ(c.changed, routing::kMonitor);
    EXPECT_EQ(c.notOurs, routing::kMonitor);
    ASSERT_TRUE(rig.another(routing::kMonitor, [](device::CardRouting& r) { r.monitor = {2, 1}; }));
    c = rig.watch.check(false);
    EXPECT_EQ(c.changed, routing::kMonitor);
    EXPECT_EQ(c.notOurs, 0u);
}

TEST_CASE(RoutingWatch, ASourceSetElsewhereRewritesTheSelectAndBothGoBack)
{
    // Another program sets AES on its own audio system: the SDK rewrites the
    // shared select, and the analog inputs are gone.
    Rig rig;
    ASSERT_TRUE(rig.card.analogSelected());
    ASSERT_TRUE(rig.another(routing::kXpandSelect, [](device::CardRouting& r) { r.xpandSelect = 0; }));
    EXPECT_FALSE(rig.card.analogSelected());
    engine::RoutingWatch::Check c = rig.watch.check(true);
    EXPECT_EQ(c.changed, routing::kXpandSelect);
    EXPECT_EQ(c.putBack, routing::kXpandSelect);
    EXPECT_TRUE(rig.card.analogSelected());

    // Someone sets this audio system's own source to AES: the select follows.
    ASSERT_TRUE(rig.another(routing::kSource, [](device::CardRouting& r) { r.source = device::InputSource::Aes; }));
    EXPECT_EQ(rig.card.xpandSelect(), 0u);
    c = rig.watch.check(true);
    EXPECT_EQ(c.changed, routing::kSource | routing::kXpandSelect);
    EXPECT_EQ(c.putBack, routing::kSource | routing::kXpandSelect);
    EXPECT_TRUE(rig.card.routing().source == device::InputSource::Analog);
    EXPECT_EQ(rig.card.xpandSelect(), 1u);
}

TEST_CASE(RoutingWatch, TheTaskModeIsReportedNeverWritten)
{
    Rig rig;
    rig.card.anotherProgramSetsTaskMode(device::TaskMode::Oem);
    const engine::RoutingWatch::Check c = rig.watch.check(true);
    EXPECT_EQ(c.changed, routing::kTaskMode);
    EXPECT_EQ(c.notOurs, 0u);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_TRUE(rig.card.routing().taskMode == device::TaskMode::Oem);
    EXPECT_EQ(part(c.now, routing::kTaskMode), std::string("OEM"));
    EXPECT_EQ(part(c.before, routing::kTaskMode), std::string("standard"));
}

TEST_CASE(RoutingWatch, RestoreLeavesAnotherProgramsRouting)
{
    FakeCardModel model = poweredUp();
    model.analog.selectedAtStart = true;   // so that "put back" and "left" differ for the select too
    Rig rig(model);
    ASSERT_TRUE(rig.configured);
    ASSERT_TRUE(rig.another((routing::kAesGroup << 1) | routing::kXpandSelect, [](device::CardRouting& r) {
        r.aes[1] = {1, 1};   // AES out 5-8 now play audio system 1's channels 1-4
        r.xpandSelect = 0;
    }));
    rig.card.restore();
    const device::CardRouting& r = rig.card.routing();
    EXPECT_TRUE(r.aes[0] == (device::OutputSource{1, 1}));   // ours: back as it was found
    EXPECT_TRUE(r.aes[1] == (device::OutputSource{1, 1}));   // theirs: left
    EXPECT_TRUE(r.monitor == (device::OutputSource{1, 1}));  // ours: back
    EXPECT_EQ(r.xpandSelect, 0u);                             // theirs: left (it was 1)
    EXPECT_TRUE(r.source == device::InputSource::Aes);        // this audio system's own: back
}

TEST_CASE(RoutingWatch, OnlyTheGroupTakenGoesQuiet)
{
    // AES out 1-8 looped to AES in 1-8. Another program takes AES out 1-4,
    // then points AES out 5-8 at our channels 1-4: AES in 1-4 hear nothing,
    // AES in 5 hears our output 1.
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.cableDelayFrames = 12;
    Rig rig(model, driverConfig(device::InputSource::Aes));
    ASSERT_TRUE(rig.configured);
    ASSERT_TRUE(rig.another(routing::kAesGroup | (routing::kAesGroup << 1), [](device::CardRouting& r) {
        r.aes[0] = {1, 1};
        r.aes[1] = {2, 1};
    }));
    std::vector<std::int32_t> frames(1024 * 16);
    for (std::size_t f = 0; f < 1024; ++f)
        for (unsigned c = 0; c < 16; ++c)
            frames[f * 16 + c] = static_cast<std::int32_t>((f << 12) | (c << 8));
    ASSERT_TRUE(rig.card.dmaWrite(device::Ring::Playback, 0, frames.data(), 1024 * 64));
    ASSERT_TRUE(rig.card.startOutput());
    ASSERT_TRUE(rig.card.startInput());
    rig.clock.advance(20 * kMs);
    std::vector<std::int32_t> captured(512 * 16);
    ASSERT_TRUE(rig.card.dmaRead(device::Ring::Capture, 0, captured.data(), 512 * 64));
    EXPECT_EQ(captured[100 * 16 + 0], 0);
    EXPECT_EQ(captured[100 * 16 + 3], 0);
    EXPECT_EQ(captured[100 * 16 + 4], frames[88 * 16 + 0]);
    EXPECT_EQ(captured[100 * 16 + 7], frames[88 * 16 + 3]);
}

TEST_CASE(RoutingWatch, SourcesItCannotSetAreRefused)
{
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());
    device::AudioConfig config = driverConfig();
    config.source = device::InputSource::Other;
    std::string error;
    EXPECT_FALSE(card.configure(config, error));
    EXPECT_CONTAINS(error, "another source");

    // Nor can a part be written that the routing given does not have.
    ASSERT_TRUE(card.configure(driverConfig(), error));
    device::CardRouting empty;
    EXPECT_FALSE(card.writeRouting(empty, routing::kMonitor));
    device::CardRouting r = card.routing();
    r.aes[0] = {2, 3};   // not the start of a group of four
    EXPECT_FALSE(card.writeRouting(r, routing::kAesGroup));
    EXPECT_TRUE(card.routing().aes[0] == (device::OutputSource{2, 1}));
}

// ---------------------------------------------------------------------------
// The Xpand's analog level
// ---------------------------------------------------------------------------

namespace
{

struct Lines
{
    std::vector<std::string> lines;
    static void add(void* context, const char* line) { static_cast<Lines*>(context)->lines.push_back(line); }
};

} // namespace

TEST_CASE(RoutingWatch, ALevelLeftAsTheCardHasItIsOnlyReported)
{
    // The driver's default: the level is not the driver's, so a change is
    // said, as the task mode is, and not put back even when keeping.
    Rig rig;
    ASSERT_TRUE(rig.watch.takeBaseline(routing::kWritable & ~routing::kAnalogLevel));
    EXPECT_EQ(rig.watch.owned() & routing::kAnalogLevel, 0u);
    ASSERT_TRUE(rig.another(routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 18; }));
    const engine::RoutingWatch::Check c = rig.watch.check(true);
    EXPECT_EQ(c.changed, routing::kAnalogLevel);
    EXPECT_EQ(c.notOurs, 0u);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_EQ(rig.card.routing().analogLevelDbu, 18);
    Lines out;
    EXPECT_EQ(engine::reportCheck(rig.watch, c, "Keep my routing", &Lines::add, &out), 0u);
    ASSERT_EQ(out.lines.size(), std::size_t(1));
    EXPECT_EQ(out.lines[0], std::string("routing: Xpand analog level is now +18 dBu (it was +24 dBu)"));
}

TEST_CASE(RoutingWatch, ALevelTheDriverSetIsKept)
{
    device::AudioConfig config = driverConfig();
    config.analogLevelDbu = 15;
    Rig rig(poweredUp(), config);
    ASSERT_TRUE(rig.watching);
    EXPECT_EQ(rig.card.routing().analogLevelDbu, 15);
    int level = 0;
    ASSERT_TRUE(rig.card.readAnalogLevel(level));
    EXPECT_EQ(level, 15);
    ASSERT_TRUE(rig.another(routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 12; }));
    const engine::RoutingWatch::Check c = rig.watch.check(true);
    EXPECT_EQ(c.notOurs, routing::kAnalogLevel);
    EXPECT_EQ(c.putBack, routing::kAnalogLevel);
    EXPECT_EQ(rig.card.routing().analogLevelDbu, 15);
    Lines out;
    EXPECT_EQ(engine::reportCheck(rig.watch, c, "Keep my routing", &Lines::add, &out), 1u);
    ASSERT_EQ(out.lines.size(), std::size_t(1));
    EXPECT_CONTAINS(out.lines[0], "another program changed Xpand analog level to +12 dBu; the driver's is +15 dBu; "
                                  "put back");
}

TEST_CASE(RoutingWatch, ChangeMakesAPartTheDriversAndReleaseLetsItGo)
{
    Rig rig;
    ASSERT_TRUE(rig.watch.takeBaseline(routing::kWritable & ~routing::kAnalogLevel));
    device::CardRouting wanted = rig.watch.baseline();
    wanted.analogLevelDbu = 18;
    EXPECT_TRUE(rig.watch.change(wanted, routing::kAnalogLevel));
    EXPECT_EQ(rig.card.routing().analogLevelDbu, 18);
    EXPECT_EQ(rig.watch.baseline().analogLevelDbu, 18);
    EXPECT_NE(rig.watch.owned() & routing::kAnalogLevel, 0u);
    engine::RoutingWatch::Check c = rig.watch.check(true);
    EXPECT_EQ(c.changed, 0u);   // the driver's own change is no news

    ASSERT_TRUE(rig.another(routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 12; }));
    c = rig.watch.check(true);
    EXPECT_EQ(c.putBack, routing::kAnalogLevel);
    EXPECT_EQ(rig.card.routing().analogLevelDbu, 18);

    rig.watch.release(routing::kAnalogLevel);
    ASSERT_TRUE(rig.another(routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 12; }));
    c = rig.watch.check(true);
    EXPECT_EQ(c.changed, routing::kAnalogLevel);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_EQ(rig.card.routing().analogLevelDbu, 12);

    wanted.analogLevelDbu = 20;   // not one of the Xpand's
    EXPECT_FALSE(rig.watch.change(wanted, routing::kAnalogLevel));
}

TEST_CASE(RoutingWatch, TheLevelGoesBackUnlessSomeoneElseSetItSince)
{
    device::AudioConfig config = driverConfig();
    config.analogLevelDbu = 18;
    {
        Rig rig(poweredUp(), config);
        ASSERT_TRUE(rig.configured);
        EXPECT_EQ(rig.card.routing().analogLevelDbu, 18);
        rig.card.restore();
        EXPECT_EQ(rig.card.routing().analogLevelDbu, 24);   // ours: back as it was found
    }
    {
        Rig rig(poweredUp(), config);
        ASSERT_TRUE(rig.another(routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 12; }));
        rig.card.restore();
        EXPECT_EQ(rig.card.routing().analogLevelDbu, 12);   // theirs: left
    }
    {
        // Left as the card has it, it is not touched at all.
        Rig rig;
        ASSERT_TRUE(rig.another(routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 15; }));
        rig.card.restore();
        EXPECT_EQ(rig.card.routing().analogLevelDbu, 15);
    }
}

TEST_CASE(RoutingWatch, OnlyTheXpandsLevelsAreSet)
{
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());
    device::AudioConfig config = driverConfig();
    config.analogLevelDbu = 20;
    std::string error;
    EXPECT_FALSE(card.configure(config, error));
    EXPECT_CONTAINS(error, "+24, +18, +15 or +12 dBu");

    FakeCardModel noXpand = poweredUp();
    noXpand.hasBreakoutBoard = false;
    FakeCardAudio bare(clock, noXpand);
    config.analogLevelDbu = 18;
    EXPECT_FALSE(bare.configure(config, error));
    EXPECT_CONTAINS(error, "no Xpand");
    int level = 0;
    EXPECT_FALSE(bare.readAnalogLevel(level));
}

// ---------------------------------------------------------------------------
// The task mode, when the driver sets it, and what release leaves
// ---------------------------------------------------------------------------

TEST_CASE(RoutingWatch, ATaskModeTheDriverSetIsReassertedEvenWithoutKeep)
{
    device::AudioConfig config = driverConfig();
    config.taskMode = device::TaskMode::Oem;
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());
    std::string error;
    ASSERT_TRUE(card.configure(config, error));
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    engine::RoutingWatch watch(card);
    ASSERT_TRUE(watch.takeBaseline(routing::kRouting | routing::kTaskMode, routing::kTaskMode));
    card.anotherProgramSetsTaskMode(device::TaskMode::Standard);
    const engine::RoutingWatch::Check c = watch.check(false);   // Keep my routing off
    EXPECT_EQ(c.notOurs, routing::kTaskMode);
    EXPECT_EQ(c.putBack, routing::kTaskMode);
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    // With the mode the driver's again and a look gone by, the rest of the
    // routing is not kept without keep: another program's change stays.
    EXPECT_EQ(watch.check(false).notOurs, 0u);
    ASSERT_TRUE(card.anotherProgramSets(card.routing(), 0));
    device::CardRouting theirs = card.routing();
    theirs.aes[0] = {1, 1};
    ASSERT_TRUE(card.anotherProgramSets(theirs, routing::kAesGroup));
    const engine::RoutingWatch::Check d = watch.check(false);
    EXPECT_EQ(d.notOurs, routing::kAesGroup);
    EXPECT_EQ(d.putBack, 0u);
    card.restore();
}

TEST_CASE(RoutingWatch, WhatTheServicesMovedGoesBackWithTheTaskMode)
{
    // On the KONA X, in standard task mode, AJA's services move AES out 1-8
    // and the monitor to the mixer, audio system 4, within a second (Q9).
    // A program that had reserved the card and lets it go puts the mode
    // back to standard (AJA Input does when its last instance closes): the
    // driver's outputs go to the mixer before its next look. Taking the
    // mode back alone left them there, silent, unless Keep my routing was
    // on.
    device::AudioConfig config = driverConfig();
    config.taskMode = device::TaskMode::Oem;
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());
    std::string error;
    ASSERT_TRUE(card.configure(config, error));
    engine::RoutingWatch watch(card);
    ASSERT_TRUE(watch.takeBaseline(routing::kRouting | routing::kTaskMode, routing::kTaskMode));
    ASSERT_TRUE(card.aesOutputsRouted());
    ASSERT_TRUE(card.monitorRouted());

    // The KONA X's two groups of AES outputs, and the monitor.
    const unsigned outputs = routing::kAesGroup | (routing::kAesGroup << 1) | routing::kMonitor;
    auto servicesRun = [&card] {
        device::CardRouting theirs = card.routing();
        theirs.aes[0] = {4, 1};
        theirs.aes[1] = {4, 5};
        theirs.monitor = {4, 1};
        return card.anotherProgramSets(theirs, outputs);
    };

    card.anotherProgramSetsTaskMode(device::TaskMode::Standard);
    ASSERT_TRUE(servicesRun());
    EXPECT_FALSE(card.aesOutputsRouted());
    engine::RoutingWatch::Check c = watch.check(false);   // Keep my routing off
    EXPECT_EQ(c.notOurs, routing::kTaskMode | outputs);
    EXPECT_EQ(c.putBack, routing::kTaskMode | outputs);
    EXPECT_EQ(c.putBackFailed, 0u);
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    EXPECT_TRUE(card.aesOutputsRouted());
    EXPECT_TRUE(card.monitorRouted());

    // What the services wrote while that look was putting the mode back
    // (they were in the middle of a pass) goes back at the next one.
    ASSERT_TRUE(servicesRun());
    c = watch.check(false);
    EXPECT_EQ(c.notOurs, outputs);
    EXPECT_EQ(c.putBack, outputs);
    EXPECT_TRUE(card.aesOutputsRouted());

    // After that, with the mode still the driver's, a change is another
    // program's own, as ever: reported, and left.
    ASSERT_TRUE(servicesRun());
    c = watch.check(false);
    EXPECT_EQ(c.notOurs, outputs);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_FALSE(card.aesOutputsRouted());
    card.restore();
}

TEST_CASE(RoutingWatch, WhatTheServicesWriteRightAfterAStartGoesBack)
{
    // The driver takes the card out of standard mode when it starts. AJA's
    // services, in the middle of a pass just then, may write the outputs
    // once more after the driver did: its outputs on their mixer from the
    // first block on, and nothing to say so. The first looks after such a
    // start put back what is not the driver's, Keep my routing or not.
    device::AudioConfig config = driverConfig();
    config.taskMode = device::TaskMode::Oem;
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());   // in standard mode
    std::string error;
    ASSERT_TRUE(card.configure(config, error));
    engine::RoutingWatch watch(card);
    ASSERT_TRUE(watch.takeBaseline(routing::kRouting | routing::kTaskMode, routing::kTaskMode, true));

    const unsigned outputs = routing::kAesGroup | (routing::kAesGroup << 1) | routing::kMonitor;
    auto servicesWrite = [&card] {
        device::CardRouting theirs = card.routing();
        theirs.aes[0] = {4, 1};
        theirs.aes[1] = {4, 5};
        theirs.monitor = {4, 1};
        return card.anotherProgramSets(theirs, outputs);
    };

    ASSERT_TRUE(servicesWrite());
    ASSERT_TRUE(!card.aesOutputsRouted());
    engine::RoutingWatch::Check c = watch.check(false);   // Keep my routing off
    EXPECT_EQ(c.notOurs, outputs);
    EXPECT_EQ(c.putBack, outputs);
    EXPECT_EQ(c.putBackFailed, 0u);
    EXPECT_TRUE(card.aesOutputsRouted());
    EXPECT_TRUE(card.monitorRouted());
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);

    // The second look too, and no more: from then on a change is another
    // program's own.
    ASSERT_TRUE(servicesWrite());
    c = watch.check(false);
    EXPECT_EQ(c.putBack, outputs);
    EXPECT_TRUE(card.aesOutputsRouted());
    ASSERT_TRUE(servicesWrite());
    c = watch.check(false);
    EXPECT_EQ(c.notOurs, outputs);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_TRUE(!card.aesOutputsRouted());
    card.restore();
}

TEST_CASE(RoutingWatch, AStartOverAnOemModeAlreadyThereTakesNothingBack)
{
    // The card already in OEM mode before the driver (Resolume's, AJA
    // Input's): AJA's services were not running, and what changes right
    // after the start is another program's own, as at any other time. The
    // same when the task mode is not the driver's to keep.
    device::AudioConfig config = driverConfig();
    config.taskMode = device::TaskMode::Oem;
    {
        VirtualClock clock;
        FakeCardAudio card(clock, poweredUp());
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        engine::RoutingWatch watch(card);
        ASSERT_TRUE(watch.takeBaseline(routing::kRouting | routing::kTaskMode, routing::kTaskMode, false));
        device::CardRouting theirs = card.routing();
        theirs.aes[0] = {4, 1};
        ASSERT_TRUE(card.anotherProgramSets(theirs, routing::kAesGroup));
        const engine::RoutingWatch::Check c = watch.check(false);
        EXPECT_EQ(c.notOurs, routing::kAesGroup);
        EXPECT_EQ(c.putBack, 0u);
        card.restore();
    }
    {
        VirtualClock clock;
        FakeCardAudio card(clock, poweredUp());
        std::string error;
        ASSERT_TRUE(card.configure(driverConfig(), error));   // the task mode only read
        engine::RoutingWatch watch(card);
        ASSERT_TRUE(watch.takeBaseline(routing::kRouting, 0, true));
        device::CardRouting theirs = card.routing();
        theirs.aes[0] = {4, 1};
        ASSERT_TRUE(card.anotherProgramSets(theirs, routing::kAesGroup));
        const engine::RoutingWatch::Check c = watch.check(false);
        EXPECT_EQ(c.notOurs, routing::kAesGroup);
        EXPECT_EQ(c.putBack, 0u);
        card.restore();
    }
}

TEST_CASE(RoutingWatch, ATaskModeOnlyWatchedTakesNothingBack)
{
    // Without OEM mode the task mode is not the driver's: when it changes,
    // nothing goes back with it.
    Rig rig;
    ASSERT_TRUE(rig.watching);
    rig.card.anotherProgramSetsTaskMode(device::TaskMode::Oem);
    ASSERT_TRUE(rig.another(routing::kAesGroup, [](device::CardRouting& r) { r.aes[0] = {4, 1}; }));
    const engine::RoutingWatch::Check c = rig.watch.check(false);
    EXPECT_EQ(c.changed, routing::kTaskMode | routing::kAesGroup);
    EXPECT_EQ(c.notOurs, routing::kAesGroup);
    EXPECT_EQ(c.putBack, 0u);
    EXPECT_FALSE(rig.card.aesOutputsRouted());
}

TEST_CASE(RoutingWatch, TheTaskModeGoesBackUnlessAnotherProgramHoldsTheCard)
{
    device::AudioConfig config = driverConfig();
    config.taskMode = device::TaskMode::Oem;
    {
        VirtualClock clock;
        FakeCardAudio card(clock, poweredUp());
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        card.restore();
        EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Standard);
        EXPECT_EQ(card.restoreNote(), std::string());
    }
    {
        // AJA Input reserved the card meanwhile: it is left in OEM mode, so
        // the plugin is not left to the services.
        VirtualClock clock;
        FakeCardAudio card(clock, poweredUp());
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        card.anotherProgramHoldsTheCard(true);
        card.restore();
        EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
        EXPECT_CONTAINS(card.restoreNote(), "the task mode left as it is: AJIN, process 4242, holds the card");
    }
    {
        // Not set by the driver: never touched, whoever holds the card.
        VirtualClock clock;
        FakeCardAudio card(clock, poweredUp());
        std::string error;
        ASSERT_TRUE(card.configure(driverConfig(), error));
        card.anotherProgramSetsTaskMode(device::TaskMode::Oem);
        card.restore();
        EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    }
}

TEST_CASE(RoutingWatch, AnOemModeFoundWhileTheCardWasHeldGoesToStandard)
{
    // The OEM mode a program that holds the card set is its own: found at
    // the start, it is not what to put back once that program is gone.
    device::AudioConfig config = driverConfig();
    config.taskMode = device::TaskMode::Oem;
    FakeCardModel model = poweredUp();
    model.taskMode = device::TaskMode::Oem;
    model.heldAtStart = true;
    {
        // Still held at the end: left to that program.
        VirtualClock clock;
        FakeCardAudio card(clock, model);
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        card.restore();
        EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
        EXPECT_EQ(card.restoreNote(), std::string("the task mode left as it is: AJIN, process 4242, holds the card"));
    }
    {
        // Held no longer: standard, not the OEM mode found.
        VirtualClock clock;
        FakeCardAudio card(clock, model);
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        card.anotherProgramHoldsTheCard(false);
        card.restore();
        EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Standard);
        EXPECT_EQ(card.restoreNote(),
                  std::string("the task mode put to standard, not back to OEM: AJIN, process 4242, which held the "
                              "card at the start, holds it no longer"));
    }
    {
        // An OEM mode nobody held is the card's own state: back to it.
        FakeCardModel own = model;
        own.heldAtStart = false;
        VirtualClock clock;
        FakeCardAudio card(clock, own);
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        card.restore();
        EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
        EXPECT_TRUE(card.restoreNote().empty());
    }
}

TEST_CASE(RoutingWatch, TheRestoreNoteSaysWhatStayed)
{
    Rig rig;
    ASSERT_TRUE(rig.another((routing::kAesGroup << 1) | routing::kMonitor, [](device::CardRouting& r) {
        r.aes[1] = {1, 5};
        r.monitor = {1, 3};
    }));
    rig.card.restore();
    EXPECT_EQ(rig.card.restoreNote(),
              std::string("AES out 5-8 left as another program set it; the monitor left as another program set it"));
}

namespace
{

/// The driver's set-up with SDI out 2 and the HDMI output playing too.
device::AudioConfig withVideoOutputs()
{
    device::AudioConfig config = driverConfig();
    config.sdiOutputs = 2;
    config.hdmiOutput = true;
    return config;
}

} // namespace

TEST_CASE(RoutingWatch, TheVideoOutputsAreReadOnlyOnceTheDriverSetsThem)
{
    // Not asked for: not read, so nobody's video outputs are watched.
    Rig plain;
    ASSERT_TRUE(plain.watching);
    EXPECT_EQ(device::routingParts(plain.watch.baseline()) & (routing::kSdiOuts | routing::kHdmiOut), 0u);
    ASSERT_TRUE(plain.another(routing::kSdiOut, [](device::CardRouting& r) { r.sdiOut[0] = {2, false}; }));
    EXPECT_EQ(plain.watch.check(false).changed, 0u);

    Rig rig(poweredUp(), withVideoOutputs());
    ASSERT_TRUE(rig.watching);
    const device::CardRouting& ours = rig.watch.baseline();
    EXPECT_EQ(device::routingParts(ours) & (routing::kSdiOuts | routing::kHdmiOut),
              (routing::kSdiOut << 1) | routing::kHdmiOut);
    EXPECT_TRUE(ours.sdiOut[1] == (device::SdiOutputAudio{2, true}));
    EXPECT_TRUE(ours.hdmiOut == (device::HdmiOutputAudio{2, 8, 1}));
    EXPECT_CONTAINS(engine::describeRouting(ours),
                    "monitor: audio system 2, channels 1-2; SDI out 2: audio system 2; HDMI out: audio system 2, "
                    "channels 1-8; task mode: standard");
    EXPECT_TRUE(rig.card.routing().sdiOut[0] == (device::SdiOutputAudio{1, true}));   // SDI out 1: untouched
}

TEST_CASE(RoutingWatch, AVideoOutputTakenIsReportedAndPutBackWhenKept)
{
    Rig rig(poweredUp(), withVideoOutputs());
    ASSERT_TRUE(rig.watching);
    ASSERT_TRUE(rig.another((routing::kSdiOut << 1) | routing::kHdmiOut, [](device::CardRouting& r) {
        r.sdiOut[1] = {1, true};
        r.hdmiOut = {1, 2, 1};
    }));
    engine::RoutingWatch::Check c = rig.watch.check(false);
    EXPECT_EQ(c.changed, (routing::kSdiOut << 1) | routing::kHdmiOut);
    EXPECT_EQ(c.notOurs, (routing::kSdiOut << 1) | routing::kHdmiOut);
    EXPECT_EQ(c.putBack, 0u);
    std::vector<std::string> lines;
    engine::reportCheck(rig.watch, c, "Keep my routing",
                        [](void* context, const char* line) {
                            static_cast<std::vector<std::string>*>(context)->push_back(line);
                        },
                        &lines);
    ASSERT_EQ(lines.size(), std::size_t(2));
    EXPECT_EQ(lines[0], std::string("routing: another program changed SDI out 2 to audio system 1; the driver's is "
                                    "audio system 2; left as it is (Keep my routing is off)"));
    EXPECT_EQ(lines[1], std::string("routing: another program changed HDMI out to audio system 1, channels 1-2; the "
                                    "driver's is audio system 2, channels 1-8; left as it is (Keep my routing is "
                                    "off)"));

    c = rig.watch.check(true);
    EXPECT_EQ(c.putBack, (routing::kSdiOut << 1) | routing::kHdmiOut);
    EXPECT_EQ(c.putBackFailed, 0u);
    EXPECT_TRUE(rig.card.routing().sdiOut[1] == (device::SdiOutputAudio{2, true}));
    EXPECT_TRUE(rig.card.routing().hdmiOut == (device::HdmiOutputAudio{2, 8, 1}));
}

TEST_CASE(RoutingWatch, TheVideoOutputsGoBackUnlessTakenSince)
{
    device::AudioConfig config = withVideoOutputs();
    config.sdiOutputs = 3;   // SDI out 1 and 2
    Rig rig(poweredUp(), config);
    ASSERT_TRUE(rig.configured);
    ASSERT_TRUE(rig.another(routing::kSdiOut, [](device::CardRouting& r) { r.sdiOut[0] = {1, false}; }));
    rig.card.restore();
    const device::CardRouting& r = rig.card.routing();
    EXPECT_TRUE(r.sdiOut[0] == (device::SdiOutputAudio{1, false}));   // theirs: left
    EXPECT_TRUE(r.sdiOut[1] == (device::SdiOutputAudio{1, true}));    // ours: as found
    EXPECT_TRUE(r.hdmiOut == (device::HdmiOutputAudio{1, 8, 1}));     // ours: as found
    EXPECT_EQ(rig.card.restoreNote(), std::string("SDI out 1 left as another program set it"));
}

TEST_CASE(RoutingWatch, VideoOutputsTheCardDoesNotHaveAreRefused)
{
    VirtualClock clock;
    FakeCardAudio card(clock, poweredUp());
    device::AudioConfig config = driverConfig();
    config.sdiOutputs = 4;   // SDI out 3
    std::string error;
    EXPECT_FALSE(card.configure(config, error));
    EXPECT_EQ(error, std::string("the card has 2 SDI outputs"));

    FakeCardModel noHdmi = poweredUp();
    noHdmi.hdmiOutputs = 0;
    FakeCardAudio bare(clock, noHdmi);
    device::AudioConfig hdmi = driverConfig();
    hdmi.hdmiOutput = true;
    EXPECT_FALSE(bare.configure(hdmi, error));
    EXPECT_EQ(error, std::string("the card has no HDMI output"));

    // Values the card cannot hold are refused too.
    Rig rig(poweredUp(), withVideoOutputs());
    device::CardRouting wrong = rig.card.routing();
    wrong.hdmiOut = {2, 8, 5};
    EXPECT_FALSE(rig.card.writeRouting(wrong, routing::kHdmiOut));
    wrong.sdiOut[1] = {0, true};
    EXPECT_FALSE(rig.card.writeRouting(wrong, routing::kSdiOut << 1));
    EXPECT_TRUE(rig.card.routing().hdmiOut == (device::HdmiOutputAudio{2, 8, 1}));
}
