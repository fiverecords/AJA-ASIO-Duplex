// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CostProbe.h
 * @brief   duplex-probe cost: what one engine cycle's pieces cost (phase 0, step 2).
 *
 *  - A register read (ReadAudioLastIn), many times (Q2).
 *  - DMA reads from the capture ring and writes to the playback ring of
 *    4, 8 and 16 KB (one block of 64, 128 and 256 frames of 16 channels),
 *    with plain and with page-locked host buffers (Q2).
 *  - How late the wake-up timer is, sleeping 0.5, 1 and 2 ms (in the
 *    command: MMCSS "Pro Audio" and the high-resolution waitable timer, as
 *    the engine will use them).
 *
 * From the worst of these comes a first estimate of the output guard G:
 * one DMA read, one DMA write and a late wake-up must fit in it. Video
 * traffic is not part of this run (that is step 5, with the plugin).
 *
 * Then which DMAs go through at all, with the audio system stopped and
 * again running (0.1.15 on the KONA X: the piece of a block that ended at
 * the ring's end failed every lap; the sweep's DMA errors came and went
 * from run to run):
 *  - writes to the playback ring and reads from the capture ring of 512,
 *    2560 and 8192 bytes at the ring's end, ending there, crossing it,
 *    past it in the room the card leaves alone, at the ring's start, and
 *    for comparison one frame before the end and in the middle;
 *  - many fresh buffers, a DMA each, to catch one that fails wherever it
 *    goes; and plain buffers where locked ones were just unlocked.
 */

#pragma once

#include "common/Clock.h"
#include "common/Report.h"
#include "common/Stats.h"
#include "device/CardAudio.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ajaduplex::probe
{

struct CostSettings
{
    unsigned registerReads = 100000;
    unsigned dmaRepeats = 5000;
    std::vector<std::uint32_t> dmaSizes = {4096, 8192, 16384};
    std::vector<std::int64_t> sleepPeriodsNs = {500000, 1000000, 2000000};
    unsigned sleepRepeats = 2000;
    // Which DMAs go through
    std::vector<std::uint32_t> checkSizes = {512, 2560, 8192};
    unsigned checkRepeats = 3;
    unsigned freshBuffers = 128;       ///< of 8 KB, a write and a read each
    unsigned reusedBuffers = 32;       ///< plain buffers allocated where a locked one was just unlocked
};

/// One kind of DMA tried a few times: how many went through.
struct DmaCheck
{
    std::string group;             ///< e.g. "DMA at the ring's end, audio stopped"
    std::string name;              ///< e.g. "write 2560 bytes ending at the ring's end"
    unsigned tries = 0;
    unsigned failures = 0;
    std::uint32_t systemError = 0; ///< of the last failure
    std::string note;              ///< what else there is to say (which buffers failed)
};

struct CostItem
{
    std::string name;       ///< e.g. "DMA read 8 KB, locked"
    std::string group;      ///< "register", "dma", "wake"
    std::uint32_t bytes = 0;
    bool locked = false;
    bool write = false;
    std::int64_t periodNs = 0;
    Distribution values;    ///< nanoseconds
};

struct CostRun
{
    std::string description;
    std::string timerDescription;   ///< which timer and thread priority the wake-up test used
    std::vector<CostItem> items;
    std::vector<DmaCheck> checks;
    bool interrupted = false;
    std::vector<std::string> notes;
};

/// Register and DMA costs on an open, stopped audio system (configured for
/// 16 channels; the input source is left alone), then the DMA checks, the
/// second time with the audio system running (it is left stopped). Returns
/// false with `error` when configure fails.
bool runCardCost(device::CardAudio& card, Clock& clock, const CostSettings& settings,
                 const std::function<bool()>& stopRequested, CostRun& run, std::string& error);

/// Wake-up lateness on `clock`: sleep until now + period, then measure how
/// far past the target the clock is. The caller sets up the thread (MMCSS)
/// and the clock's timer.
void runWakeCost(Clock& clock, const CostSettings& settings, const std::function<bool()>& stopRequested,
                 CostRun& run);

/// The design's wake-up scheme: sleep until about 300 us before the block
/// is due, then poll the head every 50-100 us.
constexpr std::int64_t kEarlyWakeNs = 300000;
constexpr std::int64_t kPollStepNs = 100000;

struct GuardEstimate
{
    unsigned frames = 0;          ///< block size N
    std::uint32_t bytes = 0;      ///< DMA size for N frames of 16 channels
    std::int64_t dmaReadNs = 0;   ///< p99.9 with locked buffers (plain if no locked result)
    std::int64_t dmaWriteNs = 0;
    std::int64_t wakeLateNs = 0;  ///< p99.9 lateness of a 1 ms sleep
    std::int64_t lateBeyondEarlyWakeNs = 0;   ///< what of it the early wake-up does not absorb
    std::int64_t pollNs = kPollStepNs;
    std::int64_t totalNs = 0;
    double samples = 0.0;         ///< totalNs at 48 kHz
};

/// One estimate per DMA size measured: G must cover a DMA read, a DMA
/// write, the part of a late wake-up beyond the early-wake margin, and one
/// polling step.
std::vector<GuardEstimate> estimateGuard(const CostRun& run);

void addCostReport(const CostRun& run, Report& report);

/// "item,bin_lower_ns,bin_upper_ns,count", 10 log bins per decade.
std::string costCsv(const CostRun& run);

} // namespace ajaduplex::probe
