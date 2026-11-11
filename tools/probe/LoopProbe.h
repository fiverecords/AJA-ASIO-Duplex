// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    LoopProbe.h
 * @brief   duplex-probe loop: the engine itself, with AES out looped to AES in
 *          (phase 0, step 3, and its exit test).
 *
 * Each looped output channel plays a tone of its own, and ToneLoop.h checks
 * what each looped input brings back: glitches, slips, level, and the round
 * trip the host sees, from a sweep. Through AES that is the path since
 * 0.1.17: the KONA X's AES inputs resample, so words never come back as
 * they were sent (2026-11-10). Where they do (another card, the simulated
 * one), --path aes-words checks every frame: each looped output carries a
 * frame counter, and each looped input must bring back the counter of the
 * output cabled to it, one frame after the other; any frame lost, repeated,
 * silent or from the wrong channel is counted, and the round trip is exact.
 *
 * A sweep runs N = 256, 128, 64, 32 against G = 64, 32, 16, 8 for a few
 * seconds each and says which combinations come out clean; a single run
 * with --frames 128 --guard G --seconds 3600 is the exit test of phase 0.
 *
 * With --path analog the same engine runs through the Xpand's converters
 * instead, XLR out L/R cabled to XLR in L/R, with the same tone check.
 */

#pragma once

#include "ToneLoop.h"
#include "common/Clock.h"
#include "common/Report.h"
#include "device/CardAudio.h"
#include "engine/AudioEngine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ajaduplex::probe
{

/// The test signal: a 16-bit payload in the top of the 24 bits that AES
/// carries, sign-extended, so its peak is -48 dBFS. Whatever else follows
/// those outputs (the Xpand's analog out may follow AES 1-2) hears a quiet
/// buzz, not full-scale steps.
///
///   payload bit 0       1, so a valid word is never zero (silence)
///   payload bits 1-3    output channel, 0-7 (AES 1-8)
///   payload bits 4-15   frame counter, wraps every 4096 frames (85 ms)
///
/// The low byte of the 32-bit word is ignored: 24-bit AES does not carry it.
struct LoopSignal
{
    static constexpr unsigned kCounterBits = 12;
    static constexpr std::uint32_t kCounterSpan = 1u << kCounterBits;
    static constexpr std::uint32_t kCounterMask = kCounterSpan - 1u;
    static constexpr unsigned kChannels = 8;   ///< AES 1-8
    static constexpr double kPeakDbfs = -48.2;

    static std::int32_t encode(std::int64_t frame, unsigned channel);

    struct Decoded
    {
        bool silent = false;          ///< zero, low byte aside
        bool valid = false;           ///< a signal word
        std::uint32_t counter = 0;
        unsigned channel = 0;
    };
    static Decoded decode(std::int32_t sample);
};

/// Follows one looped input channel.
class ChannelVerifier
{
public:
    /// `outputChannel`: the output channel cabled to this input (0-based).
    explicit ChannelVerifier(unsigned outputChannel = 0) : outputChannel_(outputChannel) {}

    /// One input sample, with its input frame number since start.
    void feed(std::int64_t inputFrame, std::int32_t sample);

    bool locked() const { return locked_; }
    unsigned outputChannel() const { return outputChannel_; }

    std::uint64_t good = 0;
    std::uint64_t lost = 0;          ///< output frames that never came back
    std::uint64_t repeated = 0;
    std::uint64_t silent = 0;        ///< zero words after lock
    std::uint64_t wrongChannel = 0;  ///< signal words of another output channel
    int heardChannel = -1;           ///< that channel (0-based), the last one seen
    std::uint64_t garbage = 0;       ///< non-zero words that are not the signal
    std::uint64_t jumpsBack = 0;     ///< back by more than one frame
    std::uint64_t jumps = 0;         ///< discontinuities of any kind
    std::int64_t roundTrip = -1;     ///< frames, latest (followed across gaps under 2048 frames)
    std::int64_t minRoundTrip = -1;
    std::int64_t maxRoundTrip = -1;
    std::uint64_t roundTripChanges = 0;

    bool clean() const
    {
        return locked_ && lost == 0 && repeated == 0 && jumpsBack == 0 && silent == 0 && wrongChannel == 0 &&
               garbage == 0;
    }

private:
    unsigned outputChannel_;
    bool locked_ = false;
    std::uint32_t expected_ = 0;
};

enum class LoopPath
{
    Aes,       ///< AES out cabled to AES in: a tone on each looped channel, through the inputs' resampler
    AesWords,  ///< the same cables, every frame checked word by word: needs inputs that do not resample
    Analog,    ///< XLR out L/R cabled to XLR in L/R: tones through the converters
};

/// The paths checked with tones (all but AesWords).
inline bool playsTones(LoopPath path)
{
    return path != LoopPath::AesWords;
}

struct LoopSettings
{
    unsigned frames = 128;                   ///< N
    unsigned guard = 32;                     ///< G
    double seconds = 10.0;
    LoopPath path = LoopPath::Aes;
    std::vector<unsigned> channels = {0, 1}; ///< AES paths: looped channels, 0-based: out k cabled to in k
    engine::HeadMode headMode = engine::HeadMode::Conservative;
    /// The card in OEM task mode while the run lasts (put back after), so
    /// that AJA's services leave the routing alone; false: as it is.
    bool oemTasks = true;
};

/// An engine event, for the report: what, and where in the input.
struct LoopEvent
{
    engine::EngineEvent event;
    std::int64_t samplePosition;
    engine::DmaErrorDetail dma{};   ///< for a DMA error, recovered or not: which DMA
};

struct LoopResult
{
    static constexpr std::size_t kMaxEvents = 8;

    LoopSettings settings;
    bool started = false;
    std::string error;               ///< when the engine did not start
    double seconds = 0.0;            ///< measured
    engine::EngineStats engine;
    std::vector<ChannelVerifier> channels;   ///< AES words path
    std::vector<ToneVerifier> tones;       ///< tone paths: one per looped channel (L and R on the analog path)
    bool haveAnalogLevel = false;            ///< tone paths: the XLR outputs play them too (AES 1-2, Q4)
    int analogLevelDbu = 0;                  ///< the Xpand's 0 dBFS, in dBu
    std::vector<LoopEvent> firstEvents;   ///< up to kMaxEvents
    std::uint64_t events = 0;             ///< all of them
    bool interrupted = false;
    /// The card's clock against the PC's over the run, from the blocks that
    /// were found complete soonest near its start and near its end (runs of
    /// 2 s or more). Finer than the engine's model, which fits a short window.
    bool clockMeasured = false;
    double clockPpm = 0.0;
    /// Where the blocks met the ring's end, at the end of the run: frames
    /// of the block there before the end (N: a block ended right at it).
    unsigned outputFramesBeforeEnd = 0;
    unsigned inputFramesBeforeEnd = 0;
    /// Buffers the engine retired after a failed DMA that went through from
    /// a spare, and what trying it again at stop said.
    std::vector<engine::RetiredBuffer> retired;
    /// What start's check of the buffers found (a DMA each way through
    /// each one, the spares too, before the run).
    engine::BufferCheck bufferCheck;

    unsigned leadFrames() const { return settings.frames + settings.guard; }
    bool clean() const;
    std::uint64_t lost() const;
    std::uint64_t repeated() const;
    std::uint64_t silent() const;
    std::uint64_t wrong() const;
    std::uint64_t garbage() const;
};

/// How a run is going, for another thread to read while it goes on (the
/// run's own thread never waits on the console).
struct LoopProgress
{
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<std::uint64_t> problems{0};      ///< engine events plus frames that came back wrong
    std::atomic<bool> allLocked{false};          ///< every looped input has heard its output
};

/// One run of the engine on an open audio system, for settings.seconds of
/// the clock or until `stopRequested`. A run that could not start says why
/// in LoopResult::error. `progress`, if given, is updated after every block.
LoopResult runLoop(device::CardAudio& card, Clock& clock, const LoopSettings& settings,
                   const std::function<bool()>& stopRequested, LoopProgress* progress = nullptr);

/// The (N, G) pairs to run, largest N first: N 256/128/64/32 against
/// G 64/32/16/8, or only `frames` (when not 0) and only `guard` (when not
/// negative).
std::vector<std::pair<unsigned, unsigned>> sweepGrid(unsigned frames, int guard);

/// One line for the console: CLEAN or FAILED, and the round trip.
std::string loopSummaryLine(const LoopResult& result);

struct LoopRun
{
    std::string description;
    std::vector<LoopResult> results;
    bool interrupted = false;
};

void addLoopReport(const LoopRun& run, Report& report);
std::string loopCsv(const LoopRun& run);

} // namespace ajaduplex::probe
