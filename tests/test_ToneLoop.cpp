// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// The tone loops: the signal, the verifier against made-up converters,
// and whole runs of the engine through the simulated Xpand and through the
// simulated card's resampling AES inputs.

#include "test_framework.h"

#include "ToneLoop.h"
#include "LoopProbe.h"
#include "sim/FakeCardAudio.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>

using namespace ajaduplex;
using namespace ajaduplex::probe;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

constexpr double kFullScale = 2147483648.0;

/// What a pair of converters does to the output: delay, gain, noise.
struct Converters
{
    std::int64_t roundTrip = 300;
    double gainDb = -0.5;
    double noiseDbfs = -100.0;   // AES17
    std::uint32_t seed = 99;

    double gaussian()
    {
        auto uniform = [this] {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return (double(seed) + 1.0) / 4294967297.0;
        };
        const double u1 = uniform(), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

    /// The input word for output frame `outFrame` of `channel` (or silence
    /// if outFrame < 0), as an ADC gives it.
    std::int32_t word(std::int64_t outFrame, unsigned channel, double extraGainDb = 0.0)
    {
        const double y = outFrame >= 0 ? ToneSignal::sample(outFrame, channel) / kFullScale : 0.0;
        return adc(y, extraGainDb);
    }

    /// The same for the steady tone of `channel` that left the output at
    /// the fractional output frame `outFrame` (a delay that drifts), or
    /// silence if outFrame < 0.
    std::int32_t toneWordAt(double outFrame, unsigned channel)
    {
        const double turns = std::fmod(double(ToneSignal::toneHz(channel)) * outFrame, 48000.0) / 48000.0;
        return adc(outFrame >= 0.0 ? 0.1 * std::sin(6.283185307179586 * turns) : 0.0, 0.0);
    }

    std::int32_t adc(double y, double extraGainDb)
    {
        const double sigma = std::pow(10.0, noiseDbfs / 20.0) / std::sqrt(2.0);
        const double x = std::pow(10.0, (gainDb + extraGainDb) / 20.0) * y + sigma * gaussian();
        return static_cast<std::int32_t>(std::lround(x * 8388608.0)) * 256;
    }
};

/// Feeds `seconds` of input to `v`; `map` turns an input frame into the
/// output frame it carries (or -1 for silence).
void feedAll(ToneVerifier& v, Converters& conv, double seconds, unsigned channel,
             const std::function<std::int64_t(std::int64_t)>& map,
             const std::function<double(std::int64_t)>& extraGain = nullptr)
{
    const std::int64_t frames = static_cast<std::int64_t>(seconds * 48000.0);
    for (std::int64_t n = 0; n < frames; ++n)
        v.feed(n, conv.word(map(n), channel, extraGain ? extraGain(n) : 0.0));
    v.finish();
}

/// Feeds `seconds` of input whose delay is `delay(n)` frames from the first
/// window on (a fraction too; negative: silence), and the converters' 300
/// before it.
void feedDrifting(ToneVerifier& v, Converters& conv, double seconds, unsigned channel,
                  const std::function<double(std::int64_t)>& delay)
{
    const std::int64_t frames = static_cast<std::int64_t>(seconds * 48000.0);
    for (std::int64_t n = 0; n < frames; ++n)
    {
        if (n < ToneVerifier::kFirstWindow)
            v.feed(n, conv.word(n - 300, channel));
        else
        {
            const double d = delay(n);
            v.feed(n, conv.toneWordAt(d < 0.0 ? -1.0 : static_cast<double>(n) - d, channel));
        }
    }
    v.finish();
}

/// A delay that drifts by `usPerSecond` from 300 frames at the first window.
double drifting(std::int64_t n, double usPerSecond)
{
    // us per second is 1e-6 frames per frame.
    return 300.0 + usPerSecond * 1e-6 * static_cast<double>(n - ToneVerifier::kFirstWindow);
}

std::function<std::int64_t(std::int64_t)> delayBy(std::int64_t roundTrip)
{
    return [roundTrip](std::int64_t n) { return n - roundTrip; };
}

bool never()
{
    return false;
}

LoopSettings analogSettings(unsigned frames, unsigned guard, double seconds)
{
    LoopSettings s;
    s.frames = frames;
    s.guard = guard;
    s.seconds = seconds;
    s.path = LoopPath::Analog;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// The signal
// ---------------------------------------------------------------------------

TEST_CASE(ToneSignal, Timeline)
{
    for (std::int64_t k = 0; k < ToneSignal::kSweepStart; ++k)
        EXPECT_EQ(ToneSignal::sample(k, 0), 0);

    double sweepPeak = 0.0;
    for (std::int64_t k = ToneSignal::kSweepStart; k < ToneSignal::kSweepStart + ToneSignal::kSweepFrames; ++k)
        sweepPeak = std::max(sweepPeak, std::fabs(ToneSignal::sample(k, 1) / kFullScale));
    EXPECT_TRUE(sweepPeak > 0.09 && sweepPeak <= 0.1);

    for (std::int64_t k = ToneSignal::kSweepStart + ToneSignal::kSweepFrames; k < ToneSignal::kToneStart; ++k)
        EXPECT_EQ(ToneSignal::sample(k, 0), 0);

    // Fade-in, then the tone at -20 dBFS, in the top 24 bits.
    double early = 0.0, steady = 0.0;
    for (std::int64_t k = ToneSignal::kToneStart; k < ToneSignal::kToneStart + 48; ++k)
        early = std::max(early, std::fabs(ToneSignal::sample(k, 0) / kFullScale));
    for (std::int64_t k = 20000; k < 21000; ++k)
    {
        steady = std::max(steady, std::fabs(ToneSignal::sample(k, 0) / kFullScale));
        EXPECT_EQ(ToneSignal::sample(k, 0) & 0xFF, 0);
    }
    EXPECT_TRUE(early < 0.01);
    EXPECT_TRUE(std::fabs(20.0 * std::log10(steady) + 20.0) < 0.01);
}

TEST_CASE(ToneSignal, TonesStayExactOnLongRuns)
{
    // A day in: the phase comes from (Hz x frame) mod 48000, not a growing float.
    for (const std::int64_t k : {std::int64_t(100000), std::int64_t(4147200000LL)})
    {
        for (unsigned c = 0; c < 2; ++c)
        {
            const double hz = ToneSignal::toneHz(c);
            const std::int64_t m = (static_cast<std::int64_t>(hz) * k) % 48000;
            const double x = 0.1 * std::sin(2.0 * 3.141592653589793 * double(m) / 48000.0);
            EXPECT_EQ(ToneSignal::sample(k, c), static_cast<std::int32_t>(std::lround(x * 8388608.0)) * 256);
        }
    }
    EXPECT_EQ(ToneSignal::toneHz(0), 997u);
    EXPECT_EQ(ToneSignal::toneHz(1), 1499u);
}

TEST_CASE(ToneSignal, EightTonesAndAQuieterLevel)
{
    // Through AES, one tone per channel, all prime (to 48000 too), at -40
    // dBFS: the KONA X's analog output plays AES 1-2 as well.
    const unsigned expected[8] = {997, 1499, 1997, 2503, 2999, 3499, 4001, 4507};
    for (unsigned c = 0; c < 8; ++c)
    {
        EXPECT_EQ(ToneSignal::toneHz(c), expected[c]);
        for (unsigned d = 2; d * d <= expected[c]; ++d)
            EXPECT_TRUE(expected[c] % d != 0);
    }
    double peak = 0.0;
    for (std::int64_t k = 20000; k < 21000; ++k)
    {
        peak = std::max(peak, std::fabs(ToneSignal::sample(k, 3, ToneSignal::kAesToneDbfs) / kFullScale));
        EXPECT_EQ(ToneSignal::sample(k, 3, ToneSignal::kAesToneDbfs),
                  ToneSignal::word(ToneSignal::value(k, 3) * ToneSignal::scaleFor(ToneSignal::kAesToneDbfs)));
    }
    EXPECT_TRUE(std::fabs(20.0 * std::log10(peak) + 40.0) < 0.01);
}

// ---------------------------------------------------------------------------
// The verifier, on made-up converters
// ---------------------------------------------------------------------------

TEST_CASE(ToneVerifier, CleanLoopIsMeasured)
{
    for (unsigned channel = 0; channel < 2; ++channel)
    {
        Converters conv;
        ToneVerifier v(channel);
        feedAll(v, conv, 5.0, channel, delayBy(300));
        EXPECT_TRUE(v.checked);
        EXPECT_TRUE(v.heard);
        EXPECT_FALSE(v.crossed);
        EXPECT_TRUE(std::fabs(v.levelDbfs + 20.5) < 0.02);
        EXPECT_TRUE(std::fabs(v.gainDb + 0.5) < 0.02);
        EXPECT_TRUE(std::fabs(v.noiseDbfs + 100.0) < 1.5);
        EXPECT_TRUE(v.worstNoiseDbfs < -98.0);
        EXPECT_TRUE(v.seconds >= 4);
        EXPECT_EQ(v.glitches, 0u);
        EXPECT_EQ(v.slips, 0u);
        EXPECT_EQ(v.levelChanges, 0u);
        ASSERT_TRUE(v.roundTripFound);
        EXPECT_TRUE(std::fabs(v.roundTrip - 300.0) < 0.1);
        EXPECT_TRUE(v.sweepMatch > 0.99);
        EXPECT_FALSE(v.inverted);
        EXPECT_TRUE(v.clean());
    }
}

TEST_CASE(ToneVerifier, InvertedPolarityIsReportedNotFailed)
{
    Converters conv;
    ToneVerifier v(0);
    feedAll(v, conv, 3.0, 0, delayBy(250));
    EXPECT_FALSE(v.inverted);

    // The same through an inverting cable.
    struct Inverting
    {
        Converters conv;
        std::int32_t word(std::int64_t out) { return -conv.word(out, 0); }
    } inverting;
    ToneVerifier w(0);
    for (std::int64_t n = 0; n < 3 * 48000; ++n)
        w.feed(n, inverting.word(n - 250));
    w.finish();
    EXPECT_TRUE(w.inverted);
    EXPECT_TRUE(std::fabs(w.roundTrip - 250.0) < 0.1);
    EXPECT_TRUE(w.clean());
}

TEST_CASE(ToneVerifier, LostFramesAreASlipAndAGlitch)
{
    Converters conv;
    ToneVerifier v(0);
    const std::int64_t at = 3 * 48000 + 1234;
    feedAll(v, conv, 6.0, 0, [=](std::int64_t n) { return n < at ? n - 300 : n - 300 + 5; });
    EXPECT_TRUE(v.glitches >= 1);
    EXPECT_EQ(v.slips, 1u);
    EXPECT_EQ(v.worstSlipFrames, 5);
    EXPECT_EQ(v.slipsOfUnknownSize, 0u);
    ASSERT_TRUE(!v.glitchFrames.empty());
    EXPECT_TRUE(std::llabs(static_cast<long long>(v.glitchFrames.front() - at)) < 4);
    EXPECT_FALSE(v.clean());
}

TEST_CASE(ToneVerifier, RepeatedFrameAndWholePeriodSlips)
{
    // Two still seconds first: the second says how steady the delay is,
    // which the slip's size needs.
    Converters conv;
    ToneVerifier repeated(0);
    const std::int64_t at = 3 * 48000 + 777;
    feedAll(repeated, conv, 6.0, 0, [=](std::int64_t n) { return n < at ? n - 300 : n - 300 - 1; });
    EXPECT_EQ(repeated.slips, 1u);
    EXPECT_EQ(repeated.worstSlipFrames, -1);

    // 48 frames lost is almost one period of 997 Hz: the tone hardly jumps
    // where it happens (a small glitch), and its phase moves by 144/48000
    // of a turn, under a quarter frame's: after the glitch it is a slip
    // all the same, of exactly 48 frames.
    Converters conv2;
    ToneVerifier period(0);
    feedAll(period, conv2, 6.0, 0, [=](std::int64_t n) { return n < at ? n - 300 : n - 300 + 48; });
    EXPECT_EQ(period.slips, 1u);
    EXPECT_EQ(period.worstSlipFrames, 48);
    EXPECT_FALSE(period.clean());

    // The stream falls back 96 frames, as when the output is placed anew
    // 2 ms later than it was (after a stall, say).
    Converters conv3;
    ToneVerifier late(1);
    feedAll(late, conv3, 6.0, 1, [=](std::int64_t n) { return n < at ? n - 300 : (n < at + 96 ? -1 : n - 396); });
    EXPECT_EQ(late.slips, 1u);
    EXPECT_EQ(late.worstSlipFrames, -96);
    EXPECT_TRUE(late.glitches >= 1);
}

TEST_CASE(ToneVerifier, DropoutIsOneGlitch)
{
    Converters conv;
    ToneVerifier v(1);
    const std::int64_t at = 3 * 48000 + 4321;
    feedAll(v, conv, 6.0, 1, [=](std::int64_t n) { return n >= at && n < at + 64 ? -1 : n - 300; });
    EXPECT_EQ(v.glitches, 1u);   // its two edges are one glitch
    EXPECT_EQ(v.slips, 0u);
    EXPECT_EQ(v.levelChanges, 0u);
    EXPECT_FALSE(v.clean());
}

TEST_CASE(ToneVerifier, GlitchInTheFirstSecondIsCaught)
{
    Converters conv;
    ToneVerifier v(0);
    const std::int64_t at = ToneVerifier::kFirstWindow + 20000;   // while the detector calibrates
    feedAll(v, conv, 4.0, 0, [=](std::int64_t n) { return n >= at && n < at + 32 ? -1 : n - 300; });
    EXPECT_EQ(v.glitches, 1u);
    EXPECT_FALSE(v.clean());

    // A slip there: no still second before it, so its size is not guessed.
    Converters conv2;
    ToneVerifier slipped(0);
    feedAll(slipped, conv2, 5.0, 0, [=](std::int64_t n) { return n < at ? n - 300 : (n < at + 96 ? -1 : n - 396); });
    EXPECT_EQ(slipped.slips, 1u);
    EXPECT_EQ(slipped.worstSlipFrames, 0);
    EXPECT_EQ(slipped.slipsOfUnknownSize, 1u);
}

TEST_CASE(ToneVerifier, DriftIsAddedUpNotASlip)
{
    // The analog loop on the KONA X drifted by about 0.2 us a second
    // (2026-11-10), and 0.1.16 called it a slip in every run: no slip, and
    // the drift said. Ten times that too.
    for (const double usPerSecond : {0.2, 2.0})
    {
        for (unsigned channel = 0; channel < 2; ++channel)
        {
            Converters conv;
            ToneVerifier v(channel);
            feedDrifting(v, conv, 10.0, channel, [=](std::int64_t n) { return drifting(n, usPerSecond); });
            EXPECT_EQ(v.glitches, 0u);
            EXPECT_EQ(v.slips, 0u);
            EXPECT_TRUE(v.clean());
            // From the first second's middle to the last one's: 8 s.
            EXPECT_TRUE(std::fabs(v.driftUs - 8.0 * usPerSecond) < 0.01 + 0.01 * usPerSecond);
            EXPECT_TRUE(v.wanderUs < 0.01);
        }
    }
}

TEST_CASE(ToneVerifier, DriftAcrossAGlitchIsNoSlip)
{
    // The second with the dropout is not measured, and the next one is held
    // against the last still one moved on by two seconds of drift: 2 us a
    // second is more than a fortieth of a frame, and still no slip.
    Converters conv;
    ToneVerifier v(0);
    const std::int64_t at = 5 * 48000 + 1000;
    feedDrifting(v, conv, 10.0, 0,
                 [=](std::int64_t n) { return n >= at && n < at + 64 ? -1.0 : drifting(n, 2.0); });
    EXPECT_EQ(v.glitches, 1u);
    EXPECT_EQ(v.slips, 0u);
    EXPECT_TRUE(std::fabs(v.driftUs - 16.0) < 0.05);
}

TEST_CASE(ToneVerifier, ASlipOnADriftingDelayHasItsSize)
{
    Converters conv;
    ToneVerifier v(1);
    const std::int64_t at = 5 * 48000 + 1000;
    feedDrifting(v, conv, 10.0, 1, [=](std::int64_t n) { return drifting(n, 2.0) - (n >= at ? 5.0 : 0.0); });
    EXPECT_TRUE(v.glitches >= 1);
    EXPECT_EQ(v.slips, 1u);
    EXPECT_EQ(v.worstSlipFrames, 5);
    EXPECT_EQ(v.slipsOfUnknownSize, 0u);
    EXPECT_TRUE(std::fabs(v.driftUs - 16.0) < 0.05);   // the slip is not drift
}

TEST_CASE(ToneVerifier, AWanderingDelayIsNoSlipEvenAfterAGlitch)
{
    // A converter whose delay wanders by a microsecond or so either way
    // over a few seconds: what each second moves, once the drift is out, is
    // far over a fortieth of a frame, so after a glitch the tolerance
    // follows the median wander instead.
    auto wander = [](std::int64_t n) {
        const double t = static_cast<double>(n - ToneVerifier::kFirstWindow) / 48000.0;
        const double us = std::sin(6.283185307179586 * t / 5.0) +
                          0.5 * (std::sin(6.283185307179586 * t / 3.3 + 1.0) - std::sin(1.0));
        return 300.0 + us * 0.048;   // 0.048 frames a microsecond; 300 where it starts
    };
    Converters conv;
    ToneVerifier v(0);
    const std::int64_t at = 9 * 48000 + 1000;
    feedDrifting(v, conv, 16.0, 0, [=](std::int64_t n) { return n >= at && n < at + 64 ? -1.0 : wander(n); });
    EXPECT_EQ(v.glitches, 1u);
    EXPECT_EQ(v.slips, 0u);
    EXPECT_TRUE(v.wanderUs > 0.3);

    // Frames lost on top of it are still a slip, but the wander leaves the
    // phase too rough for its size.
    Converters conv2;
    ToneVerifier lost(0);
    feedDrifting(lost, conv2, 16.0, 0, [=](std::int64_t n) { return wander(n) - (n >= at ? 3.0 : 0.0); });
    EXPECT_TRUE(lost.glitches >= 1);
    EXPECT_EQ(lost.slips, 1u);
    EXPECT_EQ(lost.worstSlipFrames, 0);
    EXPECT_EQ(lost.slipsOfUnknownSize, 1u);
}

TEST_CASE(ToneVerifier, NoiseIsTheMedianSecondsNotTheFirst)
{
    // The first second through the Xpand was noisy while its input settled
    // (2026-11-10): the noise reported is the median second's.
    Converters conv;
    conv.noiseDbfs = -70.0;
    ToneVerifier v(0);
    for (std::int64_t n = 0; n < 5 * 48000; ++n)
    {
        if (n == ToneVerifier::kFirstWindow + ToneVerifier::kWindowFrames)
            conv.noiseDbfs = -100.0;
        v.feed(n, conv.word(n - 300, 0));
    }
    v.finish();
    EXPECT_TRUE(std::fabs(v.firstNoiseDbfs + 70.0) < 1.5);
    EXPECT_TRUE(std::fabs(v.worstNoiseDbfs + 70.0) < 1.5);
    EXPECT_TRUE(std::fabs(v.noiseDbfs + 100.0) < 1.5);
    EXPECT_TRUE(v.clean());
}

TEST_CASE(ToneVerifier, LevelChange)
{
    Converters conv;
    ToneVerifier v(0);
    feedAll(v, conv, 6.0, 0, delayBy(300), [](std::int64_t n) { return n > 3 * 48000 ? -1.0 : 0.0; });
    EXPECT_TRUE(v.levelChanges >= 2);
    EXPECT_TRUE(std::fabs(v.worstLevelChangeDb + 1.0) < 0.05);
    EXPECT_FALSE(v.clean());
}

TEST_CASE(ToneVerifier, CrossedOrMissing)
{
    Converters conv;
    ToneVerifier crossed(0);   // L, fed R's tone
    feedAll(crossed, conv, 3.0, 1, delayBy(300));
    EXPECT_TRUE(crossed.checked);
    EXPECT_FALSE(crossed.heard);
    EXPECT_TRUE(crossed.crossed);
    EXPECT_FALSE(crossed.clean());

    Converters conv2;
    ToneVerifier missing(1);   // only noise
    feedAll(missing, conv2, 3.0, 1, [](std::int64_t) { return std::int64_t(-1); });
    EXPECT_TRUE(missing.checked);
    EXPECT_FALSE(missing.heard);
    EXPECT_FALSE(missing.crossed);
    EXPECT_FALSE(missing.roundTripFound);

    ToneVerifier shortRun(0);
    Converters conv3;
    feedAll(shortRun, conv3, 1.0, 0, delayBy(300));
    EXPECT_FALSE(shortRun.checked);
    EXPECT_FALSE(shortRun.clean());
}

// ---------------------------------------------------------------------------
// Runs through the simulated Xpand
// ---------------------------------------------------------------------------

TEST_CASE(AnalogLoop, CleanRunAndTheConvertersShare)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.aesOutputsOnThisSystem = false;   // the run routes what it needs

    VirtualClock clock;
    FakeCardAudio card(clock, model);
    const LoopResult r = runLoop(card, clock, analogSettings(128, 32, 4.0), never);
    ASSERT_TRUE(r.started);
    EXPECT_TRUE(r.clean());
    EXPECT_TRUE(card.analogSelected());
    EXPECT_TRUE(card.aesOutputsRouted());
    EXPECT_TRUE(card.monitorRouted());
    EXPECT_TRUE(r.haveAnalogLevel);
    EXPECT_EQ(r.analogLevelDbu, 24);
    ASSERT_EQ(r.tones.size(), std::size_t(2));
    for (const ToneVerifier& a : r.tones)
    {
        EXPECT_TRUE(a.clean());
        EXPECT_TRUE(std::fabs(a.gainDb - model.analog.gainDb) < 0.02);
        EXPECT_TRUE(std::fabs(a.noiseDbfs - model.analog.noiseDbfs) < 1.5);
    }

    // The same N and G through AES: the difference is the converters' delay
    // less the AES cable's, in the model 40 - 12 frames.
    VirtualClock clock2;
    FakeCardAudio aesCard(clock2, FakeCardModel::withAesLoop());
    LoopSettings aes = analogSettings(128, 32, 2.0);
    aes.path = LoopPath::AesWords;
    const LoopResult a = runLoop(aesCard, clock2, aes, never);
    ASSERT_TRUE(a.clean());
    const double difference = r.tones[0].roundTrip - static_cast<double>(a.channels[0].minRoundTrip);
    EXPECT_TRUE(std::fabs(difference - double(model.analog.delayFrames - model.cableDelayFrames)) < 0.2);

    card.restore();
    EXPECT_FALSE(card.analogSelected());
    EXPECT_FALSE(card.monitorRouted());
}

TEST_CASE(AnalogLoop, WorksWhicheverWayTheAnalogOutIsFed)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.analog.outFollowsMonitor = true;   // Q4, the other answer
    model.analog.inverted = true;
    VirtualClock clock;
    FakeCardAudio card(clock, model);
    const LoopResult r = runLoop(card, clock, analogSettings(64, 32, 3.0), never);
    EXPECT_TRUE(r.clean());
    ASSERT_EQ(r.tones.size(), std::size_t(2));
    EXPECT_TRUE(r.tones[0].inverted);

    Report report;
    LoopRun run;
    run.description = card.description();
    run.results.push_back(r);
    addLoopReport(run, report);
    EXPECT_CONTAINS(report.render(true), "polarity inverted");
}

TEST_CASE(AnalogLoop, NoXlrCable)
{
    FakeCardModel model = FakeCardModel::withAesLoop();   // AES only: nothing on the XLRs
    VirtualClock clock;
    FakeCardAudio card(clock, model);
    const LoopResult r = runLoop(card, clock, analogSettings(128, 32, 3.0), never);
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.clean());
    EXPECT_CONTAINS(loopSummaryLine(r), "in L never heard its tone: check the XLR cable from out L to in L");
}

TEST_CASE(AnalogLoop, LateOutputShowsAsAGlitchWithoutASlip)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAnalogLoop());
    // Two seconds in, two DMAs 6 ms slow: S is 160 frames (3.3 ms), so a few
    // blocks come too late and what the card passed is dropped. The output
    // stays on its timeline (D1): a gap in the tone, its phase unmoved.
    const std::int64_t start = clock.nowNs();
    bool injected = false;
    auto inject = [&] {
        if (!injected && clock.nowNs() - start > 2000000000)
        {
            card.delayNextDmas(2, 6000000);
            injected = true;
        }
        return false;
    };
    const LoopResult r = runLoop(card, clock, analogSettings(128, 32, 5.0), inject);
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.clean());
    EXPECT_TRUE(r.engine.lateOutputs > 0);
    ASSERT_EQ(r.tones.size(), std::size_t(2));
    EXPECT_EQ(r.engine.outputMoves, 0u);
    EXPECT_TRUE(r.tones[0].glitches > 0);
    EXPECT_TRUE(r.tones[1].glitches > 0);
    EXPECT_EQ(r.tones[0].slips, 0u);
    EXPECT_EQ(r.tones[1].slips, 0u);
    EXPECT_CONTAINS(loopSummaryLine(r), "glitches on L");
    EXPECT_TRUE(loopSummaryLine(r).find("slipped") == std::string::npos);
}

TEST_CASE(AnalogLoop, ReportAndCsv)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAnalogLoop());
    LoopRun run;
    run.description = card.description();
    run.results.push_back(runLoop(card, clock, analogSettings(128, 32, 3.0), never));
    run.results.push_back(runLoop(card, clock, analogSettings(128, 16, 3.0), never));

    Report report;
    addLoopReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "analog: XLR out L/R cabled to XLR in L/R");
    EXPECT_CONTAINS(text, "997 Hz on L, 1499 Hz on R, at -20 dBFS (+4 dBu with the Xpand at +24 dBu)");
    EXPECT_CONTAINS(text, "+24 dBu at 0 dBFS");
    EXPECT_CONTAINS(text, "card, converters and cable");
    EXPECT_CONTAINS(text, "on L and R");
    EXPECT_CONTAINS(text, "noise and distortion");
    EXPECT_CONTAINS(text, "exit test, analog (N = 128, one hour, no glitch)");
    EXPECT_CONTAINS(text, "duplex-probe loop --path analog --frames 128 --guard 256 --seconds 3600");
    EXPECT_CONTAINS(text, "converters (Q12)");

    const std::string csv = loopCsv(run);
    EXPECT_TRUE(csv.rfind("frames,guard,lead,seconds,blocks,late_outputs,dropped_frames,output_moves,overruns,stalls,"
                          "dma_errors,dma_recovered,glitches_l,",
                          0) ==
                0);
    EXPECT_CONTAINS(csv, "\n128,32,160,");
    std::size_t lines = 0;
    for (const char c : csv)
        lines += c == '\n';
    EXPECT_EQ(lines, std::size_t(3));
}

// ---------------------------------------------------------------------------
// Runs through the simulated card's resampling AES inputs
// ---------------------------------------------------------------------------

namespace
{

LoopSettings aesToneSettings(unsigned frames, unsigned guard, double seconds)
{
    LoopSettings s;
    s.frames = frames;
    s.guard = guard;
    s.seconds = seconds;
    s.path = LoopPath::Aes;
    s.channels = {0, 1, 2, 3, 4, 5, 6, 7};
    return s;
}

FakeCardModel resamplingAes()
{
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.aesResampler.on = true;
    return model;
}

} // namespace

TEST_CASE(AesToneLoop, CleanThroughTheResampler)
{
    // The KONA X's AES inputs resample: the words came back changed
    // (2026-11-10), the tones come back whole.
    VirtualClock clock;
    FakeCardAudio card(clock, resamplingAes());
    const LoopResult r = runLoop(card, clock, aesToneSettings(128, 32, 4.0), never);
    ASSERT_TRUE(r.started);
    EXPECT_TRUE(r.clean());
    ASSERT_EQ(r.tones.size(), std::size_t(8));

    // The round trip is the plain cable's and the resampler's.
    VirtualClock plainClock;
    FakeCardAudio plain(plainClock, FakeCardModel::withAesLoop());
    LoopSettings words = aesToneSettings(128, 32, 1.0);
    words.path = LoopPath::AesWords;
    const LoopResult exact = runLoop(plain, plainClock, words, never);
    ASSERT_TRUE(exact.clean());
    const double plainTrip = static_cast<double>(exact.channels[0].minRoundTrip);
    for (const ToneVerifier& a : r.tones)
    {
        EXPECT_TRUE(a.clean());
        EXPECT_TRUE(std::fabs(a.levelDbfs - ToneSignal::kAesToneDbfs) < 0.5);   // interpolated: a little less at 4.5 kHz
        EXPECT_TRUE(std::fabs(a.roundTrip - plainTrip - 18.4) < 0.2);
    }
    EXPECT_CONTAINS(loopSummaryLine(r), "on AES 1-8");

    // Word by word, through the resampler, nothing comes back.
    VirtualClock clock2;
    FakeCardAudio card2(clock2, resamplingAes());
    const LoopResult changed = runLoop(card2, clock2, words, never);
    ASSERT_TRUE(changed.started);
    EXPECT_FALSE(changed.clean());
    EXPECT_TRUE(changed.garbage() > 0);
    EXPECT_CONTAINS(loopSummaryLine(changed), "AES 1-8 in bring words changed (resampled? a path under 24 bits?): "
                                              "the KONA X's AES inputs resample, and --path aes checks them with tones");
    EXPECT_TRUE(loopSummaryLine(changed).find("jumps back") == std::string::npos);
}

TEST_CASE(AesToneLoop, CrossedCables)
{
    FakeCardModel model = resamplingAes();
    model.cable = {FakeCardModel::Patch{0, 1}, FakeCardModel::Patch{1, 0}};   // out 1 to in 2, out 2 to in 1
    VirtualClock clock;
    FakeCardAudio card(clock, model);
    LoopSettings s = aesToneSettings(128, 32, 3.0);
    s.channels = {0, 1};
    const LoopResult r = runLoop(card, clock, s, never);
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.clean());
    EXPECT_CONTAINS(loopSummaryLine(r), "AES 1 in hears AES 2's tone: crossed cables");
    EXPECT_CONTAINS(loopSummaryLine(r), "AES 2 in hears AES 1's tone: crossed cables");
}

TEST_CASE(AesToneLoop, ADelayThatNeverMovesReadsPlusZero)
{
    // Without the resampler the delay never moves: the drift came out as
    // -0.0 and the report read "delay drifted +-0.00 us".
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAesLoop());
    LoopRun run;
    run.description = card.description();
    run.results.push_back(runLoop(card, clock, aesToneSettings(128, 32, 3.0), never));
    ASSERT_TRUE(run.results[0].clean());

    Report report;
    addLoopReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "delay drifted +0.00 us, wander 0.00 us");
    EXPECT_CONTAINS(text, "(gain +0.00 dB)");
    EXPECT_TRUE(text.find("+-") == std::string::npos);
    EXPECT_TRUE(text.find("-0.00 ") == std::string::npos);
}

TEST_CASE(AesToneLoop, ReportAndCsv)
{
    VirtualClock clock;
    FakeCardAudio card(clock, resamplingAes());
    LoopRun run;
    run.description = card.description();
    run.results.push_back(runLoop(card, clock, aesToneSettings(128, 32, 3.0), never));
    run.results.push_back(runLoop(card, clock, aesToneSettings(128, 16, 3.0), never));

    Report report;
    addLoopReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "AES inputs resample: 18.4 frames more");
    EXPECT_CONTAINS(text, "AES 1, 2, 3, 4, 5, 6, 7, 8, each out to the same in");
    EXPECT_CONTAINS(text, "tones: the KONA X's AES inputs resample");
    EXPECT_CONTAINS(text, "997 Hz on AES 1, 1499 Hz on AES 2");
    EXPECT_CONTAINS(text, "4507 Hz on AES 8, at -40 dBFS");
    EXPECT_CONTAINS(text, "The analog output plays AES out 1-2 too (Q4) (-16 dBu with the Xpand at +24 dBu)");
    EXPECT_CONTAINS(text, "card, cable and the AES inputs' resampler");
    EXPECT_CONTAINS(text, "AES 8: back at");
    EXPECT_CONTAINS(text, "delay drifted");
    EXPECT_CONTAINS(text, "exit test, AES (N = 128, one hour, no glitch)");
    EXPECT_CONTAINS(text, "duplex-probe loop --channels 1-8 --frames 128 --guard 256 --seconds 3600");

    const std::string csv = loopCsv(run);
    EXPECT_TRUE(csv.rfind("frames,guard,lead,seconds,blocks,late_outputs,dropped_frames,output_moves,overruns,stalls,"
                          "dma_errors,dma_recovered,channels,heard,glitches,slips,",
                          0) == 0);
    EXPECT_CONTAINS(csv, "\n128,32,160,");
    EXPECT_CONTAINS(csv, ",8,8,0,0,0,");   // eight looped, eight heard, no glitch, slip or level change
    std::size_t lines = 0;
    for (const char c : csv)
        lines += c == '\n';
    EXPECT_EQ(lines, std::size_t(3));
}
