// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "sim/FakeCardAudio.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ajaduplex;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

constexpr std::int64_t kMs = 1000000;

bool configure16(FakeCardAudio& card)
{
    std::string error;
    return card.configure(device::AudioConfig{}, error);
}

/// Sample value carrying the frame index and channel, low byte zero like
/// real 24-bit audio.
std::int32_t ramp(std::int64_t frame, unsigned channel)
{
    return static_cast<std::int32_t>(((frame * 16 + channel) & 0xFFFFFF) << 8);
}

} // namespace

TEST_CASE(FakeCardAudio, LayoutLikeAKonaX)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    ASSERT_TRUE(configure16(card));
    const device::RingLayout layout = card.layout();
    EXPECT_EQ(layout.ringBytes, 0x3FC000u);
    EXPECT_EQ(layout.captureOffset, 0x400000u);
    EXPECT_EQ(layout.frameBytes, 64u);
    EXPECT_EQ(layout.ringFrames(), 65280u);
}

TEST_CASE(FakeCardAudio, RejectsChannelCountsTheRingCannotHold)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    std::string error;
    device::AudioConfig config;
    config.channels = 7;
    EXPECT_FALSE(card.configure(config, error));
    config.channels = 0;
    EXPECT_FALSE(card.configure(config, error));
    config.channels = 17;
    EXPECT_FALSE(card.configure(config, error));
    config.channels = 8;
    EXPECT_TRUE(card.configure(config, error));
}

TEST_CASE(FakeCardAudio, InputHeadFollowsTheAudioClock)
{
    VirtualClock clock;
    FakeCardModel model;
    model.clockPpm = 20.0;
    FakeCardAudio card(clock, model);
    ASSERT_TRUE(configure16(card));
    std::uint32_t head = 1;
    ASSERT_TRUE(card.readInputHead(head));
    EXPECT_EQ(head, 0u);   // stopped

    ASSERT_TRUE(card.startInput());
    clock.advance(1000 * kMs);
    const std::int64_t written = card.framesWritten();
    EXPECT_TRUE(written >= 48000 && written <= 48002);   // 48000.96 frames
    ASSERT_TRUE(card.readInputHead(head));
    EXPECT_EQ(head % 64u, 63u);                          // last byte of a frame
    EXPECT_EQ((head + 1u) / 64u, static_cast<std::uint32_t>(written % 65280));
}

TEST_CASE(FakeCardAudio, NextByteHeads)
{
    VirtualClock clock;
    FakeCardModel model;
    model.inputHeadAtLastByte = false;
    FakeCardAudio card(clock, model);
    ASSERT_TRUE(configure16(card));
    ASSERT_TRUE(card.startInput());
    clock.advance(10 * kMs);
    std::uint32_t head = 0;
    ASSERT_TRUE(card.readInputHead(head));
    EXPECT_EQ(head % 64u, 0u);
}

TEST_CASE(FakeCardAudio, OutputFetchesAheadInChunks)
{
    VirtualClock clock;
    FakeCardAudio card(clock);   // prefetch 32, chunks of 16
    ASSERT_TRUE(configure16(card));
    ASSERT_TRUE(card.startOutput());
    EXPECT_EQ(card.framesFetched(), std::int64_t(32));
    std::uint32_t head = 0;
    ASSERT_TRUE(card.readOutputHead(head));
    EXPECT_EQ(head, 32u * 64u);
    clock.advance(1 * kMs);   // 48 frames played
    EXPECT_EQ(card.framesFetched(), std::int64_t(80));
}

TEST_CASE(FakeCardAudio, RegisterReadsAndDmaCostVirtualTime)
{
    VirtualClock clock;
    FakeCardModel model;
    FakeCardAudio card(clock, model);
    ASSERT_TRUE(configure16(card));
    std::uint32_t head = 0;
    std::int64_t t0 = clock.nowNs();
    card.readInputHead(head);
    EXPECT_EQ(clock.nowNs() - t0, model.registerReadNs);

    std::vector<std::uint8_t> buffer(8192);
    t0 = clock.nowNs();
    ASSERT_TRUE(card.dmaRead(device::Ring::Capture, 0, buffer.data(), 8192));
    EXPECT_EQ(clock.nowNs() - t0, model.dmaSetupNs + 2048);

    ASSERT_TRUE(card.lockBuffer(buffer.data(), 8192));
    t0 = clock.nowNs();
    ASSERT_TRUE(card.dmaWrite(device::Ring::Playback, 0, buffer.data(), 8192));
    EXPECT_EQ(clock.nowNs() - t0, model.dmaSetupLockedNs + 2048);
    EXPECT_TRUE(card.unlockBuffer(buffer.data(), 8192));
    EXPECT_EQ(card.lockedBufferCount(), std::size_t(0));
}

TEST_CASE(FakeCardAudio, DmaMustStayInsideTheRingsRoom)
{
    // As on the KONA X: each ring has 4 MB, of which it uses L; DMAs may go
    // past L, into what the card never plays or fills, but no further.
    VirtualClock clock;
    FakeCardAudio card(clock);
    ASSERT_TRUE(configure16(card));
    std::vector<std::uint8_t> buffer(4096);
    const std::uint32_t ring = card.layout().ringBytes;
    const std::uint32_t room = card.layout().roomBytes;
    EXPECT_EQ(ring, 0x3FC000u);
    EXPECT_EQ(room, 0x400000u);
    EXPECT_TRUE(card.dmaRead(device::Ring::Capture, ring - 4096, buffer.data(), 4096));
    EXPECT_TRUE(card.dmaRead(device::Ring::Capture, ring - 4092, buffer.data(), 4096));
    EXPECT_TRUE(card.dmaWrite(device::Ring::Playback, ring, buffer.data(), 4));
    EXPECT_TRUE(card.dmaWrite(device::Ring::Playback, room - 4096, buffer.data(), 4096));
    EXPECT_FALSE(card.dmaWrite(device::Ring::Playback, room - 4092, buffer.data(), 4096));
    EXPECT_FALSE(card.dmaRead(device::Ring::Capture, 0, buffer.data(), 0));

    // Past the end the capture ring holds nothing the card wrote.
    ASSERT_TRUE(card.dmaRead(device::Ring::Capture, ring, buffer.data(), 64));
    EXPECT_EQ(buffer[0], std::uint8_t(0xA5));

    FakeCardModel tight;
    tight.roomPastEnd = false;
    FakeCardAudio noRoom(clock, tight);
    ASSERT_TRUE(configure16(noRoom));
    EXPECT_EQ(noRoom.layout().roomBytes, ring);
    EXPECT_FALSE(noRoom.dmaRead(device::Ring::Capture, ring - 4092, buffer.data(), 4096));
    EXPECT_FALSE(noRoom.dmaWrite(device::Ring::Playback, ring, buffer.data(), 4));
}

TEST_CASE(FakeCardAudio, ABadBufferFailsWhereverItGoes)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    ASSERT_TRUE(configure16(card));
    std::vector<std::uint8_t> good(8192), bad(8192);
    card.recordDmas(true);
    card.failDmasThroughBufferAt(device::Ring::Playback, 100000, 31, 1);
    EXPECT_TRUE(card.dmaWrite(device::Ring::Playback, 98304, good.data(), 8192));   // the spare one
    EXPECT_FALSE(card.dmaWrite(device::Ring::Playback, 98304, bad.data(), 8192));   // now bad
    EXPECT_EQ(card.lastDmaSystemError(), 31u);
    EXPECT_FALSE(card.dmaRead(device::Ring::Capture, 0, bad.data(), 64));
    EXPECT_TRUE(card.dmaWrite(device::Ring::Playback, 98304, good.data(), 8192));
    EXPECT_TRUE(card.dmaRead(device::Ring::Capture, 0, good.data(), 64));
    EXPECT_EQ(card.failedDmas(), std::uint64_t(2));
    ASSERT_EQ(card.dmaRecords().size(), std::size_t(5));
    EXPECT_FALSE(card.dmaRecords()[1].ok);
    EXPECT_TRUE(card.dmaRecords()[1].write);
    EXPECT_EQ(card.dmaRecords()[2].offset, 0u);
    EXPECT_TRUE(card.dmaRecords()[2].host == bad.data());
}

TEST_CASE(FakeCardAudio, EraseModeZeroesWhatWasFetched)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    ASSERT_TRUE(configure16(card));
    std::vector<std::uint32_t> words(64 * 1024 / 4, 0xABCDEF00u);
    ASSERT_TRUE(card.dmaWrite(device::Ring::Playback, 0, words.data(), 64 * 1024));   // 1024 frames
    ASSERT_TRUE(card.startOutput());
    clock.advance(10 * kMs);   // ~480 frames played, ~512 fetched
    // Read the position first: the DMA itself takes time, during which the
    // card fetches more.
    const std::int64_t fetched = card.framesFetched();
    std::vector<std::uint32_t> back(64 * 1024 / 4, 1);
    ASSERT_TRUE(card.dmaRead(device::Ring::Playback, 0, back.data(), 64 * 1024));
    EXPECT_TRUE(fetched > 400 && fetched < 1024);
    EXPECT_EQ(back[0], 0u);
    EXPECT_EQ(back[static_cast<std::size_t>(fetched) * 16 - 1], 0u);
    EXPECT_EQ(back[static_cast<std::size_t>(fetched) * 16], 0xABCDEF00u);
}

TEST_CASE(FakeCardAudio, CableDeliversTheOutputAfterItsDelay)
{
    VirtualClock clock;
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.cableDelayFrames = 12;
    FakeCardAudio card(clock, model);
    ASSERT_TRUE(configure16(card));

    // A ramp in every channel of the first 4096 frames, before playing.
    std::vector<std::int32_t> frames(4096 * 16);
    for (std::int64_t f = 0; f < 4096; ++f)
        for (unsigned c = 0; c < 16; ++c)
            frames[static_cast<std::size_t>(f) * 16 + c] = ramp(f, c);
    ASSERT_TRUE(card.dmaWrite(device::Ring::Playback, 0, frames.data(), 4096 * 64));

    ASSERT_TRUE(card.startOutput());
    ASSERT_TRUE(card.startInput());   // same audio tick: no extra offset
    clock.advance(50 * kMs);          // 2400 frames

    std::vector<std::int32_t> captured(2048 * 16);
    ASSERT_TRUE(card.dmaRead(device::Ring::Capture, 0, captured.data(), 2048 * 64));
    for (unsigned c = 0; c < 16; ++c)
        EXPECT_EQ(captured[11 * 16 + c], 0);                       // before the cable delay
    EXPECT_EQ(captured[12 * 16 + 0], ramp(0, 0));                  // output frame 0 arrives at input 12
    EXPECT_EQ(captured[100 * 16 + 7], ramp(88, 7));                // AES 8 looped
    EXPECT_EQ(captured[100 * 16 + 8], 0);                          // channel 9 is not patched
    EXPECT_EQ(captured[2047 * 16 + 3], ramp(2035, 3));
}

TEST_CASE(FakeCardAudio, CaptureEnableOffKeepsMemoryButMovesTheHead)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    std::string error;
    device::AudioConfig config;
    config.captureEnable = false;
    ASSERT_TRUE(card.configure(config, error));
    std::vector<std::uint32_t> words(1024, 0x5A5A5A5Au);
    ASSERT_TRUE(card.dmaWrite(device::Ring::Capture, 0, words.data(), 4096));
    ASSERT_TRUE(card.startInput());
    clock.advance(20 * kMs);
    std::uint32_t head = 0;
    ASSERT_TRUE(card.readInputHead(head));
    EXPECT_TRUE(head > 0);
    ASSERT_TRUE(card.dmaRead(device::Ring::Capture, 0, words.data(), 4096));
    EXPECT_EQ(words[0], 0x5A5A5A5Au);
    EXPECT_EQ(words[1023], 0x5A5A5A5Au);
}

TEST_CASE(FakeCardAudio, WrapEvents)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    ASSERT_TRUE(configure16(card));
    EXPECT_FALSE(card.subscribe(device::CardEvent::Audio));   // not simulated
    ASSERT_TRUE(card.subscribe(device::CardEvent::AudioInWrap));
    ASSERT_TRUE(card.startInput());

    const std::int64_t t0 = clock.nowNs();
    EXPECT_FALSE(card.waitForEvent(device::CardEvent::AudioInWrap, 100));   // 1.36 s away
    EXPECT_EQ(clock.nowNs() - t0, 100 * kMs);
    EXPECT_TRUE(card.waitForEvent(device::CardEvent::AudioInWrap, 2000));
    const std::int64_t lap = clock.nowNs() - t0;
    EXPECT_TRUE(lap > 1359 * kMs && lap < 1361 * kMs);   // 65280 frames at 48 kHz

    std::uint32_t count = 0;
    ASSERT_TRUE(card.interruptCount(device::CardEvent::AudioInWrap, count));
    EXPECT_EQ(count, 1u);
}

TEST_CASE(FakeCardAudio, CounterOnItsOwnClock)
{
    VirtualClock clock;
    FakeCardModel model;
    model.counterOnAudioClock = false;
    model.counterPpm = -50.0;
    FakeCardAudio card(clock, model);
    ASSERT_TRUE(configure16(card));
    std::uint32_t a = 0, b = 0;
    ASSERT_TRUE(card.readSampleCounter(a));
    clock.advance(10000 * kMs);
    ASSERT_TRUE(card.readSampleCounter(b));
    const std::uint32_t ticks = b - a;
    EXPECT_TRUE(ticks >= 479975 && ticks <= 479977);   // 480000 * (1 - 50e-6)
}

TEST_CASE(FakeCardAudio, RestoreStopsEverything)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    ASSERT_TRUE(configure16(card));
    std::vector<std::uint8_t> buffer(4096);
    ASSERT_TRUE(card.lockBuffer(buffer.data(), 4096));
    ASSERT_TRUE(card.startInput());
    ASSERT_TRUE(card.startOutput());
    card.restore();
    std::uint32_t head = 1;
    ASSERT_TRUE(card.readInputHead(head));
    EXPECT_EQ(head, 0u);
    EXPECT_EQ(card.lockedBufferCount(), std::size_t(0));
}

TEST_CASE(FakeCardAudio, AesOutputsFollowAnotherSystemUntilRouted)
{
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.cableDelayFrames = 12;
    model.aesOutputsOnThisSystem = false;

    for (const bool route : {false, true})
    {
        VirtualClock clock;
        FakeCardAudio card(clock, model);
        device::AudioConfig config;
        config.routeAesOutputs = route;
        std::string error;
        ASSERT_TRUE(card.configure(config, error));
        EXPECT_EQ(card.aesOutputsRouted(), route);

        std::vector<std::int32_t> frames(1024 * 16);
        for (std::int64_t f = 0; f < 1024; ++f)
            for (unsigned c = 0; c < 16; ++c)
                frames[static_cast<std::size_t>(f) * 16 + c] = ramp(f, c);
        ASSERT_TRUE(card.dmaWrite(device::Ring::Playback, 0, frames.data(), 1024 * 64));
        ASSERT_TRUE(card.startOutput());
        ASSERT_TRUE(card.startInput());
        clock.advance(20 * kMs);   // 960 frames

        std::vector<std::int32_t> captured(512 * 16);
        ASSERT_TRUE(card.dmaRead(device::Ring::Capture, 0, captured.data(), 512 * 64));
        EXPECT_EQ(captured[100 * 16 + 1], route ? ramp(88, 1) : 0);

        card.restore();
        EXPECT_FALSE(card.aesOutputsRouted());
    }
}

namespace
{

/// A tone on output channels 1-2 for the analog tests: 0.1 of full scale.
std::int32_t analogTone(std::int64_t frame, unsigned channel)
{
    const double x = 0.1 * std::sin(2.0 * 3.141592653589793 * (channel ? 1499.0 : 997.0) * double(frame) / 48000.0);
    return static_cast<std::int32_t>(std::lround(x * 8388608.0)) * 256;
}

struct AnalogRig
{
    VirtualClock clock;
    FakeCardAudio card;
    std::vector<std::int32_t> played;

    explicit AnalogRig(const FakeCardModel& model) : card(clock, model), played(2048 * 16) {}

    bool run(const device::AudioConfig& config)
    {
        std::string error;
        if (!card.configure(config, error))
            return false;
        for (std::int64_t f = 0; f < 2048; ++f)
            for (unsigned c = 0; c < 16; ++c)
                played[std::size_t(f) * 16 + c] = c < 2 ? analogTone(f, c) : ramp(f, c);
        if (!card.dmaWrite(device::Ring::Playback, 0, played.data(), 2048 * 64) || !card.startOutput() ||
            !card.startInput())
            return false;
        clock.advance(40 * kMs);   // 1920 frames
        captured.assign(1024 * 16, 0);
        return card.dmaRead(device::Ring::Capture, 0, captured.data(), 1024 * 64);
    }
    std::int32_t in(std::int64_t frame, unsigned channel) const { return captured[std::size_t(frame) * 16 + channel]; }
    std::int32_t out(std::int64_t frame, unsigned channel) const { return played[std::size_t(frame) * 16 + channel]; }

    std::vector<std::int32_t> captured;
};

device::AudioConfig analogConfig()
{
    device::AudioConfig config;
    config.source = device::InputSource::Analog;
    config.keepXpandSelect = false;
    config.routeAesOutputs = true;
    return config;
}

} // namespace

TEST_CASE(FakeCardAudio, AnalogLoopDelaysScalesAndAddsNoise)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    AnalogRig rig(model);
    ASSERT_TRUE(rig.run(analogConfig()));
    EXPECT_TRUE(rig.card.analogSelected());

    const double gain = std::pow(10.0, model.analog.gainDb / 20.0);
    const double dc = model.analog.dcOffset * 2147483648.0;
    const double sigma = std::pow(10.0, model.analog.noiseDbfs / 20.0) / std::sqrt(2.0) * 2147483648.0;
    double worst = 0.0;
    for (std::int64_t k = 200; k < 1000; ++k)
    {
        for (unsigned c = 0; c < 2; ++c)
        {
            const double expected = gain * rig.out(k - model.analog.delayFrames, c) + dc;
            worst = std::max(worst, std::fabs(rig.in(k, c) - expected));
        }
        EXPECT_EQ(rig.in(k, 2), rig.out(k - model.cableDelayFrames, 2));   // AES 3 still AES
    }
    EXPECT_TRUE(worst < 6.0 * sigma + 256.0);
    EXPECT_TRUE(worst > 0.0);   // there is noise

    int level = 0;
    EXPECT_TRUE(rig.card.readAnalogLevel(level));
    EXPECT_EQ(level, 24);
}

TEST_CASE(FakeCardAudio, AnalogOutFollowsTheMonitorOnlyOnceRouted)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.analog.outFollowsMonitor = true;
    for (const bool route : {false, true})
    {
        AnalogRig rig(model);
        device::AudioConfig config = analogConfig();
        config.routeMonitor = route;
        ASSERT_TRUE(rig.run(config));
        EXPECT_EQ(rig.card.monitorRouted(), route);
        double peak = 0.0;
        for (std::int64_t k = 200; k < 1000; ++k)
            peak = std::max(peak, std::fabs(double(rig.in(k, 0))));
        // The tone at 0.1 of full scale, or only offset and noise.
        EXPECT_EQ(peak > 0.05 * 2147483648.0, route);
        rig.card.restore();
        EXPECT_FALSE(rig.card.monitorRouted());
        EXPECT_FALSE(rig.card.analogSelected());
    }
}

TEST_CASE(FakeCardAudio, AnalogInputReplacesWhatTheModelSays)
{
    FakeCardModel model = FakeCardModel::withAnalogLoop();
    model.analog.replacesAllAes = true;
    AnalogRig all(model);
    ASSERT_TRUE(all.run(analogConfig()));
    EXPECT_EQ(all.in(500, 2), 0);   // AES 3-8 gone too
    EXPECT_EQ(all.in(500, 7), 0);

    // Keeping the select (as heads and cost do): the input stays AES.
    AnalogRig kept(FakeCardModel::withAnalogLoop());
    device::AudioConfig config = analogConfig();
    config.keepXpandSelect = true;
    ASSERT_TRUE(kept.run(config));
    EXPECT_FALSE(kept.card.analogSelected());
    EXPECT_EQ(kept.in(500, 0), kept.out(500 - model.cableDelayFrames, 0));
}
