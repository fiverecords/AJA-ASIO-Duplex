// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "engine/ClockModel.h"
#include "engine/RingMath.h"
#include "engine/SampleConvert.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace ajaduplex::engine;

namespace
{

constexpr std::uint32_t kRing = 0x3FC000;   // KONA X, 4 MB buffers
constexpr std::uint32_t kFrame = 64;        // 16 channels

} // namespace

TEST_CASE(RingMath, DistanceAndAdvanceWrap)
{
    const RingGeometry ring(kRing, kFrame);
    EXPECT_TRUE(ring.valid());
    EXPECT_EQ(ring.frames(), 65280u);
    EXPECT_EQ(ring.distance(0, 64), 64u);
    EXPECT_EQ(ring.distance(kRing - 64, 64), 128u);
    EXPECT_EQ(ring.distance(64, 64), 0u);
    EXPECT_EQ(ring.advance(kRing - 64, 192), 128u);
    EXPECT_EQ(ring.retreat(64, 128), kRing - 64);
    EXPECT_EQ(ring.retreat(0, 0), 0u);
    EXPECT_EQ(ring.frameFloor(130), 128u);
}

TEST_CASE(RingMath, FramesAvailableByHeadMode)
{
    const RingGeometry ring(kRing, kFrame);
    // Two whole frames written: last byte 127, next byte 128.
    EXPECT_EQ(ring.framesAvailable(127, 0, HeadMode::LastByte), 2u);
    EXPECT_EQ(ring.framesAvailable(128, 0, HeadMode::NextByte), 2u);
    // The design's formula is one frame short with a last-byte head, exact otherwise.
    EXPECT_EQ(ring.framesAvailable(127, 0, HeadMode::Conservative), 1u);
    EXPECT_EQ(ring.framesAvailable(128, 0, HeadMode::Conservative), 2u);
    // A head in the middle of a frame only counts whole frames.
    EXPECT_EQ(ring.framesAvailable(100, 0, HeadMode::NextByte), 1u);
    // Across the wrap.
    EXPECT_EQ(ring.framesAvailable(63, kRing - 128, HeadMode::LastByte), 3u);
    EXPECT_EQ(ring.framesAvailable(kRing - 1, kRing - 128, HeadMode::LastByte), 2u);
    // Nothing written yet.
    EXPECT_EQ(ring.framesAvailable(0, 0, HeadMode::Conservative), 0u);
}

TEST_CASE(RingMath, SplitAtTheWrap)
{
    const RingGeometry ring(kRing, kFrame);
    Segment s[2];
    EXPECT_EQ(ring.split(0, 8192, s), 1);
    EXPECT_EQ(s[0].offset, 0u);
    EXPECT_EQ(s[0].bytes, 8192u);

    EXPECT_EQ(ring.split(kRing - 8192, 8192, s), 1);   // ends exactly at the wrap
    EXPECT_EQ(ring.split(kRing - 4096, 8192, s), 2);
    EXPECT_EQ(s[0].offset, kRing - 4096);
    EXPECT_EQ(s[0].bytes, 4096u);
    EXPECT_EQ(s[1].offset, 0u);
    EXPECT_EQ(s[1].bytes, 4096u);
    EXPECT_EQ(ring.split(5, 0, s), 0);
}

TEST_CASE(SampleConvert, RoundTrip)
{
    constexpr unsigned kChannels = 16;
    constexpr std::size_t kFrames = 128;
    std::vector<std::int32_t> interleaved(kChannels * kFrames);
    for (std::size_t i = 0; i < interleaved.size(); ++i)
        interleaved[i] = static_cast<std::int32_t>((i * 2654435761u) & 0xFFFFFF00u);

    std::vector<std::vector<std::int32_t>> channels(kChannels, std::vector<std::int32_t>(kFrames, -1));
    std::vector<std::int32_t*> pointers(kChannels);
    for (unsigned c = 0; c < kChannels; ++c)
        pointers[c] = channels[c].data();
    deinterleave(interleaved.data(), kChannels, kFrames, pointers.data());
    EXPECT_EQ(channels[3][10], interleaved[10 * kChannels + 3]);
    EXPECT_EQ(channels[15][127], interleaved[127 * kChannels + 15]);

    std::vector<std::int32_t> back(kChannels * kFrames, 7);
    std::vector<const std::int32_t*> sources(pointers.begin(), pointers.end());
    interleave(sources.data(), kChannels, kFrames, back.data());
    EXPECT_TRUE(back == interleaved);
}

TEST_CASE(SampleConvert, NullChannels)
{
    std::vector<std::int32_t> interleaved = {1, 2, 3, 4, 5, 6};   // 3 frames of 2 channels
    std::vector<std::int32_t> left(3, 0);
    std::int32_t* targets[2] = {left.data(), nullptr};
    deinterleave(interleaved.data(), 2, 3, targets);
    EXPECT_EQ(left[2], 5);

    const std::int32_t* sources[2] = {nullptr, left.data()};
    interleave(sources, 2, 3, interleaved.data());
    EXPECT_EQ(interleaved[0], 0);   // a channel the host did not activate is silent
    EXPECT_EQ(interleaved[1], 1);
    EXPECT_EQ(interleaved[5], 5);
}

TEST_CASE(ClockModel, NominalUntilReady)
{
    ClockModel model(48000.0);
    EXPECT_FALSE(model.ready());
    EXPECT_EQ(model.hostTimeOf(100), std::int64_t(0));
    model.observe(1000000000, 0);
    EXPECT_FALSE(model.ready());
    EXPECT_TRUE(std::fabs(model.framesPerSecond() - 48000.0) < 1e-6);
    // Through the only point at the nominal rate: 48 frames later is 1 ms later.
    EXPECT_EQ(model.hostTimeOf(48), std::int64_t(1001000000));
}

TEST_CASE(ClockModel, FitsAFastCardClock)
{
    ClockModel model(48000.0, 256);
    const double rate = 48000.0 * (1.0 + 40e-6);   // +40 ppm
    for (int i = 0; i < 300; ++i)
    {
        const std::int64_t t = 5000000000LL + std::int64_t(i) * 2666667;   // every ~2.67 ms
        const std::int64_t frames = static_cast<std::int64_t>(static_cast<double>(t - 5000000000LL) * rate / 1e9);
        // up to 50 us of polling lateness, deterministic
        model.observe(t + (i * 7919) % 50000, frames);
    }
    ASSERT_TRUE(model.ready());
    EXPECT_TRUE(std::fabs(model.ppm() - 40.0) < 10.0);
    // Prediction of a frame one second after the last observation, within 100 us.
    const std::int64_t lastFrames = static_cast<std::int64_t>(299 * 2666667 * rate / 1e9);
    const std::int64_t predicted = model.hostTimeOf(lastFrames + 48002);
    const std::int64_t truth = 5000000000LL + static_cast<std::int64_t>((lastFrames + 48002) / rate * 1e9);
    EXPECT_TRUE(std::llabs(predicted - truth) < 100000);
}

TEST_CASE(ClockModel, IgnoresAnAbsurdFit)
{
    ClockModel model(48000.0, 64);
    for (int i = 0; i < 64; ++i)
        model.observe(std::int64_t(i) * 1000000, std::int64_t(i) * 96);   // 96 kHz: 2x nominal
    ASSERT_TRUE(model.ready());
    EXPECT_TRUE(std::fabs(model.framesPerSecond() - 48000.0) < 1e-6);
}
