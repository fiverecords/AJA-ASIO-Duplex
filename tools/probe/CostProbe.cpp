// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "CostProbe.h"

#include "common/AlignedBuffer.h"
#include "common/TextFormat.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace ajaduplex::probe
{

namespace
{

using device::CardAudio;
using device::Ring;

std::string kilobytes(std::uint32_t bytes)
{
    return bytes % 1024 == 0 ? std::to_string(bytes / 1024) + " KB" : std::to_string(bytes) + " bytes";
}

const CostItem* find(const CostRun& run, const std::string& group, std::uint32_t bytes, bool locked, bool write)
{
    for (const CostItem& item : run.items)
    {
        if (item.group == group && item.bytes == bytes && item.locked == locked && item.write == write &&
            !item.values.empty())
            return &item;
    }
    return nullptr;
}

std::string hexAddress(const void* p)
{
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return text;
}

bool dma(CardAudio& card, bool write, std::uint32_t offset, AlignedBuffer& buffer, std::uint32_t bytes)
{
    return write ? card.dmaWrite(Ring::Playback, offset, buffer.data(), bytes)
                 : card.dmaRead(Ring::Capture, offset, buffer.data(), bytes);
}

/// A buffer DMABufferLock took in this run. On the KONA X one buffer failed
/// only small DMAs, maybe where a buffer of 4 KB had been locked and
/// unlocked before (cost, 2026-11-10): a bad buffer's report says.
struct LockedSpan
{
    std::uintptr_t start;
    std::uint32_t bytes;
};

std::string lockHistory(const std::vector<LockedSpan>& locked, const void* address)
{
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(address);
    for (auto it = locked.rbegin(); it != locked.rend(); ++it)
    {
        if (a >= it->start && a < it->start + it->bytes)
            return std::string(a == it->start ? "where" : "inside where") + " a buffer of " + kilobytes(it->bytes) +
                   " was locked and unlocked earlier in this run";
    }
    return "not where any buffer was locked in this run";
}

/// Which sizes, 64 bytes up to the buffer's, go one way through `b` at
/// `offset`.
std::string sizesThrough(CardAudio& card, bool write, std::uint32_t offset, AlignedBuffer& b)
{
    std::string through, failing;
    std::uint32_t largest = 64;
    for (std::uint32_t bytes = 64; bytes <= b.size() && bytes <= 16384; bytes *= 2)
    {
        if (write)
            std::memset(b.data(), 0, bytes);   // silence wherever it lands
        std::string& list = dma(card, write, offset, b, bytes) ? through : failing;
        list += (list.empty() ? "" : ", ") + std::to_string(bytes);
        largest = bytes;
    }
    const std::string kind = write ? "writes" : "reads";
    const std::string range = "(64 to " + std::to_string(largest) + " bytes)";
    if (failing.empty())
        return kind + " of every size went through " + range;
    if (through.empty())
        return kind + " of every size failed " + range;
    return kind + " of " + through + " bytes went through, of " + failing + " failed";
}

/// Where a DMA of `bytes` goes, by name: [offset, offset + bytes).
struct Position
{
    const char* name;
    std::int64_t offset;   ///< from the ring's start; -1: does not fit
};

std::vector<Position> positions(const device::RingLayout& layout, std::uint32_t bytes)
{
    const std::int64_t end = layout.ringBytes;
    const std::int64_t room = layout.roomBytes;
    const std::int64_t b = bytes;
    const std::int64_t middle = (end / 2) & ~std::int64_t(4095);   // a page boundary, as the end is
    auto fits = [&](std::int64_t offset, std::int64_t length) {
        return offset >= 0 && offset + length <= room ? offset : -1;
    };
    return {
        {"ending at the ring's end", fits(end - b, b)},
        {"ending one frame before the ring's end", fits(end - 64 - b, b)},
        {"crossing the ring's end, half on each side", fits(end - b / 2, b)},
        {"in the room past the ring's end", fits(end, b)},
        {"ending at the room's end", fits(room - b, b)},
        {"starting at the ring's start", fits(0, b)},
        {"ending at a page boundary mid-ring", fits(middle - b, b)},
    };
}

/// The checks of one pass, with the audio system stopped or running.
void checkDmas(CardAudio& card, const CostSettings& settings, bool running, const std::vector<LockedSpan>& locked,
               CostRun& run, const std::function<bool()>& stop)
{
    const device::RingLayout layout = card.layout();
    const std::string state = running ? "audio running" : "audio stopped";
    const std::string atTheEnd = "DMA at the ring's end, " + state;
    const std::uint32_t middle = (layout.ringBytes / 2) & ~4095u;

    // One buffer for writes (zeros: silence wherever it lands) and one for
    // reads, each first shown to go through mid-ring with the smallest
    // size and the largest, so that a failure below is the place's and not
    // the buffer's (in 0.1.16 the reader passed 8 KB mid-ring and failed
    // every read of 512-2624 bytes, wherever it went).
    std::uint32_t largest = 0;
    std::uint32_t smallest = 0;
    for (const std::uint32_t b : settings.checkSizes)
    {
        largest = std::max(largest, b);
        smallest = smallest == 0 ? b : std::min(smallest, b);
    }
    largest += 64;
    std::vector<std::unique_ptr<AlignedBuffer>> candidates;
    AlignedBuffer* writer = nullptr;
    AlignedBuffer* reader = nullptr;
    for (unsigned attempt = 0; attempt < 8 && (writer == nullptr || reader == nullptr); ++attempt)
    {
        candidates.push_back(std::make_unique<AlignedBuffer>(largest));
        AlignedBuffer& b = *candidates.back();
        const bool forWrites = writer == nullptr;
        const bool small = dma(card, forWrites, middle, b, smallest);
        const std::uint32_t smallError = card.lastDmaSystemError();
        const bool large = dma(card, forWrites, middle, b, largest);
        if (small && large)
        {
            (forWrites ? writer : reader) = &b;
            continue;
        }
        run.notes.push_back(std::string("a fresh buffer at ") + hexAddress(b.data()) + " (" + state + "): " +
                            (forWrites ? "a write" : "a read") + " of " + std::to_string(smallest) +
                            " bytes mid-ring " + (small ? "went through" : "failed, system error " +
                                                                               std::to_string(smallError)) +
                            ", of " + std::to_string(largest) + " bytes " +
                            (large ? "went through" : "failed, system error " +
                                                          std::to_string(card.lastDmaSystemError())) +
                            "; " + sizesThrough(card, forWrites, middle, b) + "; " + lockHistory(locked, b.data()) +
                            "; another was taken");
    }
    if (writer == nullptr || reader == nullptr)
    {
        run.notes.push_back("no buffer went through mid-ring (" + state + "): the checks at the ring's end were "
                            "not run");
        return;
    }

    for (const bool write : {true, false})
    {
        for (const std::uint32_t bytes : settings.checkSizes)
        {
            for (std::uint32_t extra : {0u, 64u})
            {
                // 64 more bytes ending 64 past the end: the piece that failed
                // in 0.1.15, made to run on past it.
                const std::vector<Position> places = positions(layout, bytes);
                for (const Position& p : places)
                {
                    if (stop())
                        return;
                    if (extra != 0 && std::string(p.name) != "ending at the ring's end")
                        continue;
                    DmaCheck check;
                    check.group = atTheEnd;
                    check.name = std::string(write ? "write " : "read ") + std::to_string(bytes + extra) + " bytes " +
                                 (extra ? "running 64 bytes past the ring's end" : p.name);
                    if (p.offset < 0)
                    {
                        check.note = "does not fit";
                        run.checks.push_back(check);
                        continue;
                    }
                    for (unsigned i = 0; i < settings.checkRepeats; ++i)
                    {
                        ++check.tries;
                        if (!dma(card, write, static_cast<std::uint32_t>(p.offset), write ? *writer : *reader,
                                 bytes + extra))
                        {
                            ++check.failures;
                            check.systemError = card.lastDmaSystemError();
                        }
                    }
                    run.checks.push_back(check);
                }
            }
        }
    }

    // Many fresh buffers at once, each with a write and a read mid-ring of
    // the smallest size and of 8 KB (writes first, while they hold zeros):
    // a bad one fails wherever it goes, and again when tried again; the
    // first few bad ones say which sizes they pass and their history.
    {
        DmaCheck check;
        check.group = "DMA buffers, " + state;
        check.name = std::to_string(settings.freshBuffers) + " fresh buffers of 8 KB, a write and a read each of " +
                     std::to_string(smallest) + " bytes and of 8 KB, mid-ring";
        std::vector<std::unique_ptr<AlignedBuffer>> buffers;
        buffers.reserve(settings.freshBuffers);
        unsigned described = 0;
        for (unsigned i = 0; i < settings.freshBuffers && !stop(); ++i)
        {
            buffers.push_back(std::make_unique<AlignedBuffer>(8192));
            AlignedBuffer& b = *buffers.back();
            std::string failed;
            bool failedWrite = false, failedRead = false;
            for (const bool write : {true, false})
            {
                for (const std::uint32_t bytes : {smallest, 8192u})
                {
                    ++check.tries;
                    if (dma(card, write, middle, b, bytes))
                        continue;
                    ++check.failures;
                    check.systemError = card.lastDmaSystemError();
                    const bool again = dma(card, write, middle, b, bytes);
                    failed += (failed.empty() ? "" : ", ") + std::string(write ? "write" : "read") + " of " +
                              std::to_string(bytes) + " failed (system error " + std::to_string(check.systemError) +
                              (again ? ", went through when tried again)" : ", failed again)");
                    (write ? failedWrite : failedRead) = true;
                }
            }
            if (failed.empty())
                continue;
            std::string note = "buffer " + std::to_string(i) + " at " + hexAddress(b.data()) + ": " + failed;
            if (described < 3)
            {
                ++described;
                if (failedWrite)
                    note += "; " + sizesThrough(card, true, middle, b);
                if (failedRead)
                    note += "; " + sizesThrough(card, false, middle, b);
                note += "; " + lockHistory(locked, b.data());
            }
            check.note += (check.note.empty() ? "" : "; ") + note;
        }
        run.checks.push_back(check);
    }

    // Plain buffers where a locked one was just unlocked (0.1.15 locked its
    // block buffers and unlocked them at every stop).
    {
        DmaCheck check;
        check.group = "DMA buffers, " + state;
        check.name = std::to_string(settings.reusedBuffers) + " plain buffers of 8 KB, each made right after a "
                     "locked one was unlocked and freed";
        unsigned sameAddress = 0;
        bool lockFailed = false;
        for (unsigned i = 0; i < settings.reusedBuffers && !stop(); ++i)
        {
            const void* lockedAt = nullptr;
            {
                AlignedBuffer lockedBuffer(8192);
                lockedAt = lockedBuffer.data();
                if (card.lockBuffer(lockedBuffer.data(), 8192))
                {
                    dma(card, true, middle, lockedBuffer, 8192);
                    card.unlockBuffer(lockedBuffer.data(), 8192);
                }
                else
                    lockFailed = true;
            }
            AlignedBuffer plain(8192);
            sameAddress += plain.data() == lockedAt;
            ++check.tries;
            if (!dma(card, true, middle, plain, 8192))
            {
                ++check.failures;
                check.systemError = card.lastDmaSystemError();
            }
        }
        check.note = std::to_string(sameAddress) + " of them where the locked one had been" +
                     (lockFailed ? "; DMABufferLock failed for some" : "");
        run.checks.push_back(check);
    }
}

} // namespace

bool runCardCost(CardAudio& card, Clock& clock, const CostSettings& settings,
                 const std::function<bool()>& stopRequested, CostRun& run, std::string& error)
{
    auto stop = [&]() {
        if (!run.interrupted && stopRequested && stopRequested())
            run.interrupted = true;
        return run.interrupted;
    };

    run.description = card.description();
    device::AudioConfig config;
    config.source = device::InputSource::Unchanged;   // the source plays no part in these costs
    if (!card.configure(config, error))
        return false;
    const device::RingLayout layout = card.layout();

    // Register read
    {
        CostItem item;
        item.name = "register read (ReadAudioLastIn)";
        item.group = "register";
        item.values.reserve(settings.registerReads);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < settings.registerReads && !stop(); ++i)
        {
            const std::int64_t t0 = clock.nowNs();
            const bool ok = card.readInputHead(value);
            const std::int64_t t1 = clock.nowNs();
            if (!ok)
            {
                run.notes.push_back("ReadAudioLastIn failed; register cost stopped at " + std::to_string(i));
                break;
            }
            item.values.add(t1 - t0);
        }
        run.items.push_back(std::move(item));
    }

    // DMA: for each size, plain then locked; reads and writes interleaved,
    // walking the rings the way the engine will.
    std::vector<LockedSpan> lockedSpans;
    for (const std::uint32_t bytes : settings.dmaSizes)
    {
        if (bytes == 0 || bytes > layout.ringBytes)
            continue;
        for (const bool locked : {false, true})
        {
            if (stop())
                break;
            AlignedBuffer buffer(bytes);
            if (locked && !card.lockBuffer(buffer.data(), bytes))
            {
                run.notes.push_back("DMABufferLock failed for " + kilobytes(bytes) + ": no locked results at that size");
                continue;
            }
            if (locked)
                lockedSpans.push_back(LockedSpan{reinterpret_cast<std::uintptr_t>(buffer.data()), bytes});
            CostItem readItem, writeItem;
            readItem.group = writeItem.group = "dma";
            readItem.bytes = writeItem.bytes = bytes;
            readItem.locked = writeItem.locked = locked;
            writeItem.write = true;
            readItem.name = "DMA read " + kilobytes(bytes) + (locked ? ", locked" : ", plain");
            writeItem.name = "DMA write " + kilobytes(bytes) + (locked ? ", locked" : ", plain");
            readItem.values.reserve(settings.dmaRepeats);
            writeItem.values.reserve(settings.dmaRepeats);

            const std::uint32_t span = layout.ringBytes - bytes;
            for (unsigned i = 0; i < settings.dmaRepeats && !stop(); ++i)
            {
                const std::uint32_t offset = span ? static_cast<std::uint32_t>((std::uint64_t(i) * bytes) % span) & ~63u : 0u;
                std::int64_t t0 = clock.nowNs();
                const bool readOk = card.dmaRead(Ring::Capture, offset, buffer.data(), bytes);
                std::int64_t t1 = clock.nowNs();
                const bool writeOk = card.dmaWrite(Ring::Playback, offset, buffer.data(), bytes);
                const std::int64_t t2 = clock.nowNs();
                if (!readOk || !writeOk)
                {
                    run.notes.push_back("a DMA of " + kilobytes(bytes) + " failed; that series stopped at " + std::to_string(i));
                    break;
                }
                readItem.values.add(t1 - t0);
                writeItem.values.add(t2 - t1);
            }
            if (locked)
                card.unlockBuffer(buffer.data(), bytes);
            run.items.push_back(std::move(readItem));
            run.items.push_back(std::move(writeItem));
        }
    }

    // Which DMAs go through: stopped, then running (as when the engine
    // moves its blocks).
    checkDmas(card, settings, false, lockedSpans, run, stop);
    if (!stop())
    {
        // The cost runs above wrote what they read: zeros first, so that
        // the running pass plays silence.
        AlignedBuffer zeros(64 * 1024);
        for (std::uint32_t offset = 0; offset < layout.ringBytes; offset += 64 * 1024)
            card.dmaWrite(Ring::Playback, offset, zeros.data(), std::min<std::uint32_t>(64 * 1024, layout.ringBytes - offset));
        if (card.startOutput() && card.startInput())
        {
            clock.sleepForNs(20000000);   // the heads under way
            checkDmas(card, settings, true, lockedSpans, run, stop);
        }
        else
            run.notes.push_back("the audio system did not start: no checks with it running");
        card.stopInput();
        card.stopOutput();
    }
    return true;
}

void runWakeCost(Clock& clock, const CostSettings& settings, const std::function<bool()>& stopRequested,
                 CostRun& run)
{
    for (const std::int64_t period : settings.sleepPeriodsNs)
    {
        if (run.interrupted || (stopRequested && stopRequested()))
        {
            run.interrupted = true;
            break;
        }
        CostItem item;
        item.group = "wake";
        item.periodNs = period;
        item.name = "wake-up lateness, " + formatNs(period) + " sleep";
        item.values.reserve(settings.sleepRepeats);
        for (unsigned i = 0; i < settings.sleepRepeats; ++i)
        {
            if (stopRequested && stopRequested())
            {
                run.interrupted = true;
                break;
            }
            const std::int64_t target = clock.nowNs() + period;
            clock.sleepUntilNs(target);
            item.values.add(clock.nowNs() - target);
        }
        run.items.push_back(std::move(item));
    }
}

std::vector<GuardEstimate> estimateGuard(const CostRun& run)
{
    const CostItem* wake = nullptr;
    for (const CostItem& item : run.items)
    {
        if (item.group == "wake" && !item.values.empty() && (wake == nullptr || item.periodNs == 1000000))
            wake = &item;
    }

    std::vector<GuardEstimate> estimates;
    for (const CostItem& item : run.items)
    {
        if (item.group != "dma" || item.write || item.values.empty())
            continue;
        // One estimate per size: prefer locked buffers, which the engine uses.
        const bool lockedAvailable = find(run, "dma", item.bytes, true, false) && find(run, "dma", item.bytes, true, true);
        if (item.locked != lockedAvailable)
            continue;
        const CostItem* write = find(run, "dma", item.bytes, item.locked, true);
        if (write == nullptr)
            continue;
        GuardEstimate g;
        g.bytes = item.bytes;
        g.frames = item.bytes / 64;   // 16 channels x 4 bytes
        g.dmaReadNs = item.values.percentile(99.9);
        g.dmaWriteNs = write->values.percentile(99.9);
        g.wakeLateNs = wake ? std::max<std::int64_t>(0, wake->values.percentile(99.9)) : 0;
        g.lateBeyondEarlyWakeNs = std::max<std::int64_t>(0, g.wakeLateNs - kEarlyWakeNs);
        g.totalNs = g.dmaReadNs + g.dmaWriteNs + g.lateBeyondEarlyWakeNs + g.pollNs;
        g.samples = static_cast<double>(g.totalNs) * 48000.0 / 1e9;
        estimates.push_back(g);
    }
    std::sort(estimates.begin(), estimates.end(),
              [](const GuardEstimate& a, const GuardEstimate& b) { return a.bytes < b.bytes; });
    return estimates;
}

void addCostReport(const CostRun& run, Report& report)
{
    Report::Section& overview = report.addSection("cost");
    overview.add("device", run.description);
    if (!run.timerDescription.empty())
        overview.add("wake-up timer", run.timerDescription);
    if (run.interrupted)
        overview.add("interrupted", "yes (Ctrl+C): the results are partial");

    Report::Section& registers = report.addSection("register read");
    Report::Section& dma = report.addSection("DMA");
    Report::Section& wake = report.addSection("wake-up");
    for (const CostItem& item : run.items)
    {
        if (item.group == "register")
            registers.add(item.name, summarizeNs(item.values));
        else if (item.group == "dma")
            dma.add(item.name, summarizeNs(item.values));
        else if (item.group == "wake")
        {
            std::string line = summarizeNs(item.values);
            const std::size_t over300us = item.values.countAtOrAbove(300000);
            line += "\nlater than 300 us: " + std::to_string(over300us);
            wake.add(item.name, line);
        }
    }

    Report::Section& guard = report.addSection("guard G, first estimate");
    const std::vector<GuardEstimate> estimates = estimateGuard(run);
    if (estimates.empty())
        guard.add("estimate", "n/a: no DMA or wake-up results");
    for (const GuardEstimate& g : estimates)
    {
        char samples[32];
        std::snprintf(samples, sizeof(samples), "%.1f", g.samples);
        guard.add("N = " + std::to_string(g.frames),
                  formatNs(g.totalNs) + " = " + samples + " samples: DMA read " + formatNs(g.dmaReadNs) +
                      " + write " + formatNs(g.dmaWriteNs) + " + late wake-up beyond " + formatNs(kEarlyWakeNs) +
                      " " + formatNs(g.lateBeyondEarlyWakeNs) + " + poll step " + formatNs(g.pollNs));
    }
    guard.add("how", "p99.9 values; the engine sleeps until " + formatNs(kEarlyWakeNs) +
                         " before a block is due and then polls, so only lateness past that margin counts");
    guard.add("note", "without video traffic; the plugin's DMA comes in step 5");

    // Which DMAs went through, group by group, and in a line what failed.
    // The tables go to the file only; the console gets the short form.
    std::vector<std::string> failed;
    std::vector<std::pair<std::string, Report::Section*>> groups;
    for (const DmaCheck& c : run.checks)
    {
        Report::Section* section = nullptr;
        for (const auto& [title, existing] : groups)
        {
            if (title == c.group)
                section = existing;
        }
        if (section == nullptr)
        {
            section = &report.addSection(c.group, /*detail*/ true);
            groups.emplace_back(c.group, section);
        }
        std::string value;
        if (c.tries == 0)
            value = c.note.empty() ? "not tried" : c.note;
        else
        {
            value = c.failures == 0 ? "went through " + std::to_string(c.tries) + "/" + std::to_string(c.tries)
                                    : "FAILED " + std::to_string(c.failures) + "/" + std::to_string(c.tries) +
                                          ", system error " + std::to_string(c.systemError);
            if (!c.note.empty())
                value += "\n" + c.note;
        }
        section->add(c.name, value);
        if (c.failures != 0)
            failed.push_back(c.name + " (" + c.group.substr(c.group.find(", ") + 2) + ")");
    }
    if (!run.checks.empty())
    {
        std::string all;
        for (const std::string& f : failed)
            all += (all.empty() ? "" : "\n") + f;
        report.addSection("DMA checks, in short")
            .add("failed", failed.empty() ? std::string("none: every DMA tried went through") : all);
    }

    if (!run.notes.empty())
    {
        std::string all;
        for (const std::string& note : run.notes)
            all += (all.empty() ? "" : "\n") + note;
        report.addSection("notes").block(all);
    }
}

std::string costCsv(const CostRun& run)
{
    std::string csv = "item,bin_lower_ns,bin_upper_ns,count\n";
    char line[160];
    for (const CostItem& item : run.items)
    {
        for (const Distribution::Bin& bin : item.values.logBins(10))
        {
            std::snprintf(line, sizeof(line), "\"%s\",%lld,%lld,%zu\n", item.name.c_str(),
                          static_cast<long long>(bin.lower), static_cast<long long>(bin.upper), bin.count);
            csv += line;
        }
    }
    return csv;
}

} // namespace ajaduplex::probe
