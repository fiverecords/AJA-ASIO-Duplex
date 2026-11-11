// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// duplex-probe loop: the test signal, the verifier, and whole runs of the
// engine on the simulated card with AES out looped to AES in.

#include "test_framework.h"

#include "LoopProbe.h"
#include "sim/FakeCardAudio.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace ajaduplex;
using namespace ajaduplex::probe;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

/// Feeds `count` frames of output channel `channel` starting at output frame
/// `firstOut`, arriving at input frame `firstIn` onwards.
void feedRun(ChannelVerifier& v, std::int64_t firstIn, std::int64_t firstOut, std::int64_t count, unsigned channel)
{
    for (std::int64_t i = 0; i < count; ++i)
        v.feed(firstIn + i, LoopSignal::encode(firstOut + i, channel));
}

void feedSilence(ChannelVerifier& v, std::int64_t firstIn, std::int64_t count)
{
    for (std::int64_t i = 0; i < count; ++i)
        v.feed(firstIn + i, 0);
}

/// The counter, word by word: the simulated card's AES inputs do not
/// resample unless told to.
LoopSettings settings(unsigned frames, unsigned guard, double seconds)
{
    LoopSettings s;
    s.frames = frames;
    s.guard = guard;
    s.seconds = seconds;
    s.path = LoopPath::AesWords;
    return s;
}

bool never()
{
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// The signal
// ---------------------------------------------------------------------------

TEST_CASE(LoopSignal, EncodeDecodeRoundTrip)
{
    for (unsigned channel = 0; channel < LoopSignal::kChannels; ++channel)
    {
        for (std::int64_t frame = 0; frame < 3 * std::int64_t(LoopSignal::kCounterSpan); frame += 7)
        {
            const std::int32_t word = LoopSignal::encode(frame, channel);
            const LoopSignal::Decoded d = LoopSignal::decode(word);
            if (!d.valid || d.silent || d.channel != channel ||
                d.counter != static_cast<std::uint32_t>(frame % LoopSignal::kCounterSpan))
            {
                EXPECT_TRUE(false);
                return;
            }
        }
    }
}

TEST_CASE(LoopSignal, QuietAndInTheTop24Bits)
{
    std::int64_t peak = 0;
    for (unsigned channel = 0; channel < LoopSignal::kChannels; ++channel)
    {
        for (std::int64_t frame = 0; frame < std::int64_t(LoopSignal::kCounterSpan); ++frame)
        {
            const std::int32_t word = LoopSignal::encode(frame, channel);
            EXPECT_NE(word, 0);
            EXPECT_EQ(word & 0xFF, 0);   // nothing in the byte AES drops
            peak = std::max<std::int64_t>(peak, std::llabs(static_cast<long long>(word)));
        }
    }
    const double dbfs = 20.0 * std::log10(static_cast<double>(peak) / 2147483648.0);
    EXPECT_TRUE(dbfs <= -48.0);
    EXPECT_TRUE(dbfs > -49.0);
    EXPECT_TRUE(std::fabs(dbfs - LoopSignal::kPeakDbfs) < 0.1);
}

TEST_CASE(LoopSignal, LowByteIgnoredAndOtherWordsRejected)
{
    const std::int32_t word = LoopSignal::encode(1234, 5);
    const LoopSignal::Decoded withLowByte = LoopSignal::decode(word | 0x5A);
    EXPECT_TRUE(withLowByte.valid);
    EXPECT_EQ(withLowByte.counter, 1234u);
    EXPECT_EQ(withLowByte.channel, 5u);

    const LoopSignal::Decoded zero = LoopSignal::decode(0x3C);   // silence, low byte aside
    EXPECT_TRUE(zero.silent);
    EXPECT_FALSE(zero.valid);

    const LoopSignal::Decoded loud = LoopSignal::decode(0x40000000);   // not sign-extended 16 bits
    EXPECT_FALSE(loud.valid);
    EXPECT_FALSE(loud.silent);

    const LoopSignal::Decoded even = LoopSignal::decode(2 * 256);   // marker bit clear
    EXPECT_FALSE(even.valid);
    EXPECT_FALSE(even.silent);
}

// ---------------------------------------------------------------------------
// The verifier
// ---------------------------------------------------------------------------

TEST_CASE(ChannelVerifier, CleanStreamAcrossTheCounterWrap)
{
    ChannelVerifier v(2);
    feedSilence(v, 0, 300);                      // before the first block comes round
    feedRun(v, 300, 0, 3 * 4096 + 100, 2);       // round trip 300, wraps three times
    EXPECT_TRUE(v.locked());
    EXPECT_TRUE(v.clean());
    EXPECT_EQ(v.good, std::uint64_t(3 * 4096 + 100));
    EXPECT_EQ(v.silent, 0u);
    EXPECT_EQ(v.roundTrip, 300);
    EXPECT_EQ(v.minRoundTrip, 300);
    EXPECT_EQ(v.maxRoundTrip, 300);
    EXPECT_EQ(v.roundTripChanges, 0u);
}

TEST_CASE(ChannelVerifier, LongRoundTripIsExact)
{
    ChannelVerifier v(0);
    feedSilence(v, 0, 3000);
    feedRun(v, 3000, 0, 5000, 0);
    EXPECT_TRUE(v.clean());
    EXPECT_EQ(v.roundTrip, 3000);
}

TEST_CASE(ChannelVerifier, LostFrames)
{
    ChannelVerifier v(0);
    feedRun(v, 200, 0, 1000, 0);
    feedRun(v, 1200, 1005, 500, 0);   // output frames 1000-1004 never arrive
    EXPECT_FALSE(v.clean());
    EXPECT_EQ(v.lost, 5u);
    EXPECT_EQ(v.jumps, 1u);
    EXPECT_EQ(v.minRoundTrip, 195);
    EXPECT_EQ(v.maxRoundTrip, 200);
    EXPECT_EQ(v.roundTripChanges, 1u);
}

TEST_CASE(ChannelVerifier, RepeatedFrame)
{
    ChannelVerifier v(1);
    feedRun(v, 100, 0, 50, 1);
    feedRun(v, 150, 49, 50, 1);   // frame 49 twice
    EXPECT_FALSE(v.clean());
    EXPECT_EQ(v.repeated, 1u);
    EXPECT_EQ(v.lost, 0u);
    EXPECT_EQ(v.maxRoundTrip, 101);
}

TEST_CASE(ChannelVerifier, JumpBack)
{
    ChannelVerifier v(0);
    feedRun(v, 100, 0, 500, 0);
    feedRun(v, 600, 300, 500, 0);   // 200 frames again, from 300
    EXPECT_FALSE(v.clean());
    EXPECT_EQ(v.jumpsBack, 1u);
    EXPECT_EQ(v.repeated, 0u);
    EXPECT_EQ(v.lost, 0u);
    EXPECT_EQ(v.maxRoundTrip, 300);
}

TEST_CASE(ChannelVerifier, SilenceAfterLockMovesTheRoundTrip)
{
    // A late block: the card plays erased frames, then the same stream later.
    ChannelVerifier v(0);
    feedRun(v, 300, 0, 1000, 0);
    feedSilence(v, 1300, 160);
    feedRun(v, 1460, 1000, 1000, 0);
    EXPECT_FALSE(v.clean());
    EXPECT_EQ(v.silent, 160u);
    EXPECT_EQ(v.lost, 0u);
    EXPECT_EQ(v.minRoundTrip, 300);
    EXPECT_EQ(v.maxRoundTrip, 460);
    EXPECT_EQ(v.roundTrip, 460);
}

TEST_CASE(ChannelVerifier, WrongChannelAndGarbage)
{
    ChannelVerifier crossed(0);
    feedRun(crossed, 100, 0, 500, 3);   // hears AES 4
    EXPECT_FALSE(crossed.locked());
    EXPECT_FALSE(crossed.clean());
    EXPECT_EQ(crossed.wrongChannel, 500u);
    EXPECT_EQ(crossed.heardChannel, 3);

    ChannelVerifier noisy(0);
    for (int i = 0; i < 100; ++i)
        noisy.feed(i, 0x12345600 + i * 0x10000);   // full-scale words
    EXPECT_FALSE(noisy.locked());
    EXPECT_EQ(noisy.garbage, 100u);
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------

TEST_CASE(LoopProbe, SweepGrid)
{
    const auto all = sweepGrid(0, -1);
    ASSERT_EQ(all.size(), std::size_t(16));
    EXPECT_EQ(all.front().first, 256u);
    EXPECT_EQ(all.front().second, 64u);
    EXPECT_EQ(all.back().first, 32u);
    EXPECT_EQ(all.back().second, 8u);

    const auto onlyN = sweepGrid(128, -1);
    ASSERT_EQ(onlyN.size(), std::size_t(4));
    EXPECT_EQ(onlyN[0].first, 128u);
    EXPECT_EQ(onlyN[3].second, 8u);

    const auto onlyG = sweepGrid(0, 0);   // G 0 is a value, not "sweep"
    ASSERT_EQ(onlyG.size(), std::size_t(4));
    EXPECT_EQ(onlyG[2].first, 64u);
    EXPECT_EQ(onlyG[2].second, 0u);

    const auto one = sweepGrid(96, 24);   // not on the grid: still run
    ASSERT_EQ(one.size(), std::size_t(1));
    EXPECT_EQ(one[0].first, 96u);
    EXPECT_EQ(one[0].second, 24u);
}

// ---------------------------------------------------------------------------
// Runs on the simulated card
// ---------------------------------------------------------------------------

TEST_CASE(LoopProbe, CleanRunOnTheSimulatedCard)
{
    VirtualClock clock;
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.aesOutputsOnThisSystem = false;   // the loop has to route them
    FakeCardAudio card(clock, model);

    LoopSettings s = settings(128, 32, 3.0);
    s.channels = {0, 1, 6, 7};
    const LoopResult r = runLoop(card, clock, s, never);
    ASSERT_TRUE(r.started);
    EXPECT_TRUE(r.clean());
    EXPECT_FALSE(r.interrupted);
    EXPECT_TRUE(r.seconds >= 3.0 && r.seconds < 3.1);
    EXPECT_TRUE(r.engine.blocks > 1100);
    ASSERT_EQ(r.channels.size(), std::size_t(4));
    for (const ChannelVerifier& c : r.channels)
    {
        EXPECT_TRUE(c.locked());
        EXPECT_EQ(c.roundTripChanges, 0u);
        EXPECT_EQ(c.minRoundTrip, r.channels.front().minRoundTrip);
        // At least N + S, plus what the card and cable add.
        EXPECT_TRUE(c.minRoundTrip >= 128 + 160 + 12);
        EXPECT_TRUE(c.good > 100000);
    }
    EXPECT_EQ(r.events, 0u);
    // The simulated card runs 20 ppm fast; the run dates it to a few ppm.
    EXPECT_TRUE(r.clockMeasured);
    EXPECT_TRUE(r.clockPpm > 15.0 && r.clockPpm < 25.0);
    EXPECT_TRUE(card.aesOutputsRouted());
    card.restore();
    EXPECT_FALSE(card.aesOutputsRouted());   // put back as it was
}

TEST_CASE(LoopProbe, CrossedCablesAreNamed)
{
    VirtualClock clock;
    FakeCardModel model;
    model.cable = {FakeCardModel::Patch{0, 1}, FakeCardModel::Patch{1, 0}};   // out 1 -> in 2, out 2 -> in 1
    FakeCardAudio card(clock, model);

    const LoopResult r = runLoop(card, clock, settings(128, 32, 1.0), never);
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.clean());
    ASSERT_EQ(r.channels.size(), std::size_t(2));
    EXPECT_EQ(r.channels[0].heardChannel, 1);
    EXPECT_EQ(r.channels[1].heardChannel, 0);
    EXPECT_CONTAINS(loopSummaryLine(r), "AES 1 in hears AES 2 out: crossed cables");
}

TEST_CASE(LoopProbe, MissingCableIsNamed)
{
    VirtualClock clock;
    FakeCardModel model;
    model.cable = {FakeCardModel::Patch{0, 0}};   // only AES 1 is cabled
    FakeCardAudio card(clock, model);

    const LoopResult r = runLoop(card, clock, settings(64, 16, 1.0), never);
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.clean());
    EXPECT_TRUE(r.channels[0].locked());
    EXPECT_TRUE(r.channels[0].clean());
    EXPECT_FALSE(r.channels[1].locked());
    EXPECT_CONTAINS(loopSummaryLine(r), "AES 2 in never heard AES 2 out: check that cable");
}

TEST_CASE(LoopProbe, LateWakeUpsBreakSmallGuardsOnly)
{
    FakeCardModel model = FakeCardModel::withAesLoop();

    VirtualClock tight;
    tight.setSleepLateness(sim::SleepLateness{20000, 0.002, 2000000, 11});
    FakeCardAudio tightCard(tight, model);
    const LoopResult small = runLoop(tightCard, tight, settings(32, 8, 2.0), never);
    ASSERT_TRUE(small.started);
    EXPECT_FALSE(small.clean());
    EXPECT_TRUE(small.engine.lateOutputs > 0);
    EXPECT_TRUE(small.silent() > 0);         // erased frames played
    EXPECT_TRUE(small.lost() > 0);
    // Late outputs cost frames, not latency (D1): one round trip throughout.
    EXPECT_EQ(small.channels[0].roundTripChanges, 0u);
    EXPECT_EQ(small.channels[0].maxRoundTrip, small.channels[0].minRoundTrip);
    EXPECT_EQ(small.engine.outputMoves, 0u);
    EXPECT_TRUE(small.engine.droppedOutputFrames > 0);
    EXPECT_CONTAINS(loopSummaryLine(small), " frames dropped)");
    EXPECT_FALSE(small.firstEvents.empty());
    EXPECT_EQ(small.firstEvents.front().event, engine::EngineEvent::LateOutput);

    VirtualClock loose;
    loose.setSleepLateness(sim::SleepLateness{20000, 0.002, 2000000, 11});
    FakeCardAudio looseCard(loose, model);
    const LoopResult large = runLoop(looseCard, loose, settings(256, 64, 2.0), never);
    ASSERT_TRUE(large.started);
    EXPECT_TRUE(large.clean());
}

TEST_CASE(LoopProbe, ProgressFollowsTheRun)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    LoopProgress progress;
    const LoopResult clean = runLoop(card, clock, settings(128, 32, 1.0), never, &progress);
    ASSERT_TRUE(clean.clean());
    EXPECT_EQ(progress.blocks.load(), clean.engine.blocks);
    EXPECT_EQ(progress.problems.load(), 0u);
    EXPECT_TRUE(progress.allLocked.load());

    FakeCardModel half;
    half.cable = {FakeCardModel::Patch{0, 0}};   // AES 2 not cabled
    VirtualClock clock2;
    FakeCardAudio card2(clock2, half);
    LoopProgress progress2;
    runLoop(card2, clock2, settings(128, 32, 1.0), never, &progress2);
    EXPECT_FALSE(progress2.allLocked.load());
    EXPECT_EQ(progress2.problems.load(), 0u);   // nothing wrong yet, just nothing heard

    VirtualClock late;
    late.setSleepLateness(sim::SleepLateness{20000, 0.002, 2000000, 11});
    FakeCardAudio card3(late, FakeCardModel::withAesLoop());
    LoopProgress progress3;
    const LoopResult failed = runLoop(card3, late, settings(32, 8, 2.0), never, &progress3);
    EXPECT_FALSE(failed.clean());
    EXPECT_TRUE(progress3.problems.load() > 0);
}

TEST_CASE(LoopProbe, StopRequestEndsTheRun)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    int calls = 0;
    const LoopResult r = runLoop(card, clock, settings(128, 32, 60.0), [&calls] { return ++calls > 200; });
    ASSERT_TRUE(r.started);
    EXPECT_TRUE(r.interrupted);
    EXPECT_FALSE(r.clean());
    EXPECT_TRUE(r.seconds < 2.0);
}

TEST_CASE(LoopProbe, ChannelsOutsideAesAreRefused)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    LoopSettings s = settings(128, 32, 1.0);
    s.channels = {0, 8};
    const LoopResult r = runLoop(card, clock, s, never);
    EXPECT_FALSE(r.started);
    EXPECT_CONTAINS(r.error, "AES channels are 1-8");
}

TEST_CASE(LoopProbe, ADmaErrorSaysWhichDma)
{
    // The sweep on the KONA X (2026-11-10) had DMA errors once a lap at N =
    // 64 and its report said no more than when: now it says which DMA, and
    // how a second try from a spare buffer went.
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    card.failPlaybackWritesAt(0, 87, 1);   // after the start's zeroing: what wraps at the ring's end, every lap
    LoopRun run;
    run.results.push_back(runLoop(card, clock, settings(128, 256, 3.0), never));
    const LoopResult& r = run.results.front();
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.clean());
    EXPECT_TRUE(r.engine.dmaErrors >= 1);
    EXPECT_EQ(r.engine.dmaRecovered, std::uint64_t(0));
    EXPECT_TRUE(r.retired.empty());

    Report report;
    addLoopReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "DMA error at 1.");
    EXPECT_CONTAINS(text, "(output, the part of the block past the ring's end, at the ring's start: ");
    EXPECT_CONTAINS(text, " bytes at ring offset 0; system error 87; again from the same buffer: failed, system "
                          "error 87; from a spare buffer: failed, system error 87)");
    EXPECT_CONTAINS(text, "at the ring's end: output ");
    EXPECT_CONTAINS(text, "one DMA running on past the end");
    EXPECT_CONTAINS(text, "OEM while each run lasts");
}

TEST_CASE(LoopProbe, ABadBufferIsSavedByASpare)
{
    // A DMA that fails because of its host buffer goes through from a
    // spare: the run stays clean, and the report says so and what the
    // buffer did when tried again at stop.
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    card.failDmasThroughBufferAt(device::Ring::Playback, 0x3FC000u - 64u, 31, 1);   // after the start's zeroing
    LoopRun run;
    run.results.push_back(runLoop(card, clock, settings(64, 32, 3.0), never));
    const LoopResult& r = run.results.front();
    ASSERT_TRUE(r.started);
    EXPECT_TRUE(r.clean());
    EXPECT_EQ(r.engine.dmaErrors, std::uint64_t(0));
    EXPECT_EQ(r.engine.dmaRecovered, std::uint64_t(1));
    ASSERT_EQ(r.retired.size(), std::size_t(1));
    EXPECT_TRUE(r.retired[0].checked);
    EXPECT_TRUE(r.retired[0].failsAgain);
    EXPECT_EQ(r.retired[0].againSystemError, 31u);

    Report report;
    addLoopReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "CLEAN over 3.0 s");
    EXPECT_CONTAINS(text, "1 DMA errors saved by a second try from a spare buffer (nothing lost)");
    EXPECT_CONTAINS(text, "DMA recovered at 1.");
    EXPECT_CONTAINS(text, "again from the same buffer: failed, system error 31; from a spare buffer: went "
                          "through; nothing lost)");
    EXPECT_CONTAINS(text, "failed again at stop (system error 31), the card stopped: the buffer is bad");
}

TEST_CASE(LoopProbe, TheCardIsInOemTaskModeWhileItRuns)
{
    // The sweep of 2026-11-10 ran in standard task mode, and AJA's services
    // re-routed the AES outputs to the mixer meanwhile: runs hold OEM now,
    // and restore() puts the card's mode back.
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    ASSERT_TRUE(card.routing().taskMode == device::TaskMode::Standard);
    const LoopResult r = runLoop(card, clock, settings(128, 32, 1.0), never);
    ASSERT_TRUE(r.started);
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    card.restore();
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Standard);

    FakeCardAudio other(clock, FakeCardModel::withAesLoop());
    LoopSettings keep = settings(128, 32, 1.0);
    keep.oemTasks = false;
    ASSERT_TRUE(runLoop(other, clock, keep, never).started);
    EXPECT_TRUE(other.routing().taskMode == device::TaskMode::Standard);
}

TEST_CASE(LoopProbe, ReportAndCsv)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    LoopRun run;
    run.description = card.description();
    run.results.push_back(runLoop(card, clock, settings(128, 32, 1.0), never));
    run.results.push_back(runLoop(card, clock, settings(128, 16, 1.0), never));
    LoopResult failed = runLoop(card, clock, settings(64, 8, 1.0), never);
    failed.channels[0].lost = 3;   // as if three frames went missing
    run.results.push_back(failed);

    Report report;
    addLoopReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "[loop]");
    EXPECT_CONTAINS(text, "AES 1, 2, each out to the same in");
    EXPECT_CONTAINS(text, "peak -48.2 dBFS");
    EXPECT_CONTAINS(text, "N 128, G 32 (S 160)");
    EXPECT_CONTAINS(text, "CLEAN over 1.0 s");
    EXPECT_CONTAINS(text, "FAILED over 1.0 s");
    EXPECT_CONTAINS(text, "3 frames lost");
    EXPECT_CONTAINS(text, "card and cable");
    EXPECT_CONTAINS(text, "[verdict]");
    EXPECT_CONTAINS(text, "clean from G 16");
    EXPECT_CONTAINS(text, "not clean with any G tried");
    EXPECT_CONTAINS(text, "N = 128 with G = 16");
    EXPECT_CONTAINS(text, "duplex-probe loop --path aes-words --frames 128 --guard 256 --seconds 3600");
    // The hour at the driver's own guard, not at the smallest one that held for a second.
    EXPECT_CONTAINS(text, "the driver's own guard");

    const std::string csv = loopCsv(run);
    EXPECT_TRUE(csv.rfind("frames,guard,lead,seconds,", 0) == 0);
    std::size_t lines = 0;
    for (const char c : csv)
        lines += c == '\n';
    EXPECT_EQ(lines, std::size_t(4));
    EXPECT_CONTAINS(csv, "\n128,32,160,");
    EXPECT_CONTAINS(csv, ",1\n");   // clean
    EXPECT_CONTAINS(csv, ",0\n");   // the failed one
}

TEST_CASE(LoopProbe, VerdictDoesNotDrawALineThroughNoise)
{
    // G 8 came out clean but G 16 did not: the runs were too short.
    LoopRun run;
    for (const unsigned guard : {16u, 8u})
    {
        LoopResult r;
        r.settings = settings(128, guard, 10.0);
        r.started = true;
        r.seconds = 10.0;
        r.channels.emplace_back(0);
        feedRun(r.channels.back(), 300, 0, 1000, 0);
        if (guard == 16)
            r.engine.lateOutputs = 1;
        run.results.push_back(r);
    }
    Report report;
    addLoopReport(run, report);
    EXPECT_CONTAINS(report.render(true), "clean from G 8, but not with G 16: too rare for runs this short");
}

TEST_CASE(LoopProbe, ExitTestNeedsAnHourAt128)
{
    LoopRun run;
    LoopResult r;
    r.settings = settings(128, 16, 3600.0);
    r.started = true;
    r.seconds = 3600.2;
    r.channels.emplace_back(0);
    feedRun(r.channels.back(), 300, 0, 1000, 0);
    run.results.push_back(r);

    Report report;
    addLoopReport(run, report);
    EXPECT_CONTAINS(report.render(true), "PASSED");

    run.results.front().seconds = 1200.0;   // only 20 minutes
    Report shorter;
    addLoopReport(run, shorter);
    EXPECT_CONTAINS(shorter.render(true), "not run yet");
}
