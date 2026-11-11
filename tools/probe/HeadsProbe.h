// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    HeadsProbe.h
 * @brief   duplex-probe heads: how the card's audio heads move (phase 0, step 2).
 *
 * Runs on one audio system, with no cables, in this order:
 *
 *  1. Configure it (16 channels, 48 kHz, 4 MB buffers, AES input with the
 *     Xpand's analog/AES select kept as it was), fill the capture ring with
 *     a sentinel word and the playback ring with an inaudible marker, start
 *     output, then input.
 *  2. Last byte or next (Q1): during the first lap of the capture ring, read
 *     LastIn, DMA the words around it and read LastIn again. A sentinel
 *     still at the head means the head points at the next byte to write.
 *  3. Erase mode: half a lap in, the playback ring behind LastOut must be
 *     zero and the part not played yet still the marker.
 *  4. Sampling (Q1, Q16): LastIn, LastOut and the 48 kHz counter in a tight
 *     loop for a few seconds; every change is kept with its time.
 *  5. Events (Q6): eAudio, eAudioInWrap and eAudioOutWrap, each subscribed
 *     and waited for in turn.
 *  6. Capture enable (Q13): input restarted with capture enable off; does
 *     the head move, and does anything reach memory?
 *
 * The procedure only uses CardAudio and Clock, so it runs the same against
 * the simulated card. Putting the audio system back is the caller's job
 * (CardAudio::restore).
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

/// Written into the capture ring before starting. Real samples carry 24-bit
/// audio in the upper bits, so a word with a non-zero low byte never comes
/// from the card.
constexpr std::uint32_t kSentinel = 0x5A5A5A5Au;

/// Written into the playback ring, which the card plays: one step of 24-bit
/// audio (-138 dBFS), inaudible on any output that follows this audio
/// system, yet not zero, so erase mode can be told apart.
constexpr std::uint32_t kPlaybackMarker = 0x00000100u;

struct HeadSample
{
    std::int64_t tNs = 0;          ///< clock time just before the three reads (LastIn is read first)
    std::uint32_t input = 0;       ///< LastIn
    std::uint32_t output = 0;      ///< LastOut
    std::uint32_t counter = 0;     ///< raw 48 kHz counter
};

struct SentinelTrial
{
    std::uint32_t headBefore = 0;  ///< LastIn before the DMA
    std::uint32_t headAfter = 0;   ///< LastIn after it
    int wordAtHead = -1;           ///< word at headBefore & ~3: 1 written, 0 sentinel, -1 not in the window
    int wordAfterHead = -1;        ///< the word after it, same coding
    std::int64_t dmaNs = 0;
};

struct EraseCheck
{
    bool done = false;
    std::uint32_t head = 0;        ///< LastOut when checked
    unsigned behindZero = 0, behindMarker = 0, behindOther = 0;
    unsigned aheadZero = 0, aheadMarker = 0, aheadOther = 0;
};

struct CaptureEnableCheck
{
    bool done = false;
    bool headMoved = false;
    unsigned written = 0;          ///< words no longer the sentinel
    unsigned untouched = 0;
};

struct EventCheck
{
    device::CardEvent event = device::CardEvent::Audio;
    bool subscribed = false;
    unsigned arrived = 0;
    unsigned timeouts = 0;
    std::vector<std::int64_t> arrivalsNs;
    bool haveCounts = false;
    std::uint32_t countBefore = 0;
    std::uint32_t countAfter = 0;
    std::int64_t durationNs = 0;
};

struct HeadsSettings
{
    double sampleSeconds = 10.0;
    double eventSeconds = 3.0;
    unsigned sentinelTrials = 2000;
    std::size_t maxSamples = 4000000;
    std::size_t maxCostValues = 1000000;
};

struct HeadsRun
{
    std::string description;
    device::RingLayout layout;
    std::int64_t sampleStartNs = 0;
    std::int64_t sampleEndNs = 0;
    std::vector<HeadSample> samples;
    Distribution readCost;          ///< one register read, from the sampling loop
    std::vector<SentinelTrial> trials;
    EraseCheck erase;
    CaptureEnableCheck captureEnable;
    std::vector<EventCheck> events;
    bool interrupted = false;
    std::vector<std::string> notes; ///< problems that did not stop the run
};

/// Runs the steps above on an open, stopped audio system. Returns false with
/// `error` set when a step the rest depends on fails (configure, the first
/// DMA, start). `stopRequested` is polled throughout; when it returns true
/// the run ends early with run.interrupted set and what it has so far.
bool runHeads(device::CardAudio& card, Clock& clock, const HeadsSettings& settings,
              const std::function<bool()>& stopRequested, HeadsRun& run, std::string& error);

// ---------------------------------------------------------------------------
// Analysis (pure functions over a HeadsRun)
// ---------------------------------------------------------------------------

struct HeadMotion
{
    std::size_t changes = 0;
    std::uint64_t advancedBytes = 0;
    double elapsedSeconds = 0.0;
    double framesPerSecond = 0.0;
    double ppm = 0.0;                                              ///< against 48 kHz
    std::vector<std::pair<std::uint32_t, std::size_t>> steps;      ///< bytes -> count, most common first
    std::vector<std::pair<std::uint32_t, std::size_t>> phases;     ///< value % frameBytes -> count
    Distribution intervalsNs;
    std::size_t backwardSteps = 0;
    std::uint32_t maxValue = 0;
};

HeadMotion analyzeHead(const std::vector<HeadSample>& samples, bool input, const device::RingLayout& layout);

struct CounterMotion
{
    bool valid = false;
    double ticksPerSecond = 0.0;
    double ppm = 0.0;
    double inputFramesPerTick = 0.0;   ///< input head frames per counter tick over the same span
};

CounterMotion analyzeCounter(const std::vector<HeadSample>& samples, const device::RingLayout& layout);

struct SentinelSummary
{
    std::size_t trials = 0;
    std::size_t atHeadWritten = 0;
    std::size_t atHeadSentinel = 0;
    std::size_t afterHeadSentinel = 0;
    std::size_t outsideWindow = 0;
    Distribution dmaNs;
    std::string conclusion;
};

SentinelSummary summarizeSentinel(const std::vector<SentinelTrial>& trials);

std::string eraseConclusion(const EraseCheck& check);
std::string captureEnableConclusion(const CaptureEnableCheck& check);

/// Report sections for the run, in the order of the steps.
void addHeadsReport(const HeadsRun& run, Report& report);

/// The kept samples, at most `maxRows` of them from the start (the summary
/// uses them all): "t_ns,last_in,last_out,counter", times from the start of
/// sampling.
constexpr std::size_t kHeadsCsvRows = 250000;
std::string headsCsv(const HeadsRun& run, std::size_t maxRows = kHeadsCsvRows);

} // namespace ajaduplex::probe
