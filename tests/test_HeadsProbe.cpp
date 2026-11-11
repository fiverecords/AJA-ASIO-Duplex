// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// duplex-probe heads, run end to end against the simulated card. Each
// variant of the card model is a case the real KONA X could turn out to
// be; the analysis has to name it correctly.

#include "test_framework.h"

#include "HeadsProbe.h"
#include "sim/FakeCardAudio.h"

#include <cmath>

using namespace ajaduplex;
using namespace ajaduplex::probe;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

HeadsSettings quickSettings()
{
    HeadsSettings s;
    s.sampleSeconds = 2.0;
    s.eventSeconds = 2.0;   // longer than one lap, so a wrap event always fits
    s.sentinelTrials = 500;
    return s;
}

struct Outcome
{
    bool ok = false;
    std::string error;
    HeadsRun run;
};

Outcome runOn(const FakeCardModel& model, const std::function<bool()>& stop = {})
{
    VirtualClock clock;
    FakeCardAudio card(clock, model);
    Outcome out;
    out.ok = runHeads(card, clock, quickSettings(), stop, out.run, out.error);
    return out;
}

bool near(double value, double expected, double tolerance)
{
    return std::fabs(value - expected) <= tolerance;
}

} // namespace

TEST_CASE(HeadsProbe, DefaultModelEndToEnd)
{
    const Outcome o = runOn(FakeCardModel{});
    ASSERT_TRUE(o.ok);
    const HeadsRun& run = o.run;
    EXPECT_FALSE(run.interrupted);
    EXPECT_TRUE(run.notes.empty());
    EXPECT_EQ(run.layout.frameBytes, 64u);
    EXPECT_TRUE(run.samples.size() > 1000);

    const HeadMotion in = analyzeHead(run.samples, true, run.layout);
    ASSERT_TRUE(!in.steps.empty());
    EXPECT_EQ(in.steps.front().first, 64u);            // one frame per step
    ASSERT_TRUE(!in.phases.empty());
    EXPECT_EQ(in.phases.front().first, 63u);           // last byte of a frame
    EXPECT_EQ(in.phases.front().second, in.changes);
    EXPECT_TRUE(near(in.framesPerSecond, 48000.96, 0.2));
    EXPECT_TRUE(near(in.ppm, 20.0, 4.0));
    EXPECT_EQ(in.backwardSteps, std::size_t(0));       // wraps count as forward steps
}

TEST_CASE(HeadsProbe, OutputHeadStepsInChunks)
{
    const Outcome o = runOn(FakeCardModel{});
    ASSERT_TRUE(o.ok);
    const HeadMotion out = analyzeHead(o.run.samples, false, o.run.layout);
    ASSERT_TRUE(!out.steps.empty());
    EXPECT_EQ(out.steps.front().first, 16u * 64u);     // 16-frame fetches
    EXPECT_EQ(out.phases.front().first, 0u);           // next byte
    EXPECT_TRUE(near(out.framesPerSecond, 48000.96, 1.0));
}

TEST_CASE(HeadsProbe, CounterOnTheAudioClock)
{
    const Outcome o = runOn(FakeCardModel{});
    ASSERT_TRUE(o.ok);
    const CounterMotion c = analyzeCounter(o.run.samples, o.run.layout);
    ASSERT_TRUE(c.valid);
    EXPECT_TRUE(near(c.ticksPerSecond, 48000.96, 0.2));
    EXPECT_TRUE(near(c.inputFramesPerTick, 1.0, 5e-6));
}

TEST_CASE(HeadsProbe, CounterOnAnotherClockShowsUp)
{
    FakeCardModel model;
    model.counterOnAudioClock = false;
    model.counterPpm = -30.0;   // audio clock is +20: 50 ppm apart
    const Outcome o = runOn(model);
    ASSERT_TRUE(o.ok);
    const CounterMotion c = analyzeCounter(o.run.samples, o.run.layout);
    ASSERT_TRUE(c.valid);
    EXPECT_TRUE(near((c.inputFramesPerTick - 1.0) * 1e6, 50.0, 5.0));
}

TEST_CASE(HeadsProbe, LastByteHeadIsRecognised)
{
    const Outcome o = runOn(FakeCardModel{});
    ASSERT_TRUE(o.ok);
    const SentinelSummary s = summarizeSentinel(o.run.trials);
    EXPECT_TRUE(s.trials > 100);
    EXPECT_EQ(s.atHeadSentinel, std::size_t(0));
    EXPECT_TRUE(s.afterHeadSentinel > 0);
    EXPECT_CONTAINS(s.conclusion, "LAST byte");
}

TEST_CASE(HeadsProbe, NextByteHeadIsRecognised)
{
    FakeCardModel model;
    model.inputHeadAtLastByte = false;
    const Outcome o = runOn(model);
    ASSERT_TRUE(o.ok);
    const SentinelSummary s = summarizeSentinel(o.run.trials);
    EXPECT_TRUE(s.atHeadSentinel > 0);
    EXPECT_CONTAINS(s.conclusion, "NEXT byte");
    const HeadMotion in = analyzeHead(o.run.samples, true, o.run.layout);
    EXPECT_EQ(in.phases.front().first, 0u);
}

TEST_CASE(HeadsProbe, ChunkedInputHead)
{
    FakeCardModel model;
    model.inputChunkFrames = 8;
    const Outcome o = runOn(model);
    ASSERT_TRUE(o.ok);
    const HeadMotion in = analyzeHead(o.run.samples, true, o.run.layout);
    EXPECT_EQ(in.steps.front().first, 8u * 64u);
    EXPECT_TRUE(near(in.framesPerSecond, 48000.96, 2.0));
}

TEST_CASE(HeadsProbe, EraseModeVerdicts)
{
    const Outcome works = runOn(FakeCardModel{});
    ASSERT_TRUE(works.ok);
    EXPECT_TRUE(works.run.erase.done);
    EXPECT_CONTAINS(eraseConclusion(works.run.erase), "erase mode works");

    FakeCardModel broken;
    broken.eraseSupported = false;
    const Outcome fails = runOn(broken);
    ASSERT_TRUE(fails.ok);
    EXPECT_CONTAINS(eraseConclusion(fails.run.erase), "did NOT zero");
}

TEST_CASE(HeadsProbe, CaptureEnableVerdicts)
{
    const Outcome gated = runOn(FakeCardModel{});
    ASSERT_TRUE(gated.ok);
    EXPECT_TRUE(gated.run.captureEnable.done);
    EXPECT_TRUE(gated.run.captureEnable.headMoved);
    EXPECT_EQ(gated.run.captureEnable.written, 0u);
    EXPECT_CONTAINS(captureEnableConclusion(gated.run.captureEnable), "must call SetAudioCaptureEnable(true)");

    FakeCardModel model;
    model.captureEnableGatesWrites = false;
    const Outcome free = runOn(model);
    ASSERT_TRUE(free.ok);
    EXPECT_CONTAINS(captureEnableConclusion(free.run.captureEnable), "without capture enable");
}

TEST_CASE(HeadsProbe, Events)
{
    const Outcome o = runOn(FakeCardModel{});
    ASSERT_TRUE(o.ok);
    ASSERT_EQ(o.run.events.size(), std::size_t(3));
    EXPECT_FALSE(o.run.events[0].subscribed);           // eAudio is not simulated
    EXPECT_TRUE(o.run.events[1].subscribed);
    EXPECT_TRUE(o.run.events[1].arrived >= 1);
    EXPECT_TRUE(o.run.events[1].haveCounts);
    EXPECT_EQ(o.run.events[1].countAfter - o.run.events[1].countBefore, o.run.events[1].arrived);
    EXPECT_TRUE(o.run.events[2].arrived >= 1);
}

TEST_CASE(HeadsProbe, StopRequestEndsTheRunEarly)
{
    int calls = 0;
    const Outcome o = runOn(FakeCardModel{}, [&calls] { return ++calls > 5000; });
    ASSERT_TRUE(o.ok);
    EXPECT_TRUE(o.run.interrupted);
    EXPECT_TRUE(o.run.events.empty());          // later steps are skipped
    EXPECT_FALSE(o.run.captureEnable.done);
}

TEST_CASE(HeadsProbe, ReportAndCsv)
{
    const Outcome o = runOn(FakeCardModel{});
    ASSERT_TRUE(o.ok);
    Report report;
    addHeadsReport(o.run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "[input head (LastIn)]");
    EXPECT_CONTAINS(text, "64 bytes (1 frame)");
    EXPECT_CONTAINS(text, "[last byte or next]");
    EXPECT_CONTAINS(text, "LAST byte");
    EXPECT_CONTAINS(text, "[erase mode]");
    EXPECT_CONTAINS(text, "eAudioInWrap");

    const std::string csv = headsCsv(o.run);
    EXPECT_EQ(csv.rfind("t_ns,last_in,last_out,counter\n", 0), std::size_t(0));
    std::size_t lines = 0;
    for (const char ch : csv)
        lines += ch == '\n';
    EXPECT_EQ(lines, o.run.samples.size() + 1);
}

TEST_CASE(HeadsAnalysis, StepsAcrossTheWrap)
{
    device::RingLayout layout;
    layout.ringBytes = 0x3FC000;
    layout.frameBytes = 64;
    layout.captureOffset = 0x400000;
    std::vector<HeadSample> samples;
    std::uint32_t value = layout.ringBytes - 64 * 3 - 1;
    for (int i = 0; i < 10; ++i)
    {
        HeadSample s;
        s.tNs = i * 20833;
        s.input = value;
        samples.push_back(s);
        value = static_cast<std::uint32_t>((std::uint64_t(value) + 64) % layout.ringBytes);
    }
    const HeadMotion m = analyzeHead(samples, true, layout);
    EXPECT_EQ(m.changes, std::size_t(9));
    EXPECT_EQ(m.backwardSteps, std::size_t(0));
    ASSERT_TRUE(!m.steps.empty());
    EXPECT_EQ(m.steps.front().first, 64u);
    EXPECT_EQ(m.steps.front().second, std::size_t(9));
    EXPECT_EQ(m.advancedBytes, std::uint64_t(9 * 64));
}

TEST_CASE(HeadsAnalysis, NoMotion)
{
    device::RingLayout layout;
    layout.ringBytes = 0x3FC000;
    layout.frameBytes = 64;
    std::vector<HeadSample> samples(5);
    const HeadMotion m = analyzeHead(samples, true, layout);
    EXPECT_EQ(m.changes, std::size_t(0));
    EXPECT_EQ(m.framesPerSecond, 0.0);
    const SentinelSummary s = summarizeSentinel({});
    EXPECT_CONTAINS(s.conclusion, "no trials");
}
