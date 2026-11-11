// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "CostProbe.h"
#include "sim/FakeCardAudio.h"

using namespace ajaduplex;
using namespace ajaduplex::probe;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

CostSettings smallSettings()
{
    CostSettings s;
    s.registerReads = 1000;
    s.dmaRepeats = 100;
    s.sleepRepeats = 50;
    return s;
}

const CostItem* item(const CostRun& run, const std::string& name)
{
    for (const CostItem& i : run.items)
    {
        if (i.name == name)
            return &i;
    }
    return nullptr;
}

} // namespace

TEST_CASE(CostProbe, CardCostsMatchTheModel)
{
    VirtualClock clock;
    FakeCardModel model;   // no jitter: every value is exact
    FakeCardAudio card(clock, model);
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    EXPECT_TRUE(run.notes.empty());

    const CostItem* reg = item(run, "register read (ReadAudioLastIn)");
    ASSERT_TRUE(reg != nullptr);
    EXPECT_EQ(reg->values.count(), std::size_t(1000));
    EXPECT_EQ(reg->values.percentile(50), model.registerReadNs);

    const CostItem* plain = item(run, "DMA read 4 KB, plain");
    const CostItem* locked = item(run, "DMA write 16 KB, locked");
    ASSERT_TRUE(plain != nullptr && locked != nullptr);
    EXPECT_EQ(plain->values.count(), std::size_t(100));
    EXPECT_EQ(plain->values.max(), model.dmaSetupNs + 1024);
    EXPECT_EQ(locked->values.min(), model.dmaSetupLockedNs + 4096);
    EXPECT_EQ(card.lockedBufferCount(), std::size_t(0));   // every lock was undone
}

TEST_CASE(CostProbe, WakeLatenessOnAVirtualClockIsZero)
{
    VirtualClock clock;
    CostRun run;
    runWakeCost(clock, smallSettings(), {}, run);
    ASSERT_EQ(run.items.size(), std::size_t(3));
    for (const CostItem& i : run.items)
    {
        EXPECT_EQ(i.values.count(), std::size_t(50));
        EXPECT_EQ(i.values.max(), std::int64_t(0));
    }
}

TEST_CASE(CostProbe, GuardEstimateUsesLockedBuffers)
{
    VirtualClock clock;
    FakeCardModel model;
    FakeCardAudio card(clock, model);
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    runWakeCost(clock, smallSettings(), {}, run);

    const std::vector<GuardEstimate> g = estimateGuard(run);
    ASSERT_EQ(g.size(), std::size_t(3));
    EXPECT_EQ(g[0].frames, 64u);
    EXPECT_EQ(g[1].frames, 128u);
    EXPECT_EQ(g[2].frames, 256u);
    EXPECT_EQ(g[1].dmaReadNs, model.dmaSetupLockedNs + 2048);
    EXPECT_EQ(g[1].lateBeyondEarlyWakeNs, std::int64_t(0));   // the virtual clock is never late
    EXPECT_EQ(g[1].totalNs, 2 * (model.dmaSetupLockedNs + 2048) + kPollStepNs);
    EXPECT_TRUE(g[1].samples > 6.1 && g[1].samples < 6.2);   // 128 us at 48 kHz
}

TEST_CASE(CostProbe, ReportAndCsv)
{
    VirtualClock clock;
    FakeCardModel model;
    model.jitterNs = 3000;
    FakeCardAudio card(clock, model);
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    runWakeCost(clock, smallSettings(), {}, run);

    Report report;
    addCostReport(run, report);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "[register read]");
    EXPECT_CONTAINS(text, "DMA read 8 KB, locked");
    EXPECT_CONTAINS(text, "[guard G, first estimate]");
    EXPECT_CONTAINS(text, "N = 128");

    const std::string csv = costCsv(run);
    EXPECT_EQ(csv.rfind("item,bin_lower_ns,bin_upper_ns,count\n", 0), std::size_t(0));
    EXPECT_CONTAINS(csv, "\"DMA write 4 KB, plain\",");
}

TEST_CASE(CostProbe, StopRequest)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    CostRun run;
    std::string error;
    int calls = 0;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), [&calls] { return ++calls > 200; }, run, error));
    EXPECT_TRUE(run.interrupted);
    EXPECT_EQ(card.lockedBufferCount(), std::size_t(0));
}

namespace
{

const DmaCheck* check(const CostRun& run, const std::string& group, const std::string& name)
{
    for (const DmaCheck& c : run.checks)
    {
        if (c.group == group && c.name == name)
            return &c;
    }
    return nullptr;
}

} // namespace

TEST_CASE(CostProbe, EveryDmaCheckGoesThroughOnAGoodCard)
{
    VirtualClock clock;
    FakeCardAudio card(clock);
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    ASSERT_TRUE(run.checks.size() > 40u);
    for (const DmaCheck& c : run.checks)
    {
        EXPECT_EQ(c.failures, 0u);
        EXPECT_TRUE(c.tries > 0u);   // every place fits in the KONA X's room
    }
    const DmaCheck* end = check(run, "DMA at the ring's end, audio running", "write 2560 bytes ending at the ring's end");
    ASSERT_TRUE(end != nullptr);
    EXPECT_EQ(end->tries, 3u);
    EXPECT_TRUE(check(run, "DMA at the ring's end, audio stopped", "read 2624 bytes running 64 bytes past the ring's end"));
    EXPECT_TRUE(check(run, "DMA at the ring's end, audio stopped", "write 8192 bytes in the room past the ring's end"));
    const DmaCheck* reused = check(run, "DMA buffers, audio stopped",
                                   "32 plain buffers of 8 KB, each made right after a locked one was unlocked and freed");
    ASSERT_TRUE(reused != nullptr);
    EXPECT_EQ(reused->tries, 32u);
    EXPECT_EQ(card.lockedBufferCount(), std::size_t(0));

    Report report;
    addCostReport(run, report);
    EXPECT_CONTAINS(report.render(false), "none: every DMA tried went through");
    EXPECT_FALSE(report.render(false).find("[DMA at the ring's end, audio stopped]") != std::string::npos);
    const std::string text = report.render(true);
    EXPECT_CONTAINS(text, "[DMA at the ring's end, audio stopped]");
    EXPECT_CONTAINS(text, "[DMA buffers, audio running]");
    EXPECT_CONTAINS(text, "went through 3/3");
}

TEST_CASE(CostProbe, AFailingPlaceIsNamed)
{
    // 0.1.15's failing piece: 2560 bytes ending at the ring's end.
    VirtualClock clock;
    FakeCardAudio card(clock);
    card.failPlaybackWritesAt(0x3FC000u - 2560u, 31);
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    const DmaCheck* end = check(run, "DMA at the ring's end, audio stopped", "write 2560 bytes ending at the ring's end");
    ASSERT_TRUE(end != nullptr);
    EXPECT_EQ(end->failures, 3u);
    EXPECT_EQ(end->systemError, 31u);
    const DmaCheck* past =
        check(run, "DMA at the ring's end, audio running", "write 2624 bytes running 64 bytes past the ring's end");
    ASSERT_TRUE(past != nullptr);
    EXPECT_EQ(past->failures, 3u);   // the fake fails every write from that offset
    const DmaCheck* read = check(run, "DMA at the ring's end, audio stopped", "read 2560 bytes ending at the ring's end");
    ASSERT_TRUE(read != nullptr);
    EXPECT_EQ(read->failures, 0u);

    Report report;
    addCostReport(run, report);
    const std::string text = report.render(false);
    EXPECT_CONTAINS(text, "write 2560 bytes ending at the ring's end (audio stopped)");
    EXPECT_CONTAINS(text, "write 2560 bytes ending at the ring's end (audio running)");
    EXPECT_CONTAINS(report.render(true), "FAILED 3/3, system error 31");
}

TEST_CASE(CostProbe, ABadBufferIsFoundAmongTheFreshOnes)
{
    // A buffer that fails wherever it goes: the sixth fresh one (after the
    // write buffer's own two checks mid-ring, and two writes through each
    // fresh one before it).
    VirtualClock clock;
    FakeCardAudio card(clock);
    card.failDmasThroughBufferAt(device::Ring::Playback, 0x1FE000u, 31, 12);
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    const DmaCheck* fresh = check(run, "DMA buffers, audio stopped",
                                  "128 fresh buffers of 8 KB, a write and a read each of 512 bytes and of 8 KB, mid-ring");
    ASSERT_TRUE(fresh != nullptr);
    EXPECT_EQ(fresh->tries, 512u);
    EXPECT_EQ(fresh->failures, 4u);   // its writes and its reads
    EXPECT_CONTAINS(fresh->note, "buffer 5 at 0x");
    EXPECT_CONTAINS(fresh->note, ": write of 512 failed (system error 31, failed again), write of 8192 failed");
    EXPECT_CONTAINS(fresh->note, "read of 8192 failed (system error 31, failed again)");
    EXPECT_CONTAINS(fresh->note, "writes of every size failed (64 to 8192 bytes)");
    EXPECT_CONTAINS(fresh->note, "reads of every size failed (64 to 8192 bytes)");
    EXPECT_CONTAINS(fresh->note, "locked");   // where a buffer was locked before, or not
    const DmaCheck* end = check(run, "DMA at the ring's end, audio stopped", "write 2560 bytes ending at the ring's end");
    ASSERT_TRUE(end != nullptr);
    EXPECT_EQ(end->failures, 0u);
}

TEST_CASE(CostProbe, ABufferThatFailsOnlySmallDmasIsCaughtBeforeTheChecks)
{
    // 0.1.16 on the KONA X: the read buffer passed 8 KB mid-ring, then
    // failed every read of 512-2624 bytes wherever it went. Now it is shown
    // with the smallest size too, and its sizes are said.
    VirtualClock clock;
    FakeCardAudio card(clock);
    card.failDmasThroughBufferAt(device::Ring::Capture, 0x1FE000u, 31, 0, 4096);   // the reader's first check
    CostRun run;
    std::string error;
    ASSERT_TRUE(runCardCost(card, clock, smallSettings(), {}, run, error));
    std::string note;   // the first: the running pass may get the same memory back, and the same note
    for (const std::string& n : run.notes)
    {
        if (note.empty() && n.find("a fresh buffer at 0x") == 0)
            note = n;
    }
    EXPECT_CONTAINS(note, "(audio stopped): a read of 512 bytes mid-ring failed, system error 31, of 8256 bytes "
                          "went through");
    EXPECT_CONTAINS(note, "reads of 8192 bytes went through, of 64, 128, 256, 512, 1024, 2048, 4096 failed");
    EXPECT_CONTAINS(note, "another was taken");
    const DmaCheck* end = check(run, "DMA at the ring's end, audio stopped", "read 512 bytes ending at the ring's end");
    ASSERT_TRUE(end != nullptr);
    EXPECT_EQ(end->failures, 0u);   // the reader taken instead
}
