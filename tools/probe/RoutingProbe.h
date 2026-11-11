// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    RoutingProbe.h
 * @brief   duplex-probe routing: where the Xpand's analog output takes its
 *          signal from (Q4), what the analog input replaces (Q5), and what
 *          the select's other values do (Q14). Phase 0, step 4b.
 *
 * Each of the 16 outputs of the audio system plays a tone of its own, and
 * each input is asked which tones it hears. The tones are 25 Hz times a
 * prime: whole periods in the playback ring (65280 frames), so the card
 * loops the ring by itself with nothing to feed it, and whole periods in
 * 40 ms, so a few windows of 1920 frames measure all of them exactly; no
 * tone is a harmonic, a sum or a difference of others.
 *
 * With AES out 1-8 looped to AES in 1-8 and XLR out L/R looped to XLR in
 * L/R, three set-ups answer Q4 and Q5:
 *
 *  - input AES: which AES inputs are cabled (each hears its own output);
 *  - input analog, the monitor on 15-16: inputs 1-2 hear outputs 1-2 if
 *    the analog output follows AES 1-2, outputs 15-16 if it follows the
 *    monitor; AES in 3-8 still hear AES, or nothing, after the analog took
 *    over (Q5);
 *  - input analog, the monitor on 1-2: the same check from the other side.
 *
 * With --scan-select, the select is then written with every value 0-15
 * (Q14) and the inputs heard each time.
 */

#pragma once

#include "common/Clock.h"
#include "common/Report.h"
#include "device/CardAudio.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ajaduplex::probe
{

struct RoutingSignal
{
    static constexpr unsigned kChannels = 16;
    static constexpr double kToneDbfs = -20.0;
    static constexpr std::size_t kWindowFrames = 1920;   ///< 40 ms: whole periods of every tone

    /// The tone of output channel `channel` (0-15), in Hz.
    static unsigned toneHz(unsigned channel);

    /// Output channel `channel` at frame `frame`, as the card takes it.
    static std::int32_t sample(std::int64_t frame, unsigned channel);
};

/// The levels of the 16 tones in a stretch of one input channel.
struct ToneLevels
{
    static constexpr double kPresentDbfs = -50.0;   ///< 30 dB under the tones

    std::array<double, RoutingSignal::kChannels> dbfs{};   ///< per output channel's tone
    double residualDbfs = -200.0;                          ///< all that is not a tone, AES17 dBFS

    /// The output channels whose tone is there, loudest first.
    std::vector<unsigned> heard() const;
};

/// `frames` (a multiple of RoutingSignal::kWindowFrames) of input channel
/// `channel`, out of interleaved frames of `channels` channels.
ToneLevels measureTones(const std::int32_t* interleaved, unsigned channels, unsigned channel, std::size_t frames);

struct RoutingCapture
{
    std::string name;
    bool done = false;
    std::string error;
    unsigned selectValue = 0;          ///< the Xpand's select while it was taken
    std::vector<ToneLevels> inputs;    ///< inputs 1-8 (AES in 1-8, the analog in on 1-2)
};

struct RoutingRun
{
    std::string description;
    bool haveAnalogLevel = false;
    int analogLevelDbu = 0;
    bool haveSelect = false;
    unsigned selectBefore = 0;
    RoutingCapture aes;                ///< input AES
    RoutingCapture analogMonitor16;    ///< input analog, the monitor on 15-16
    RoutingCapture analogMonitor12;    ///< input analog, the monitor on 1-2
    bool scanned = false;
    std::vector<RoutingCapture> scan;  ///< --scan-select: values 0-15
    bool interrupted = false;
    bool oemTasks = true;              ///< the card held in OEM task mode meanwhile
};

/// The three set-ups, and the select scan if asked, with the card in OEM
/// task mode unless `oemTasks` is false (AJA's services leave the routing
/// alone then). Leaves the audio system stopped; the caller restore()s the
/// card, which puts the task mode back too.
RoutingRun runRouting(device::CardAudio& card, Clock& clock, bool scanSelect,
                      const std::function<bool()>& stopRequested, bool oemTasks = true);

// The answers, for the report and the tests
enum class AnalogOutSource
{
    Unknown,   ///< the set-ups did not run
    Aes12,     ///< the analog output plays AES out 1-2
    Monitor,   ///< it plays the monitor
    Both,      ///< it heard both at once
    Nothing,   ///< the analog input heard no output at all
};
AnalogOutSource analogOutSource(const RoutingRun& run);

enum class AnalogInReplaces
{
    Unknown,   ///< AES in 3-8 were not cabled, or the set-ups did not run
    Aes12,     ///< only AES in 1-2: 3-8 still hear AES
    AllAes,    ///< every AES input: 3-8 hear nothing
    Some,      ///< some of 3-8, not all
};
AnalogInReplaces analogInReplaces(const RoutingRun& run);

/// What input `input` (0-7) heard in `capture`, short: "A" its own AES
/// output, "L" or "R" the analog output's left or right, "-" nothing, "o5"
/// output 5's tone, several joined with "+".
std::string inputLabel(const RoutingRun& run, const RoutingCapture& capture, unsigned input);

void addRoutingReport(const RoutingRun& run, Report& report);
std::string routingCsv(const RoutingRun& run);

} // namespace ajaduplex::probe
