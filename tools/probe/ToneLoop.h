// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    ToneLoop.h
 * @brief   The tone check of duplex-probe loop: XLR out L/R looped to XLR
 *          in L/R through the Xpand's converters, and (since 0.1.17) AES
 *          out looped to AES in through the KONA X's sample-rate converter.
 *
 * Converters change every sample, and so does the sample-rate converter
 * on the KONA X's AES inputs (AJA: "synchronous or nonsynchronous, internal
 * sample rate conversion"; the words came back changed, 2026-11-10), so
 * neither loop can be checked word by word. Each output instead plays a
 * tone of its own (997 Hz on channel 1, L or AES 1; 1499 Hz on 2, R or
 * AES 2; up to 4507 Hz on AES 8) after a short sweep, and each input is
 * checked for what a glitch does to a tone:
 *
 *  - Per sample, x[n] - 2 cos(w) x[n-1] + x[n-2] is zero for a pure tone
 *    whatever its level and phase, and jumps at any discontinuity: a gap of
 *    silence, a lost or repeated frame, a click.
 *  - Per second (a whole number of periods of every tone), a least-squares
 *    fit gives the tone's level and phase and what is left: noise and
 *    distortion. The loop is synchronous, so the phase must not move by
 *    whole frames: such a move is frames lost or repeated. A slip is a
 *    whole number of frames k, and the phase moves by 2 pi (Hz x k mod
 *    48000) / 48000; the tones' frequencies are prime to 48000, so a move
 *    measured to better than 2 pi / 96000 gives k exactly (modulo a
 *    second).
 *  - The delay is not quite still, though: on the KONA X the analog loop's
 *    drifted by about 2 us in 10 s (2026-11-10), the same on L and R, and
 *    through the AES inputs' converter a tone came back with about 4.5 us
 *    of jitter (the routing probe's residual, 6 dB per octave). Each
 *    second's phase is held against the last still second's, moved on by
 *    the drift so far; what drifts is added up and reported, and so is
 *    what is left, the wander. A second with a glitch in it is not measured
 *    (a burst of something else moves its phase); after one, a move of
 *    more than a fortieth of a frame is a slip, without one more than a
 *    quarter of a frame, and never less than eight times the median
 *    wander. A slip whose move is smaller (a whole number of the tone's
 *    periods, nearly) and that the per-sample check missed counts as drift
 *    on that tone, and another tone, the pair's, sees it.
 *  - After the run, the sweep's position in the input (cross-correlation)
 *    gives the round trip in frames, with a fraction, and the polarity.
 *
 * The analog round trip is card, converters and cable: what the driver
 * must report to hosts for the XLR input (Q12). Through AES the round trip
 * includes the AES inputs' converter.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace ajaduplex::probe
{

struct ToneSignal
{
    static constexpr unsigned kChannels = 2;          ///< L and R: outputs 1-2, inputs 1-2
    static constexpr unsigned kMaxTones = 8;          ///< AES 1-8: one tone each
    static constexpr double kToneDbfs = -20.0;        ///< analog: +4 dBu with the Xpand at +24 dBu
    /// AES: the KONA X's analog output plays AES out 1-2 too (Q4), so
    /// quieter, -16 dBu there with the Xpand at +24 dBu; a converter's
    /// jitter scales with the tone, so the check is as fine.
    static constexpr double kAesToneDbfs = -40.0;
    static constexpr std::int64_t kSweepStart = 2400; ///< output frame: 50 ms in
    static constexpr std::int64_t kSweepFrames = 1024;
    static constexpr double kSweepFromHz = 300.0;
    static constexpr double kSweepToHz = 12000.0;
    static constexpr std::int64_t kToneStart = 4800;  ///< 100 ms in
    static constexpr std::int64_t kFadeFrames = 480;  ///< 10 ms fade-in

    /// The tone of a channel (0 = L or AES 1, 1 = R or AES 2, up to AES 8),
    /// in Hz: a whole number, so a second holds a whole number of periods,
    /// and prime, so no slip shorter than a second leaves its phase alone.
    static unsigned toneHz(unsigned channel);

    /// Output channel `channel` at output frame `frame`, as the card takes
    /// it: the sweep and the tone at `toneDbfs`.
    static std::int32_t sample(std::int64_t frame, unsigned channel, double toneDbfs = kToneDbfs);

    /// The same in parts, for the engine's thread: the signal at kToneDbfs
    /// in full-scale units, what to scale it by for another level (once,
    /// beforehand: it takes a pow), and the word for a value.
    static double value(std::int64_t frame, unsigned channel);
    static double scaleFor(double toneDbfs);
    static std::int32_t word(double value);

    /// The sweep, in full-scale units at kToneDbfs, kSweepFrames long.
    static const std::vector<double>& sweep();
};

/// Follows one input channel of a tone loop.
class ToneVerifier
{
public:
    static constexpr std::int64_t kFirstWindow = 12000;   ///< input frame: the tone is steady by then
    static constexpr std::int64_t kWindowFrames = 48000;  ///< one second
    static constexpr std::size_t kMaxGlitchTimes = 8;
    static constexpr double kSlipFrames = 0.25;          ///< moves under this, with no glitch, are drift
    static constexpr double kFineSlipFrames = 1.0 / 40.0;   ///< ... and after a glitch
    static constexpr double kWanderFactor = 8.0;         ///< ... and never under this many median wanders
    static constexpr std::size_t kWanderSeconds = 15;    ///< the median of the last ones

    /// `channel`: 0-based, of `channels` tones in play (2 for the analog
    /// loop, L and R; 8 for AES); it expects that channel's tone at
    /// `toneDbfs`, and tells "crossed" by its pair's (L and R, AES 1 and 2,
    /// 3 and 4, ...).
    explicit ToneVerifier(unsigned channel = 0, unsigned channels = ToneSignal::kChannels,
                          double toneDbfs = ToneSignal::kToneDbfs);

    /// One input sample, with its input frame number since start. Cheap and
    /// allocation-free: it runs on the engine's thread.
    void feed(std::int64_t inputFrame, std::int32_t sample);

    /// After the run: the round trip from the sweep.
    void finish();

    unsigned channel() const { return channel_; }
    double toneDbfs() const { return toneDbfs_; }

    // What came back
    bool checked = false;            ///< a first whole second was checked
    bool heard = false;              ///< its tone, above -60 dBFS
    bool crossed = false;            ///< not its tone but its pair's
    double levelDbfs = -200.0;       ///< the tone in the first second
    double gainDb = 0.0;             ///< levelDbfs less the tone's level as played
    double noiseDbfs = -200.0;       ///< noise and distortion, AES17 dBFS: the median second's (after finish)
    double firstNoiseDbfs = -200.0;  ///< ... in the first second
    double worstNoiseDbfs = -200.0;  ///< ... in the worst second
    std::uint64_t seconds = 0;       ///< whole seconds checked
    /// How much later (+) the tone came back in the last still second than
    /// in the first, in microseconds, less the slips: the delay's drift.
    double driftUs = 0.0;
    /// What is left of each second's move once the drift is taken out, RMS
    /// in microseconds: how unsteady the delay is from second to second.
    double wanderUs = 0.0;

    // What went wrong
    std::uint64_t glitches = 0;      ///< discontinuities, 10 ms apart or more
    std::vector<std::int64_t> glitchFrames;   ///< input frames of the first ones
    std::uint64_t slips = 0;         ///< times the tone's phase moved
    std::int64_t worstSlipFrames = 0;     ///< the largest: + frames lost, - frames late (repeated, delayed)
    std::uint64_t slipsOfUnknownSize = 0; ///< no clean second before it, or the delay wandered too much
    std::uint64_t levelChanges = 0;  ///< seconds more than 0.5 dB off the first one
    double worstLevelChangeDb = 0.0;

    // Round trip, from the sweep
    bool roundTripFound = false;
    double roundTrip = 0.0;          ///< frames, from an output frame to the input frame it comes back as
    double sweepMatch = 0.0;         ///< normalised correlation at the peak, 0-1
    bool inverted = false;

    bool clean() const
    {
        return heard && !crossed && roundTripFound && seconds >= 2 && glitches == 0 && slips == 0 &&
               levelChanges == 0;
    }

private:
    void closeWindow();
    void spike(std::int64_t inputFrame);
    void followPhase(double phase);
    void noteSlip(double phaseAfter, std::uint64_t secondAfter);
    void addNoise(double noise);
    double medianWander() const;     ///< of the last seconds, in radians per root second
    double tolerance(double base, double gap) const;

    double toneDbfs_;

    unsigned channel_;
    unsigned ownHz_;
    unsigned otherHz_;
    std::int64_t ownHzInverse_;      ///< ownHz_ x this = 1 modulo 48000
    double c_;                       ///< 2 cos(w) of its own tone
    double slipPhase_;               ///< a quarter frame of its tone, in radians
    double fineSlipPhase_;           ///< a fortieth of a frame
    double driftPhase_ = 0.0;        ///< the drift, added up
    static constexpr int kNoiseBins = 2000;     ///< per-second noise, 0.1 dB bins from -200 to 0 dBFS
    std::vector<std::uint32_t> noiseHistogram_;

    std::vector<double> start_;      ///< the input before the first window, for the sweep
    double x1_ = 0.0, x2_ = 0.0;     ///< the two previous samples
    int run_ = 0;                    ///< consecutive frames up to the last one, at most 2
    std::int64_t lastFrame_ = -1;

    // The window being added up
    std::int64_t windowStart_ = kFirstWindow;
    double sumSin_ = 0.0, sumCos_ = 0.0, otherSin_ = 0.0, otherCos_ = 0.0, sum_ = 0.0, sumSquares_ = 0.0;
    std::int64_t count_ = 0;

    // The first window's reference, and the last still second's. Seconds
    // are counted from the first window, 0.
    bool haveReference_ = false;
    double referenceLevel_ = 0.0;
    std::uint64_t second_ = 0;       ///< the second just closed
    double stillPhase_ = 0.0;        ///< the last still second's phase
    std::uint64_t stillSecond_ = 0;
    bool stillValid_ = false;        ///< ... and it had no glitch in it
    double step_ = 0.0;              ///< the phase's move per second, from the last still seconds
    bool haveStep_ = false;
    double wanderSquares_ = 0.0;     ///< what is left of each still second's move, squared
    std::uint64_t wanderCount_ = 0;
    double recentWander_[kWanderSeconds] = {};   ///< ... the last ones, unsquared, the first move too
    std::size_t recentCount_ = 0;
    std::size_t recentNext_ = 0;
    bool glitchSinceStill_ = false;  ///< a glitch in a second after the last still one
    bool windowSpiked_ = false;      ///< a glitch in the second being added up
    bool inSlip_ = false;            ///< the phase left the still one and has not settled
    double phaseBeforeMove_ = 0.0;   ///< the last still second's, while it moves
    std::uint64_t secondBeforeMove_ = 0;
    bool moveMeasurable_ = false;    ///< there was a still second without a glitch before the move
    double movedPhase_ = 0.0;        ///< the last second measured while it moves
    std::uint64_t movedSecond_ = 0;

    // Per-sample detector: calibrated on the first window
    static constexpr int kBins = 240;          ///< |e| from 1e-12 to 1, 20 per decade
    std::vector<std::uint32_t> histogram_;
    struct Peak
    {
        double value;
        std::int64_t frame;
    };
    static constexpr std::size_t kPeaks = 16;
    Peak peaks_[kPeaks] = {};
    std::size_t peakCount_ = 0;
    double threshold_ = -1.0;        ///< not calibrated yet
    std::int64_t lastSpike_ = -1000000;
};

} // namespace ajaduplex::probe
