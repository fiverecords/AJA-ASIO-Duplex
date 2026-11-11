// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// The engine on the simulated card, with AES out 1-8 looped to AES in 1-8.
// The client writes a frame counter on every output channel and records
// what comes back on input 1, so continuity and round trip can be checked.

#include "test_framework.h"

#include "engine/AudioEngine.h"
#include "sim/FakeCardAudio.h"

#include <vector>

using namespace ajaduplex;
using namespace ajaduplex::engine;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

constexpr std::int64_t kMs = 1000000;
constexpr unsigned kChannels = 16;

std::int32_t encode(std::int64_t frame, unsigned channel)
{
    return static_cast<std::int32_t>(((std::uint32_t(frame) & 0xFFFFFu) << 12) | ((channel & 0xFu) << 8));
}

class TestClient final : public EngineClient
{
public:
    TestClient(VirtualClock& clock, unsigned frames) : clock_(clock), frames_(frames)
    {
        for (unsigned h = 0; h < 2; ++h)
        {
            in_[h].assign(kChannels, std::vector<std::int32_t>(frames, 0));
            out_[h].assign(kChannels, std::vector<std::int32_t>(frames, 0));
            for (unsigned c = 0; c < kChannels; ++c)
            {
                inPointers_[h][c] = in_[h][c].data();
                outPointers_[h][c] = out_[h][c].data();
            }
        }
    }

    BlockBuffers buffers(unsigned half) override
    {
        return BlockBuffers{inPointers_[half], outPointers_[half]};
    }

    void process(const BlockInfo& info, unsigned half) override
    {
        blocks.push_back(info);
        for (unsigned f = 0; f < frames_; ++f)
            captured.push_back(in_[half][0][f]);
        for (unsigned c = 0; c < kChannels; ++c)
            for (unsigned f = 0; f < frames_; ++f)
                out_[half][c][f] = encode(std::int64_t(info.index) * frames_ + f, c);
        if (engine != nullptr && outputReadyAt == info.index)
        {
            engine->outputReady();
            for (unsigned c = 0; c < kChannels; ++c)
                for (unsigned f = 0; f < frames_; ++f)
                    out_[half][c][f] = 0x7FFFFF00;   // too late to be sent
        }
        if (sleepAt == info.index)
            clock_.advance(sleepNs);
        if (sleepEveryNs > 0)
            clock_.advance(sleepEveryNs);
    }

    void onEngineEvent(EngineEvent event, const BlockInfo&) override { events.push_back(event); }

    /// Round trips (input frame - output frame it carries), one per input
    /// sample of channel 1 that holds a counter.
    std::vector<std::int64_t> roundTrips() const
    {
        std::vector<std::int64_t> trips;
        for (std::size_t i = 0; i < captured.size(); ++i)
        {
            const std::uint32_t v = static_cast<std::uint32_t>(captured[i]);
            if (v == 0 || ((v >> 8) & 0xFu) != 0)
                continue;
            const std::int64_t counter = (v >> 12) & 0xFFFFF;
            trips.push_back((std::int64_t(i) - counter) & 0xFFFFF);
        }
        return trips;
    }

    /// Output frames that never came back: gaps in the counters on input 1.
    std::uint64_t lostFrames() const
    {
        std::uint64_t lost = 0;
        std::int64_t previous = -1;
        for (const std::int32_t sample : captured)
        {
            const std::uint32_t v = static_cast<std::uint32_t>(sample);
            if (v == 0 || ((v >> 8) & 0xFu) != 0)
                continue;
            const std::int64_t counter = (v >> 12) & 0xFFFFF;
            if (previous >= 0)
                lost += static_cast<std::uint64_t>((counter - previous - 1) & 0xFFFFF);
            previous = counter;
        }
        return lost;
    }

    std::vector<BlockInfo> blocks;
    std::vector<std::int32_t> captured;
    std::vector<EngineEvent> events;
    AudioEngine* engine = nullptr;
    std::uint64_t outputReadyAt = ~std::uint64_t(0);
    std::uint64_t sleepAt = ~std::uint64_t(0);
    std::int64_t sleepNs = 0;
    std::int64_t sleepEveryNs = 0;   ///< what every callback takes

private:
    VirtualClock& clock_;
    unsigned frames_;
    std::vector<std::vector<std::int32_t>> in_[2];
    std::vector<std::vector<std::int32_t>> out_[2];
    std::int32_t* inPointers_[2][kChannels] = {};
    const std::int32_t* outPointers_[2][kChannels] = {};
};

struct Rig
{
    VirtualClock clock;
    FakeCardAudio card;
    AudioEngine engine;
    TestClient client;

    explicit Rig(unsigned frames = 128, FakeCardModel model = FakeCardModel::withAesLoop())
        : card(clock, model), engine(card, clock), client(clock, frames)
    {
        client.engine = &engine;
    }

    bool start(unsigned frames = 128, unsigned guard = 32, bool aggressive = false)
    {
        EngineConfig config;
        config.blockFrames = frames;
        config.guardFrames = guard;
        config.aggressive = aggressive;
        std::string error;
        return engine.start(config, client, error);
    }

    void run(unsigned blocks)
    {
        for (unsigned i = 0; i < blocks; ++i)
        {
            if (!engine.runOnce())
                break;
        }
    }
};

bool count(const std::vector<EngineEvent>& events, EngineEvent which, std::size_t expected)
{
    std::size_t n = 0;
    for (const EngineEvent e : events)
        n += e == which;
    return n == expected;
}

/// How many round trips differ from the first.
std::size_t changes(const std::vector<std::int64_t>& trips)
{
    std::size_t n = 0;
    for (const std::int64_t t : trips)
        n += t != trips.front();
    return n;
}

} // namespace

TEST_CASE(AudioEngine, SteadyState)
{
    Rig rig;
    ASSERT_TRUE(rig.start());
    rig.run(2000);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.blocks, std::uint64_t(2000));
    EXPECT_EQ(s.lateOutputs, std::uint64_t(0));
    EXPECT_EQ(s.inputOverruns, std::uint64_t(0));
    EXPECT_EQ(s.stalls, std::uint64_t(0));
    EXPECT_EQ(s.dmaErrors, std::uint64_t(0));
    EXPECT_TRUE(s.minOutputMarginFrames > 0);
    // The playback head where the input's timing puts it, within a step of
    // each head (16 frames the output's here, one the input's).
    EXPECT_TRUE(s.minOutputStepFrames > -24 && s.maxOutputStepFrames < 24);
    // The engine's fit over its last 256 observations, which the head's
    // steps quantise: within 10 ppm of the card's +20 (start's buffer check
    // moved it from 19 to 11 by moving where the blocks fall against the
    // steps). The probe measures the clock over the whole run.
    EXPECT_TRUE(s.clockPpm > 8.0 && s.clockPpm < 32.0);
    EXPECT_TRUE(rig.client.events.empty());

    // Blocks follow each other: positions N apart, about 2.67 ms apart.
    ASSERT_EQ(rig.client.blocks.size(), std::size_t(2000));
    for (std::size_t i = 1; i < rig.client.blocks.size(); ++i)
        EXPECT_EQ(rig.client.blocks[i].samplePosition - rig.client.blocks[i - 1].samplePosition, std::int64_t(128));
    const double meanNs = static_cast<double>(rig.client.blocks.back().hostTimeNs - rig.client.blocks[100].hostTimeNs) / 1899.0;
    EXPECT_TRUE(meanNs > 2.66e6 && meanNs < 2.68e6);
}

TEST_CASE(AudioEngine, AClockModelGoneStaleHoldsNoBlockBack)
{
    // The model says when the next block is due, and only readings taken
    // while polling feed it. On Windows the timer wakes about 0.5 ms late
    // (the KONA X's PC, 2026-11-10), so the engine seldom polls and the model
    // goes without readings for a long time, while the card's clock moves
    // with temperature. Here, as on that PC: a DMA takes 110 us and the heads
    // step 8 frames; the model learns a card 500 ppm slow (its clock stands
    // still 5 us in every 10 ms for two seconds), then the card runs at its
    // own rate and the timer always wakes 0.4 ms late. Trusted whatever it
    // said, the model held each block back 0.5 ms more every second: after
    // 20 s every block was late, for good.
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.dmaSetupNs = 110000;
    model.registerReadNs = 4000;
    model.inputChunkFrames = 8;
    model.outputChunkFrames = 8;
    Rig rig(128, model);
    const std::int64_t t0 = rig.clock.nowNs();
    for (int i = 0; i < 200; ++i)
        rig.card.stallAudioClock(t0 + i * 10 * kMs, 5000);
    ASSERT_TRUE(rig.start(128, 256));
    rig.run(700);
    EXPECT_TRUE(rig.engine.stats().clockPpm < -400.0);   // learnt while it polled

    sim::SleepLateness late;
    late.tailProbability = 1.0;
    late.tailNs = 400000;
    rig.clock.setSleepLateness(late);
    rig.run(375 * 40);   // 40 s
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.lateOutputs, std::uint64_t(0));
    EXPECT_EQ(s.droppedOutputFrames, std::uint64_t(0));
    // Each block found as late as the timer makes it, and no later: S is
    // 384 frames, less the wake-up, the head's step and the two DMAs.
    EXPECT_TRUE(s.minOutputMarginFrames > 320);
    EXPECT_EQ(s.clockPpm, 0.0);   // dropped: it disagreed with the readings
    EXPECT_TRUE(rig.client.events.empty());
}

TEST_CASE(AudioEngine, TheBlockAtTheRingsEndGoesOutWhole)
{
    // On the KONA X (0.1.13's first session) one DMA error came every 1.36
    // s, a lap of the ring: the output block split at the ring's end, its
    // two DMAs each of part of a locked buffer. The fake fails those as the
    // card did; the engine locks nothing now, and no block may lose
    // anything at the ring's end.
    Rig rig;
    rig.card.lockedDmasWholeOnly(true);
    ASSERT_TRUE(rig.start(128, 256));
    EXPECT_EQ(rig.card.lockedBufferCount(), std::size_t(0));
    rig.run(1200);   // a lap is 510 blocks of 128
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.dmaErrors, std::uint64_t(0));
    EXPECT_EQ(rig.card.failedDmas(), std::uint64_t(0));
    EXPECT_EQ(s.lateOutputs, std::uint64_t(0));
    EXPECT_EQ(rig.client.lostFrames(), std::uint64_t(0));
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 100000);
    EXPECT_EQ(changes(trips), std::size_t(0));
}

TEST_CASE(AudioEngine, TheBlockAtTheRingsEndRunsOnPastIt)
{
    // On the KONA X (2026-11-10, 0.1.15) the piece of the output block that
    // ended right at the ring's end failed every lap. A block across the
    // end now goes in one DMA from its position on, running past the end
    // into the room the card never plays, and what wraps again at the
    // ring's start: no piece stops at the end, and every frame comes back.
    Rig rig;
    ASSERT_TRUE(rig.start(128, 256));
    rig.card.recordDmas(true);
    rig.run(1200);
    const std::uint32_t ring = rig.engine.layout().ringBytes;
    const std::uint32_t block = 128u * 64u;
    unsigned across = 0, atStart = 0;
    for (const FakeCardAudio::DmaRecord& d : rig.card.dmaRecords())
    {
        if (!d.write)
            continue;
        EXPECT_TRUE(d.ok);
        EXPECT_FALSE(d.offset + d.bytes == ring && d.bytes < block);   // no piece stops at the end
        if (d.offset < ring && d.offset + d.bytes > ring)
        {
            ++across;
            EXPECT_EQ(d.bytes, block);   // the whole block, from its buffer
        }
        if (d.offset == 0)
            ++atStart;
    }
    EXPECT_EQ(across, 2u);    // two laps
    EXPECT_EQ(atStart, 2u);   // what wrapped, again at the start
    EXPECT_EQ(rig.engine.stats().dmaErrors, std::uint64_t(0));
    EXPECT_EQ(rig.client.lostFrames(), std::uint64_t(0));
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 100000);
    EXPECT_EQ(changes(trips), std::size_t(0));
}

TEST_CASE(AudioEngine, TheInputBlockAtTheRingsEndComesInWhole)
{
    // A lap, 65,280 frames, is no multiple of 512: input blocks cross the
    // ring's end too, and are read past it (what comes from there is not
    // audio: the fake fills it with 0xA5).
    Rig rig(512);
    rig.card.lockedDmasWholeOnly(true);
    ASSERT_TRUE(rig.start(512, 256));
    rig.card.recordDmas(true);
    rig.run(400);   // three laps
    EXPECT_EQ(rig.engine.stats().dmaErrors, std::uint64_t(0));
    EXPECT_EQ(rig.card.failedDmas(), std::uint64_t(0));
    unsigned across = 0;
    const std::uint32_t ring = rig.engine.layout().ringBytes;
    for (const FakeCardAudio::DmaRecord& d : rig.card.dmaRecords())
        across += !d.write && d.offset < ring && d.offset + d.bytes > ring;
    EXPECT_TRUE(across >= 2u);
    EXPECT_EQ(rig.client.lostFrames(), std::uint64_t(0));
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 100000);
    EXPECT_EQ(changes(trips), std::size_t(0));
}

TEST_CASE(AudioEngine, WithoutRoomPastTheEndTheBlockIsSplit)
{
    // A card that leaves nothing past the end gets the two pieces as before.
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.roomPastEnd = false;
    Rig rig(512, model);
    ASSERT_TRUE(rig.start(512, 256));
    rig.card.recordDmas(true);
    rig.run(400);
    const std::uint32_t ring = rig.engine.layout().ringBytes;
    unsigned endingThere = 0;
    for (const FakeCardAudio::DmaRecord& d : rig.card.dmaRecords())
    {
        EXPECT_TRUE(d.offset + d.bytes <= ring);
        endingThere += d.offset + d.bytes == ring && d.bytes < 512u * 64u;
    }
    EXPECT_TRUE(endingThere >= 2u);
    EXPECT_EQ(rig.engine.stats().dmaErrors, std::uint64_t(0));
    EXPECT_EQ(rig.client.lostFrames(), std::uint64_t(0));
    EXPECT_EQ(changes(rig.client.roundTrips()), std::size_t(0));
}

TEST_CASE(AudioEngine, WhatIsLeftOfALateBlockGoesOutToo)
{
    // A late block's rest goes out from a buffer of its own too.
    Rig rig;
    rig.card.lockedDmasWholeOnly(true);
    ASSERT_TRUE(rig.start(128, 32));
    rig.client.sleepAt = 300;
    rig.client.sleepNs = 4 * 1000 * 1000;
    rig.run(600);
    const EngineStats s = rig.engine.stats();
    EXPECT_TRUE(s.lateOutputs >= 1);
    EXPECT_TRUE(s.droppedOutputFrames % 128 != 0);   // part of a block went out
    EXPECT_EQ(s.dmaErrors, std::uint64_t(0));
    EXPECT_EQ(rig.card.failedDmas(), std::uint64_t(0));
}

TEST_CASE(AudioEngine, AFailedDmaSaysWhichOne)
{
    // Writes at the start of the playback ring fail: here the part of the
    // output block past the ring's end, written again at its start, once a
    // lap. The same DMA fails from the same buffer and from a spare: lost.
    Rig rig;
    ASSERT_TRUE(rig.start(128, 256));
    rig.card.failPlaybackWritesAt(0, 87);
    rig.run(600);
    EXPECT_EQ(rig.engine.stats().dmaErrors, std::uint64_t(1));
    EXPECT_EQ(rig.engine.stats().dmaRecovered, std::uint64_t(0));
    const DmaErrorDetail& d = rig.engine.lastDmaError();
    EXPECT_TRUE(d.output);
    EXPECT_EQ(d.pieces, 2u);
    EXPECT_TRUE(d.pastEnd);
    EXPECT_EQ(d.failed, 2u);
    EXPECT_EQ(d.lost, 2u);
    EXPECT_TRUE(d.whole);
    EXPECT_EQ(d.ringOffset, 0u);
    EXPECT_TRUE(d.bytes > 0 && d.bytes < 128u * 64u && d.bytes % 64u == 0);
    EXPECT_EQ(d.systemError, 87u);
    EXPECT_TRUE(d.sameBuffer == RetryResult::Failed);
    EXPECT_EQ(d.sameBufferError, 87u);
    EXPECT_TRUE(d.spareBuffer == RetryResult::Failed);
    EXPECT_EQ(d.spareBufferError, 87u);
    EXPECT_FALSE(d.recovered());
    EXPECT_EQ(rig.card.failedDmas(), std::uint64_t(3));
    rig.engine.stop();
    EXPECT_TRUE(rig.engine.retiredBuffers().empty());

    char text[400];
    describeDmaError(d, text, sizeof(text));
    EXPECT_CONTAINS(std::string(text), "output, the part of the block past the ring's end, at the ring's start: ");
    EXPECT_CONTAINS(std::string(text), " bytes at ring offset 0; system error 87; again from the same buffer: failed, "
                                       "system error 87; from a spare buffer: failed, system error 87");
}

TEST_CASE(AudioEngine, APassingFaultIsSavedByASecondTry)
{
    Rig rig;
    ASSERT_TRUE(rig.start(128, 32));
    rig.run(100);
    rig.card.failNextDmas(1, 87);   // the next block's input
    rig.run(100);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.dmaErrors, std::uint64_t(0));
    EXPECT_EQ(s.dmaRecovered, std::uint64_t(1));
    EXPECT_TRUE(count(rig.client.events, EngineEvent::DmaRecovered, 1));
    const DmaErrorDetail& d = rig.engine.lastDmaError();
    EXPECT_FALSE(d.output);
    EXPECT_TRUE(d.sameBuffer == RetryResult::WentThrough);
    EXPECT_TRUE(d.spareBuffer == RetryResult::NotTried);
    EXPECT_TRUE(d.recovered());
    char text[400];
    describeDmaError(d, text, sizeof(text));
    EXPECT_CONTAINS(std::string(text), "input, the whole block: 8192 bytes at ring offset ");
    EXPECT_CONTAINS(std::string(text), "; system error 87; again from the same buffer: went through; nothing lost");
    EXPECT_EQ(rig.client.lostFrames(), std::uint64_t(0));
    rig.engine.stop();
    EXPECT_TRUE(rig.engine.retiredBuffers().empty());
}

TEST_CASE(AudioEngine, ABadBufferIsReplacedByASpare)
{
    // The buffer is the trouble: its DMA fails wherever it goes. A spare
    // takes its place for good, nothing is lost, and at stop the DMA fails
    // again from the old buffer, with the card stopped.
    Rig rig;
    ASSERT_TRUE(rig.start(128, 256));
    rig.card.failDmasThroughBufferAt(device::Ring::Playback, 0x3FC000u - 64u, 31);   // the block at the end
    rig.run(1200);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.dmaErrors, std::uint64_t(0));
    EXPECT_EQ(s.dmaRecovered, std::uint64_t(1));
    const DmaErrorDetail& d = rig.engine.lastDmaError();
    EXPECT_TRUE(d.output);
    EXPECT_TRUE(d.pastEnd);
    EXPECT_EQ(d.failed, 1u);
    EXPECT_TRUE(d.sameBuffer == RetryResult::Failed);
    EXPECT_EQ(d.sameBufferError, 31u);
    EXPECT_TRUE(d.spareBuffer == RetryResult::WentThrough);
    EXPECT_EQ(rig.card.failedDmas(), std::uint64_t(2));   // once more and it is gone
    EXPECT_EQ(rig.client.lostFrames(), std::uint64_t(0));
    EXPECT_EQ(changes(rig.client.roundTrips()), std::size_t(0));

    rig.engine.stop();
    const std::vector<RetiredBuffer> retired = rig.engine.retiredBuffers();
    ASSERT_EQ(retired.size(), std::size_t(1));
    EXPECT_TRUE(retired[0].output);
    EXPECT_EQ(retired[0].bytes, 128u * 64u);
    EXPECT_EQ(retired[0].systemError, 31u);
    EXPECT_TRUE(retired[0].checked);
    EXPECT_TRUE(retired[0].failsAgain);
    EXPECT_EQ(retired[0].againSystemError, 31u);
}

TEST_CASE(AudioEngine, StartChecksEveryBufferAndSwapsABadOne)
{
    // A buffer the card fails every DMA through, as one on the KONA X failed
    // every small one (cost, 2026-11-10): the first DMA through the rings'
    // middle after the zeroing is start's check of the input buffer.
    Rig rig;
    rig.card.failDmasThroughBufferAt(device::Ring::Playback, 0x1FE000u, 31, 1);
    ASSERT_TRUE(rig.start());
    const BufferCheck& check = rig.engine.bufferCheck();
    EXPECT_EQ(check.bytes, 512u);
    EXPECT_EQ(check.checked, 10u);   // the block's six and the four spares
    EXPECT_EQ(check.failed, 1u);
    EXPECT_EQ(check.replaced, 1u);
    EXPECT_EQ(check.keptBad, 0u);
    EXPECT_EQ(check.sparesLeft, 3u);
    EXPECT_TRUE(check.firstWasOutput);
    EXPECT_EQ(check.systemError, 31u);
    EXPECT_EQ(describeBufferCheck(check),
              std::string("1 of 10 buffers failed a 512-byte DMA before the run (a write to the card, system "
                          "error 31): 1 replaced by a spare that passed; 3 spares left"));

    // The run never meets it.
    rig.run(500);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.dmaErrors, std::uint64_t(0));
    EXPECT_EQ(s.dmaRecovered, std::uint64_t(0));
    EXPECT_TRUE(rig.client.events.empty());
}

TEST_CASE(AudioEngine, StartsCheckSaysNothingWhenAllPass)
{
    Rig rig;
    ASSERT_TRUE(rig.start(32, 8));
    const BufferCheck& check = rig.engine.bufferCheck();
    EXPECT_EQ(check.checked, 10u);
    EXPECT_EQ(check.bytes, 512u);    // within a block of 32 frames (2048 bytes)
    EXPECT_EQ(check.failed, 0u);
    EXPECT_EQ(check.sparesLeft, 4u);
    EXPECT_TRUE(describeBufferCheck(check).empty());
}

TEST_CASE(AudioEngine, SecondTriesStopWhenTheyNeverHelp)
{
    // A DMA that fails wherever it comes from, lap after lap: after three
    // blocks whose second tries all failed, it is not tried again (each try
    // costs a DMA's time).
    Rig rig;
    ASSERT_TRUE(rig.start(128, 256));
    rig.card.failPlaybackWritesAt(0, 87);
    rig.run(2600);   // five laps
    EXPECT_EQ(rig.engine.stats().dmaErrors, std::uint64_t(5));
    EXPECT_EQ(rig.card.failedDmas(), std::uint64_t(3 * 3 + 2));
    EXPECT_TRUE(rig.engine.lastDmaError().retriesOff);
    char text[400];
    describeDmaError(rig.engine.lastDmaError(), text, sizeof(text));
    EXPECT_CONTAINS(std::string(text), "; not tried again (second tries had failed three blocks in a row)");
}

TEST_CASE(AudioEngine, RoundTripIsConstantAndAboveNPlusS)
{
    Rig rig;
    ASSERT_TRUE(rig.start(128, 32));
    rig.run(1000);
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 100000);
    for (const std::int64_t t : trips)
        EXPECT_EQ(t, trips.front());
    // N + S = 128 + 160; the card adds its prefetch, the cable and the
    // conservative head's one frame.
    EXPECT_TRUE(trips.front() > 288 && trips.front() < 288 + 100);
}

TEST_CASE(AudioEngine, EveryOutputSampleComesBack)
{
    Rig rig(64);
    ASSERT_TRUE(rig.start(64, 16));
    rig.run(3000);
    std::int64_t previous = -1;
    std::size_t gaps = 0, counted = 0;
    for (const std::int32_t sample : rig.client.captured)
    {
        const std::uint32_t v = static_cast<std::uint32_t>(sample);
        if (v == 0)
            continue;
        const std::int64_t counter = (v >> 12) & 0xFFFFF;
        if (previous >= 0 && counter != ((previous + 1) & 0xFFFFF))
            ++gaps;
        previous = counter;
        ++counted;
    }
    EXPECT_TRUE(counted > 180000);
    EXPECT_EQ(gaps, std::size_t(0));
}

TEST_CASE(AudioEngine, LateOutputKeepsTheLatency)
{
    Rig rig;
    ASSERT_TRUE(rig.start(128, 32));
    rig.run(100);
    // Two DMAs 6 ms slow put the engine 12 ms (576 frames) behind, against
    // S = 160. Catching up, the next three blocks are late by a block or
    // more and dropped whole; the fourth is late by 32 frames or so and
    // loses those and G = 32 more; the fifth is on time again.
    rig.card.delayNextDmas(2, 6 * kMs);
    rig.run(600);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.lateOutputs, std::uint64_t(4));
    EXPECT_TRUE(count(rig.client.events, EngineEvent::LateOutput, 4));
    EXPECT_TRUE(s.droppedOutputFrames >= 3 * 128 + 64 && s.droppedOutputFrames < 3 * 128 + 96);
    EXPECT_TRUE(s.minOutputMarginFrames < -128);
    EXPECT_EQ(s.outputMoves, std::uint64_t(0));
    EXPECT_EQ(s.inputOverruns, std::uint64_t(0));

    // What was sent came back on time: one round trip throughout, and only
    // the dropped frames missing (a few more at most, those the card was
    // fetching while the rest of the late block was written).
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 80000);
    EXPECT_EQ(changes(trips), std::size_t(0));
    const std::uint64_t lost = rig.client.lostFrames();
    EXPECT_TRUE(lost >= s.droppedOutputFrames && lost < s.droppedOutputFrames + 32);
}

TEST_CASE(AudioEngine, SlowFirstCallbackAddsNoLatency)
{
    // The output is placed when the first block is found complete, not when
    // the host is done with it: a host that takes 5 ms over its first block
    // loses part of that block's output, not 5 ms on every block after.
    Rig quick;
    ASSERT_TRUE(quick.start());
    quick.run(300);
    Rig slow;
    slow.client.sleepAt = 0;
    slow.client.sleepNs = 5 * kMs;
    ASSERT_TRUE(slow.start());
    slow.run(300);

    const std::vector<std::int64_t> quickTrips = quick.client.roundTrips();
    const std::vector<std::int64_t> slowTrips = slow.client.roundTrips();
    ASSERT_TRUE(!quickTrips.empty() && !slowTrips.empty());
    EXPECT_EQ(changes(slowTrips), std::size_t(0));
    const std::int64_t difference = slowTrips.front() - quickTrips.front();
    EXPECT_TRUE(difference >= -16 && difference <= 16);   // an output head step
    const EngineStats s = slow.engine.stats();
    EXPECT_EQ(s.lateOutputs, std::uint64_t(1));
    EXPECT_TRUE(s.droppedOutputFrames > 0 && s.droppedOutputFrames < 128);
    // And the log can say it was the host.
    const LateOutputDetail& d = slow.engine.lastLateOutput();
    EXPECT_TRUE(d.hostNs >= 5 * kMs);
    EXPECT_EQ(d.leadFrames, std::int64_t(160));
    EXPECT_TRUE(d.lateFrames > 0 && d.lateFrames < 128);
    EXPECT_TRUE(d.foundLateFrames < 16);
}

TEST_CASE(AudioEngine, LateWakeUpsAddNoLatency)
{
    // A PC whose timer wakes the engine up to a millisecond late finds every
    // block late, the first one included. The output is placed from when
    // the block became complete, not from when it was found, so the round
    // trip is the punctual one (within the simulated output's 16-frame
    // head steps).
    Rig punctual;
    ASSERT_TRUE(punctual.start());
    punctual.run(500);
    for (const std::uint32_t seed : {3u, 5u, 11u})
    {
        Rig sleepy;
        sleepy.clock.setSleepLateness(sim::SleepLateness{500000, 0.0, 0, seed});
        ASSERT_TRUE(sleepy.start());
        sleepy.run(500);
        const std::vector<std::int64_t> trips = sleepy.client.roundTrips();
        ASSERT_TRUE(!trips.empty());
        EXPECT_EQ(changes(trips), std::size_t(0));
        const std::int64_t difference = trips.front() - punctual.client.roundTrips().front();
        EXPECT_TRUE(difference >= -16 && difference <= 16);
        EXPECT_EQ(sleepy.engine.stats().lateOutputs, std::uint64_t(0));
    }
}

TEST_CASE(AudioEngine, OverrunJumpsToTheNewestDataAndKeepsTheLatency)
{
    Rig rig;
    ASSERT_TRUE(rig.start());
    rig.client.sleepAt = 50;
    rig.client.sleepNs = 1000 * kMs;   // the host hangs for a second
    rig.run(300);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.inputOverruns, std::uint64_t(1));
    EXPECT_TRUE(count(rig.client.events, EngineEvent::InputOverrun, 1));
    EXPECT_EQ(s.blocks, std::uint64_t(300));
    // The sample position jumped by the frames skipped: a second's worth.
    const std::int64_t jump = rig.client.blocks[51].samplePosition - rig.client.blocks[50].samplePosition;
    EXPECT_TRUE(jump > 47000 && jump < 49000);
    // The hung block's output came a second late and was dropped; the
    // output moved as far as the input, so the round trip is what it was.
    EXPECT_EQ(s.lateOutputs, std::uint64_t(1));
    EXPECT_EQ(s.droppedOutputFrames, std::uint64_t(128));
    EXPECT_EQ(s.outputMoves, std::uint64_t(0));
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 30000);
    EXPECT_EQ(changes(trips), std::size_t(0));
}

TEST_CASE(AudioEngine, HangLongerThanTheRingIsCountedInFull)
{
    // Two seconds is a ring and a half (65280 frames): the write head alone
    // reads as 30720 frames behind, under half a ring, which is no overrun
    // at all. Time says how many times it went round.
    Rig rig;
    ASSERT_TRUE(rig.start());
    rig.client.sleepAt = 50;
    rig.client.sleepNs = 2000 * kMs;
    rig.run(300);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.inputOverruns, std::uint64_t(1));
    const std::int64_t jump = rig.client.blocks[51].samplePosition - rig.client.blocks[50].samplePosition;
    EXPECT_TRUE(jump > 95000 && jump < 97000);
    EXPECT_EQ(s.lateOutputs, std::uint64_t(1));
    EXPECT_EQ(s.outputMoves, std::uint64_t(0));
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 30000);
    EXPECT_EQ(changes(trips), std::size_t(0));
}

TEST_CASE(AudioEngine, OutputRestartedBehindItsBackIsPlacedAnew)
{
    Rig rig;
    ASSERT_TRUE(rig.start());
    rig.run(200);
    // Something stops and starts the card's output (AJA's services, say):
    // its head goes back to 0, far from where the input says it must be.
    // Left alone, O would stay half a second ahead of it for good.
    rig.card.stopOutput();
    rig.card.startOutput();
    rig.run(400);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.outputMoves, std::uint64_t(1));
    EXPECT_TRUE(count(rig.client.events, EngineEvent::OutputMoved, 1));
    EXPECT_EQ(s.inputOverruns, std::uint64_t(0));

    // From then on the output comes back, at one new round trip.
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 50000);
    const std::vector<std::int64_t> after(trips.end() - 20000, trips.end());
    EXPECT_EQ(changes(after), std::size_t(0));
    EXPECT_TRUE(after.front() > 288 && after.front() < 288 + 100);
}

TEST_CASE(AudioEngine, InputHeldBackLeavesTheOutputLateUntilItIsPlacedAnew)
{
    // What watch met on the KONA X while the AJA Control Panel changed the
    // input's source (2026-11-11): the capture side lost frames while the
    // playback head went on, by less than what counts as out of step at
    // once, and from then on every block was late, 39848 of 44161, with
    // nothing to bring the output back.
    Rig rig;
    ASSERT_TRUE(rig.start());   // N = 128, G = 32: S = 160
    rig.run(200);
    rig.card.holdInput(400);
    rig.run(700);
    const EngineStats s = rig.engine.stats();
    // Late for a quarter of a second, 93 blocks dropped whole, then placed
    // anew with the 94th: once.
    EXPECT_EQ(s.outputMoves, std::uint64_t(1));
    EXPECT_TRUE(count(rig.client.events, EngineEvent::OutputMoved, 1));
    EXPECT_EQ(s.lateOutputs, std::uint64_t(93));
    EXPECT_EQ(s.droppedOutputFrames, std::uint64_t(93 * 128));
    EXPECT_EQ(s.inputOverruns, std::uint64_t(0));
    EXPECT_EQ(s.stalls, std::uint64_t(0));

    // From then on the output comes back, at the round trip it had.
    const std::vector<std::int64_t> trips = rig.client.roundTrips();
    ASSERT_TRUE(trips.size() > 70000);
    const std::vector<std::int64_t> after(trips.end() - 20000, trips.end());
    EXPECT_EQ(changes(after), std::size_t(0));
    const std::int64_t moved = after.front() - trips.front();
    EXPECT_TRUE(moved > -24 && moved < 24);   // a step of each head at most
}

TEST_CASE(AudioEngine, AHostLateOnEveryBlockIsNotOutOfStep)
{
    // Late by its own time: the playback head is where the input's timing
    // says it is, however late that makes each block, and placing the
    // output anew would change nothing. S = 72 frames is 1.5 ms from a
    // block complete to written; this host takes 2 ms with each.
    Rig rig;
    ASSERT_TRUE(rig.start(128, 8, true));
    rig.client.sleepEveryNs = 2 * kMs;
    rig.run(600);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.blocks, std::uint64_t(600));
    EXPECT_TRUE(s.lateOutputs > 590);
    EXPECT_EQ(s.outputMoves, std::uint64_t(0));
    EXPECT_EQ(s.inputOverruns, std::uint64_t(0));
    EXPECT_TRUE(s.minOutputStepFrames > -24 && s.maxOutputStepFrames < 24);
}

TEST_CASE(AudioEngine, AnOffsetThatMakesNoBlockLateIsLeftAndShows)
{
    // Held back by less than the guard has to spare, no block is late and
    // the output stays where it is: the round trip is shorter than the one
    // reported by that much, and the stats say by how much the playback
    // head is off the input's timing.
    Rig rig;
    ASSERT_TRUE(rig.start(128, 256));   // S = 384
    rig.run(200);
    rig.card.holdInput(200);
    rig.run(600);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.lateOutputs, std::uint64_t(0));
    EXPECT_EQ(s.outputMoves, std::uint64_t(0));
    EXPECT_TRUE(s.minOutputStepFrames > -200 - 24 && s.minOutputStepFrames < -200 + 24);
    EXPECT_TRUE(s.maxOutputStepFrames < 24);
}

TEST_CASE(AudioEngine, StalledInputGivesSilenceThenResumes)
{
    Rig rig;
    ASSERT_TRUE(rig.start());
    rig.run(50);
    rig.card.stallAudioClock(rig.clock.nowNs() + 10 * kMs, 300 * kMs);
    rig.run(400);
    const EngineStats s = rig.engine.stats();
    EXPECT_EQ(s.stalls, std::uint64_t(1));
    EXPECT_TRUE(s.silentBlocks > 60 && s.silentBlocks < 130);
    EXPECT_TRUE(count(rig.client.events, EngineEvent::InputStalled, 1));
    EXPECT_TRUE(count(rig.client.events, EngineEvent::InputResumed, 1));
    EXPECT_EQ(s.inputOverruns, std::uint64_t(0));
    // Silent blocks still come at the PC's pace.
    bool sawSilent = false;
    for (std::size_t i = 1; i < rig.client.blocks.size(); ++i)
    {
        if (rig.client.blocks[i].silent && rig.client.blocks[i - 1].silent)
        {
            sawSilent = true;
            const std::int64_t gap = rig.client.blocks[i].hostTimeNs - rig.client.blocks[i - 1].hostTimeNs;
            EXPECT_TRUE(gap > 2600000 && gap < 2750000);
        }
    }
    EXPECT_TRUE(sawSilent);
    EXPECT_FALSE(rig.client.blocks.back().silent);
}

TEST_CASE(AudioEngine, OutputReadySendsAtOnce)
{
    Rig rig;
    rig.client.outputReadyAt = 20;
    ASSERT_TRUE(rig.start());
    rig.run(200);
    // The block was sent before the client overwrote it.
    for (const std::int32_t sample : rig.client.captured)
        EXPECT_NE(sample, std::int32_t(0x7FFFFF00));
    EXPECT_EQ(rig.engine.stats().lateOutputs, std::uint64_t(0));
}

TEST_CASE(AudioEngine, LastBlockPosition)
{
    Rig rig;
    std::int64_t position = -1, time = -1;
    EXPECT_FALSE(rig.engine.lastBlockPosition(position, time));
    ASSERT_TRUE(rig.start());
    rig.run(10);
    ASSERT_TRUE(rig.engine.lastBlockPosition(position, time));
    EXPECT_EQ(position, std::int64_t(9 * 128));
    EXPECT_EQ(time, rig.client.blocks.back().hostTimeNs);
}

TEST_CASE(AudioEngine, StartChecks)
{
    Rig rig;
    std::string error;
    EngineConfig config;
    config.blockFrames = 8;
    EXPECT_FALSE(rig.engine.start(config, rig.client, error));
    config.blockFrames = 20000;
    EXPECT_FALSE(rig.engine.start(config, rig.client, error));
    config.blockFrames = 128;
    config.audio.channels = 7;   // does not fit the ring
    EXPECT_FALSE(rig.engine.start(config, rig.client, error));
    config.audio.channels = 16;
    EXPECT_TRUE(rig.engine.start(config, rig.client, error));
    EXPECT_FALSE(rig.engine.start(config, rig.client, error));   // already running
}

TEST_CASE(AudioEngine, RequestStop)
{
    Rig rig;
    ASSERT_TRUE(rig.start());
    EXPECT_TRUE(rig.engine.runOnce());
    rig.engine.requestStop();
    EXPECT_FALSE(rig.engine.runOnce());
    rig.engine.stop();
    EXPECT_FALSE(rig.engine.running());
    EXPECT_EQ(rig.card.lockedBufferCount(), std::size_t(0));
}
