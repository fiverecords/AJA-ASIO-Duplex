// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// duplex-probe routing: the tones, their measurement, and the answers it
// gives against each answer the simulated card can be set to give.

#include "test_framework.h"

#include "RoutingProbe.h"
#include "sim/FakeCardAudio.h"

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

using namespace ajaduplex;
using namespace ajaduplex::probe;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

bool never()
{
    return false;
}

RoutingRun route(const FakeCardModel& model, bool scan = false)
{
    VirtualClock clock;
    FakeCardAudio card(clock, model);
    RoutingRun run = runRouting(card, clock, scan, never);
    card.restore();
    return run;
}

std::string reportOf(const RoutingRun& run)
{
    Report report;
    addRoutingReport(run, report);
    return report.render(true);
}

} // namespace

TEST_CASE(RoutingSignal, TonesAreDistinctPrimesTimes25AndLoopTheRing)
{
    std::set<unsigned> seen;
    for (unsigned c = 0; c < RoutingSignal::kChannels; ++c)
    {
        const unsigned hz = RoutingSignal::toneHz(c);
        EXPECT_EQ(hz % 25, 0u);
        const unsigned p = hz / 25;
        for (unsigned d = 2; d * d <= p; ++d)
            EXPECT_NE(p % d, 0u);
        seen.insert(hz);
        // Whole periods in the 65280-frame ring and in 1920 frames.
        EXPECT_EQ((hz * 65280u) % 48000u, 0u);
        EXPECT_EQ((hz * 1920u) % 48000u, 0u);
        EXPECT_EQ(RoutingSignal::sample(65280, c), RoutingSignal::sample(0, c));
        EXPECT_EQ(RoutingSignal::sample(65280 + 77, c), RoutingSignal::sample(77, c));
    }
    EXPECT_EQ(seen.size(), std::size_t(RoutingSignal::kChannels));
}

TEST_CASE(RoutingSignal, MeasureFindsEachToneAndTheNoise)
{
    // Input 2 of 4 channels: tones of outputs 4 and 10, at -20 and -35 dBFS.
    const std::size_t frames = 9600;
    std::vector<std::int32_t> interleaved(frames * 4, 0);
    std::uint32_t seed = 5;
    for (std::size_t n = 0; n < frames; ++n)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const double noise = 1e-5 * ((double(seed) / 4294967296.0) - 0.5) * 3.4641;   // uniform, RMS 1e-5
        const double x = RoutingSignal::sample(std::int64_t(n), 3) / 2147483648.0 +
                         0.1778 * RoutingSignal::sample(std::int64_t(n) + 11, 9) / 2147483648.0 + noise;
        interleaved[n * 4 + 2] = static_cast<std::int32_t>(std::lround(x * 8388608.0)) * 256;
    }
    const ToneLevels levels = measureTones(interleaved.data(), 4, 2, frames);
    EXPECT_TRUE(std::fabs(levels.dbfs[3] + 20.0) < 0.02);
    EXPECT_TRUE(std::fabs(levels.dbfs[9] + 35.0) < 0.05);
    for (unsigned k = 0; k < RoutingSignal::kChannels; ++k)
    {
        if (k != 3 && k != 9)
            EXPECT_TRUE(levels.dbfs[k] < -90.0);
    }
    const std::vector<unsigned> heard = levels.heard();
    ASSERT_EQ(heard.size(), std::size_t(2));
    EXPECT_EQ(heard[0], 3u);
    EXPECT_EQ(heard[1], 9u);
    EXPECT_TRUE(std::fabs(levels.residualDbfs - (-100.0 + 3.0)) < 1.5);   // RMS 1e-5, AES17

    const ToneLevels silent = measureTones(interleaved.data(), 4, 0, frames);
    EXPECT_TRUE(silent.heard().empty());
}

TEST_CASE(RoutingProbe, AnalogOutFollowsAes12AndReplacesOnlyAes12)
{
    const RoutingRun run = route(FakeCardModel::withAnalogLoop());
    EXPECT_EQ(analogOutSource(run), AnalogOutSource::Aes12);
    EXPECT_EQ(analogInReplaces(run), AnalogInReplaces::Aes12);
    ASSERT_TRUE(run.aes.done);
    ASSERT_EQ(run.aes.inputs.size(), std::size_t(8));
    for (unsigned i = 0; i < 8; ++i)
    {
        const std::string expected = i == 0 ? "L" : i == 1 ? "R" : "A";
        EXPECT_EQ(inputLabel(run, run.aes, i), "A");
        EXPECT_EQ(inputLabel(run, run.analogMonitor16, i), expected);
        EXPECT_EQ(inputLabel(run, run.analogMonitor12, i), expected);
    }
    EXPECT_FALSE(run.scanned);
    const std::string text = reportOf(run);
    EXPECT_CONTAINS(text, "it follows AES out 1-2");
    EXPECT_CONTAINS(text, "only AES in 1-2");
    EXPECT_CONTAINS(text, "not scanned (run routing --scan-select)");
    EXPECT_CONTAINS(text, "\"AES 1 + Analog L\"");
    EXPECT_CONTAINS(text, "Analog L/R plus AES 3-8 as inputs");
}

TEST_CASE(RoutingProbe, ResampledAesInputsStillAnswer)
{
    // The KONA X's AES inputs resample: on 2026-11-10 no AES input came
    // back bit-exact, and Q5 came out unknown with AES 3-8 cabled. Its
    // own output's tone, loudest, is its own output all the same.
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.aesResampler.on = true;
    const RoutingRun run = route(model);
    EXPECT_EQ(analogOutSource(run), AnalogOutSource::Aes12);
    EXPECT_EQ(analogInReplaces(run), AnalogInReplaces::Aes12);
    for (unsigned i = 0; i < 8; ++i)
    {
        EXPECT_EQ(inputLabel(run, run.aes, i), "A~");
        EXPECT_EQ(inputLabel(run, run.analogMonitor16, i), i == 0 ? "L" : i == 1 ? "R" : "A~");
    }
    const std::string text = reportOf(run);
    EXPECT_CONTAINS(text, "only AES in 1-2");
    EXPECT_CONTAINS(text, "A~ resampled on the way");
    EXPECT_CONTAINS(text, "through converters or a resampler");
}

TEST_CASE(RoutingProbe, ResampledAesInputsAndTheSelectScan)
{
    // AES in 1-2 resampled hear outputs 1-2 just as the analog input does:
    // the scan's labels are the same for every value, and the analog
    // path's gain tells (on the KONA X -0.4 dB, 2026-11-10).
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.aesResampler.on = true;
    model.analog.gainDb = -0.4;
    const RoutingRun run = route(model, true);
    ASSERT_TRUE(run.scanned);
    EXPECT_CONTAINS(reportOf(run), "only bit 0 counts: every even value is AES (0), every odd one analog (1) (told by "
                                   "the level in 1 hears out 1 at: -20.0 dBFS with 0, -20.4 with 1");

    model.analog.gainDb = 0.0;
    const RoutingRun same = route(model, true);
    EXPECT_CONTAINS(reportOf(same), "at the same level: the select did not switch anything (or the analog path has "
                                    "AES's gain to 0.05 dB)");
}

TEST_CASE(RoutingProbe, TheCardIsInOemTaskModeWhileItRuns)
{
    VirtualClock clock;
    FakeCardAudio card(clock, FakeCardModel::withAnalogLoop());
    const RoutingRun run = runRouting(card, clock, false, never);
    EXPECT_TRUE(run.oemTasks);
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    EXPECT_CONTAINS(reportOf(run), "OEM while the set-ups last");
    card.restore();
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Standard);

    FakeCardAudio other(clock, FakeCardModel::withAnalogLoop());
    const RoutingRun kept = runRouting(other, clock, false, never, false);
    EXPECT_TRUE(other.routing().taskMode == device::TaskMode::Standard);
    EXPECT_CONTAINS(reportOf(kept), "as the card had it (--tasks keep)");
}

TEST_CASE(RoutingProbe, AnalogOutFollowsTheMonitor)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.analog.outFollowsMonitor = true;
    const RoutingRun run = route(model);
    EXPECT_EQ(analogOutSource(run), AnalogOutSource::Monitor);
    EXPECT_EQ(inputLabel(run, run.analogMonitor16, 0), "L");   // out 15's tone, from the monitor
    EXPECT_EQ(inputLabel(run, run.analogMonitor16, 1), "R");
    EXPECT_EQ(inputLabel(run, run.analogMonitor12, 0), "L");   // out 1's, the monitor on 1-2
    EXPECT_TRUE(run.analogMonitor16.inputs[0].dbfs[14] > -21.0);
    EXPECT_CONTAINS(reportOf(run), "it follows the monitor");
}

TEST_CASE(RoutingProbe, AnalogInputReplacesAllAes)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.analog.replacesAllAes = true;
    const RoutingRun run = route(model);
    EXPECT_EQ(analogInReplaces(run), AnalogInReplaces::AllAes);
    EXPECT_EQ(inputLabel(run, run.analogMonitor16, 5), "-");
    EXPECT_CONTAINS(reportOf(run), "no AES input: 2 inputs");
}

TEST_CASE(RoutingProbe, UnknownsAreSaidAsSuch)
{
    // Only AES 1-2 cabled: Q5 cannot be told.
    FakeCardModel one = FakeCardModel::withAnalogLoop();
    one.cable = {FakeCardModel::Patch{0, 0}, FakeCardModel::Patch{1, 1}};
    const RoutingRun shortCabled = route(one);
    EXPECT_EQ(analogInReplaces(shortCabled), AnalogInReplaces::Unknown);
    EXPECT_EQ(inputLabel(shortCabled, shortCabled.aes, 4), "-");
    EXPECT_CONTAINS(reportOf(shortCabled), "loop AES out 3-8 to AES in 3-8 to tell");

    // No XLR loop: the analog input hears nothing.
    const RoutingRun noXlr = route(FakeCardModel::withAesLoop());
    EXPECT_EQ(analogOutSource(noXlr), AnalogOutSource::Nothing);
    EXPECT_EQ(inputLabel(noXlr, noXlr.analogMonitor16, 0), "-");
    EXPECT_CONTAINS(reportOf(noXlr), "the analog input heard no output");

    // AES out crossed: in 1 hears out 2.
    FakeCardModel crossed = FakeCardModel::withAnalogLoop();
    crossed.cable = {FakeCardModel::Patch{0, 1}, FakeCardModel::Patch{1, 0}};
    const RoutingRun x = route(crossed);
    EXPECT_EQ(inputLabel(x, x.aes, 0), "o2");
    EXPECT_EQ(inputLabel(x, x.aes, 1), "o1");
}

TEST_CASE(RoutingProbe, SelectScanTellsTheModels)
{
    FakeCardModel bit0 = FakeCardModel::withAnalogLoop();
    const RoutingRun a = route(bit0, true);
    ASSERT_TRUE(a.scanned);
    ASSERT_EQ(a.scan.size(), std::size_t(16));
    EXPECT_EQ(inputLabel(a, a.scan[6], 0), "A");
    EXPECT_EQ(inputLabel(a, a.scan[7], 0), "L");
    EXPECT_CONTAINS(reportOf(a), "only bit 0 counts");

    FakeCardModel nonZero = FakeCardModel::withAnalogLoop();
    nonZero.analog.select = FakeCardModel::AnalogPath::Select::NonZero;
    EXPECT_CONTAINS(reportOf(route(nonZero, true)), "any value but 0 selects analog");

    // The case Q14 hopes for: bit k puts the analog on pair k, AES elsewhere.
    FakeCardModel perPair = FakeCardModel::withAnalogLoop();
    perPair.analog.select = FakeCardModel::AnalogPath::Select::PerPair;
    const RoutingRun p = route(perPair, true);
    EXPECT_EQ(inputLabel(p, p.scan[2], 0), "A");
    EXPECT_EQ(inputLabel(p, p.scan[2], 2), "L");
    EXPECT_EQ(inputLabel(p, p.scan[2], 3), "R");
    const std::string text = reportOf(p);
    EXPECT_CONTAINS(text, "other values do something of their own");
    EXPECT_CONTAINS(text, "AES in 1-2 and the analog input at once with value(s) 2, 4, 6, 8, 10, 12, 14");
    EXPECT_CONTAINS(text, "[select scan]");
}

TEST_CASE(RoutingProbe, PutsTheCardBack)
{
    VirtualClock clock;
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.aesOutputsOnThisSystem = false;
    FakeCardAudio card(clock, model);
    const RoutingRun run = runRouting(card, clock, true, never);
    EXPECT_TRUE(run.haveSelect);
    EXPECT_EQ(run.selectBefore, 0u);
    EXPECT_EQ(card.xpandSelect(), 15u);   // the scan's last value, until restore
    card.restore();
    EXPECT_EQ(card.xpandSelect(), 0u);
    EXPECT_FALSE(card.monitorRouted());
    EXPECT_FALSE(card.aesOutputsRouted());
}

TEST_CASE(RoutingProbe, CsvHasARowPerInput)
{
    const RoutingRun run = route(FakeCardModel::withAnalogLoop(), true);
    const std::string csv = routingCsv(run);
    EXPECT_TRUE(csv.rfind("capture,select,input,label,heard,residual_dbfs,digital\n", 0) == 0);
    std::size_t lines = 0;
    for (const char c : csv)
        lines += c == '\n';
    EXPECT_EQ(lines, std::size_t(1 + 3 * 8 + 16 * 8));
    EXPECT_CONTAINS(csv, "\"input AES\",0,1,A,1:-20.0,");
    EXPECT_CONTAINS(csv, "\"select 1\",1,1,L,1:-20.1,");
}
