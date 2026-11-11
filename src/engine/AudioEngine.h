// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AudioEngine.h
 * @brief   Moves audio between the card's rings and the host, block by block.
 *
 * The cycle is the design's, driven by the card's heads and not by video:
 *
 *   start:  configure the audio system, stop both sides (heads to 0), zero
 *           the playback ring, erase mode on, start output, then input.
 *   block:  wait until the write head is N frames past the read position R;
 *           timestamp the block; DMA [R, R+N) out of the capture ring (two
 *           pieces across the wrap); de-interleave into the host's input
 *           buffers of half idx; the host processes and fills its output
 *           buffers; interleave and DMA them to [O, O+N) of the playback
 *           ring; O and R advance by N; idx flips.
 *   output: O is placed S frames ahead of the playback read head as it
 *           stood when the first block became complete (S = N + G, or
 *           N/2 + G in aggressive mode): neither the host's first callback
 *           nor a late wake-up adds to the latency, so O - R is always
 *           S + N plus the card's own offset between its heads, and the
 *           output latency is S, as reported.
 *
 * The ring's end. A block that runs across it goes in two DMAs: the first
 * from its position on, running on past the end into the room the card
 * never plays or fills, as far as the block or the room goes; the second
 * with what wraps, at the ring's start. No DMA of a piece stops at the end
 * (on the KONA X, a piece that did failed once a lap, 2026-11-10).
 *
 * A failed DMA is tried again at once, from the same buffer (a passing
 * fault goes through then) and, if that fails too, from a spare buffer (a
 * bad buffer's DMA does): either way nothing is lost, and a spare that
 * saves a DMA takes the failed buffer's place for good. At stop the DMA is
 * tried once more from each buffer so retired, with the card stopped.
 * Before that, at start, every buffer and spare takes a small DMA each way
 * (the KONA X once failed every small DMA through one buffer, and through
 * that one only: cost, 2026-11-10); one that fails is swapped for a spare
 * that passes, and kept aside, not freed, so no allocation hands it back.
 *
 * Recovery. Output late (the card passed O): what it passed is dropped, and
 * G frames after it (closer to the head a write may land behind it, and
 * would come out a lap later), and the rest of the block goes out on time;
 * O stays on its timeline, so the latency stays what the host was told
 * (decision D1, 2026-11-08; the design moved O S ahead again instead, which
 * made the real latency longer than the reported one). Input more than half a ring behind -> jump to the
 * newest data, O as far, count, resync. Write head still for 50 ms -> silent
 * blocks at the PC's pace until it moves again; both sides placed anew.
 *
 * Head readings are taken modulo the ring. After a long gap between two of
 * them (a host that hung) time says how many times a head went round, and
 * each output reading is checked against where the input's timing says the
 * playback head must be: if they disagree by far more than a head step, the
 * card's output is out of step with its input (restarted by someone else,
 * say) and O is placed anew, which changes the latency.
 *
 * Waiting. Between blocks the engine sleeps until shortly before the next
 * one is due, then polls the write head. When that is, the head just read
 * says, at the nominal rate; the clock model (ClockModel.h), fitted to
 * earlier readings, says it better and is used as long as the two agree.
 * A model that disagrees with the reading is stale and is dropped: it only
 * gets readings while the engine polls, which a PC whose timer wakes late
 * hardly lets it do, and believed regardless it held every block back.
 *
 * The engine knows nothing about ASIO and nothing about threads: runOnce()
 * does one block on the caller's thread, so a console tool, a test with a
 * virtual clock, or the driver's real-time thread can all drive it. Nothing
 * in runOnce() allocates or takes a lock.
 */

#pragma once

#include "common/AlignedBuffer.h"
#include "common/Clock.h"
#include "device/CardAudio.h"
#include "engine/ClockModel.h"
#include "engine/RingMath.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ajaduplex::engine
{

struct EngineConfig
{
    unsigned blockFrames = 128;           ///< N
    unsigned guardFrames = 32;            ///< G
    bool aggressive = false;              ///< S = N/2 + G instead of N + G
    device::AudioConfig audio;            ///< channels, input source, ...
    HeadMode headMode = HeadMode::Conservative;
    std::int64_t earlyWakeNs = 300000;    ///< sleep until this long before a block is due
    std::int64_t pollNs = 50000;          ///< then read the head this often
    std::int64_t stallTimeoutNs = 50000000;

    /// S: how far ahead of the card's playback read head each block is written.
    unsigned outputLeadFrames() const
    {
        return aggressive ? blockFrames / 2 + guardFrames : blockFrames + guardFrames;
    }
};

struct BlockInfo
{
    std::uint64_t index = 0;            ///< blocks since start
    std::int64_t samplePosition = 0;    ///< first frame of the block, in frames since start
    std::int64_t hostTimeNs = 0;        ///< when the block was found complete (Clock time)
    unsigned frames = 0;                ///< N
    bool silent = false;                ///< made up by the engine while the input is stalled
};

/// The host's buffers for one half of the double buffer: one pointer per
/// channel, N samples each. A null input is not filled; a null output is
/// sent as silence.
struct BlockBuffers
{
    std::int32_t* const* inputs = nullptr;
    const std::int32_t* const* outputs = nullptr;
};

enum class EngineEvent
{
    LateOutput,     ///< the card had passed the write position: what it passed was dropped, the latency kept
    InputOverrun,   ///< more than half a ring behind: jumped to the newest data, the output as far
    InputStalled,   ///< the write head stood still past the stall timeout
    InputResumed,   ///< it moves again; input and output placed anew
    OutputMoved,    ///< the output was out of step with the input and was placed anew: the latency is the reported one again
    DmaError,       ///< a DMA failed, and a second try did not save it: that block's audio was lost
    DmaRecovered,   ///< a DMA failed, and a second try from a spare buffer went through: nothing was lost
};

const char* toString(EngineEvent event);

class EngineClient
{
public:
    virtual ~EngineClient() = default;

    /// Buffers for half 0 or 1.
    virtual BlockBuffers buffers(unsigned half) = 0;

    /// The host's turn: the inputs of `half` are filled, the outputs of
    /// `half` are to be filled. May call AudioEngine::outputReady().
    virtual void process(const BlockInfo& info, unsigned half) = 0;

    /// Something the host may want to know about: samples lost with the
    /// timeline kept (late output), or the timeline itself broken (overrun,
    /// stall, output moved), after which the host should resync. Called on
    /// the engine's thread.
    virtual void onEngineEvent(EngineEvent event, const BlockInfo& info)
    {
        (void)event;
        (void)info;
    }

    /// After every block, once its output went to the card: on the engine's
    /// thread, at the quietest moment of its cycle, for the client's own
    /// short work on the card, which is only ever used from this thread (the
    /// driver's routing watch, once a second). What it takes delays the
    /// wait for the next block, not this block's output.
    virtual void afterBlock(const BlockInfo& info) { (void)info; }
};

struct EngineStats
{
    std::uint64_t blocks = 0;
    std::uint64_t silentBlocks = 0;
    std::uint64_t lateOutputs = 0;
    std::uint64_t droppedOutputFrames = 0;    ///< output frames not sent because the card had passed them
    std::uint64_t outputMoves = 0;
    std::uint64_t inputOverruns = 0;
    std::uint64_t stalls = 0;
    std::uint64_t dmaErrors = 0;              ///< blocks a DMA failed in for good: audio lost
    std::uint64_t dmaRecovered = 0;           ///< blocks whose failed DMAs all went through from a spare buffer
    std::uint64_t registerErrors = 0;
    std::uint64_t polls = 0;
    std::int64_t maxDmaInNs = 0;
    std::int64_t maxDmaOutNs = 0;
    std::int64_t maxClientNs = 0;
    std::int64_t minOutputMarginFrames = 0;   ///< smallest lead of O over the card's read head, at write time
    /// How far the playback head was from where the input's timing put it,
    /// at its least and at its most: a few frames either way (the heads'
    /// steps) while the card's input and output run together.
    std::int64_t minOutputStepFrames = 0;
    std::int64_t maxOutputStepFrames = 0;
    double clockPpm = 0.0;                    ///< card clock against the host clock, fitted; 0 while there is no fit
};

/// Where the time went for the last late output, for the log. A block's
/// output must be on the card S frames after the block is complete; this
/// one was found late, read in, handed to the host and written, in all that
/// much past it.
struct LateOutputDetail
{
    std::int64_t lateFrames = 0;       ///< how far the card had read past where it was written
    std::int64_t leadFrames = 0;       ///< S: what a block has, from complete to written
    std::int64_t foundLateFrames = 0;  ///< how long after it was complete the engine found it
    std::int64_t dmaInNs = 0;
    std::int64_t hostNs = 0;           ///< the host's callback (as far as it had got, if it sent with outputReady)
};

/// How a second try at a failed DMA went.
enum class RetryResult
{
    NotTried,
    WentThrough,
    Failed,
};

/// Which DMA failed, for the log. Every DMA starts at the start of a
/// buffer: a whole block goes straight to or from its buffer, what is left
/// of a late block from a buffer of its own, and the part of a block that
/// wraps at the ring's end from another.
struct DmaErrorDetail
{
    bool output = false;            ///< the output's write, or else the input's read
    unsigned pieces = 1;            ///< 2: across the ring's end
    unsigned failed = 0;            ///< which DMAs failed: bit 0 the first, bit 1 the second
    unsigned lost = 0;              ///< which of those a second try did not save
    bool whole = true;              ///< the whole block, or else what was left of a late one
    bool pastEnd = false;           ///< the first of two ran on past the ring's end (or else stopped at it)
    std::uint32_t ringOffset = 0;   ///< bytes into the ring, of the first DMA that failed
    std::uint32_t bytes = 0;        ///< its size
    std::uint32_t systemError = 0;  ///< CardAudio::lastDmaSystemError for it
    // The first DMA that failed, tried again at once:
    RetryResult sameBuffer = RetryResult::NotTried;    ///< from the same buffer
    std::uint32_t sameBufferError = 0;                 ///< what the system said if it failed
    RetryResult spareBuffer = RetryResult::NotTried;   ///< then from a spare buffer
    std::uint32_t spareBufferError = 0;
    bool noSpare = false;           ///< the same buffer failed again and no spare was left
    bool retriesOff = false;        ///< not tried again: second tries had failed three blocks in a row

    bool recovered() const { return failed != 0 && lost == 0; }
};

/// The failure in words, for the log and the probe's report ("output, the
/// part of the block past the ring's end, at the ring's start: 1024 bytes
/// at ring offset 0; system error 87; again from the same buffer: failed,
/// system error 87; from a spare buffer: failed, system error 87"). It
/// allocates nothing: fit for the engine's thread.
void describeDmaError(const DmaErrorDetail& detail, char* text, std::size_t size);

/// What start's check of the buffers found: each one, the spares too, took
/// a DMA of `bytes` each way at the middle of the rings before the run.
struct BufferCheck
{
    std::uint32_t bytes = 0;        ///< each DMA's size
    unsigned checked = 0;           ///< buffers, the spares among them
    unsigned failed = 0;            ///< failed one way or the other
    unsigned replaced = 0;          ///< block buffers swapped for a spare that passed
    unsigned keptBad = 0;           ///< block buffers that failed, no spare passing: second tries are left
    unsigned sparesLeft = 0;
    bool firstWasOutput = false;    ///< the first failure: a write to the card (true) or a read from it
    std::uint32_t systemError = 0;  ///< ... and what it said
};

/// The check in words ("1 of 10 buffers failed a 512-byte DMA before the
/// run (a write to the card, system error 31): replaced by a spare that
/// passed; 3 spares left"). Empty when none failed.
std::string describeBufferCheck(const BufferCheck& check);

/// A buffer the engine stopped using: a DMA through it failed twice and
/// went through from a spare. At stop that DMA is tried from it once more,
/// with the card stopped.
struct RetiredBuffer
{
    bool output = false;
    std::uint32_t ringOffset = 0;
    std::uint32_t bytes = 0;
    std::uint32_t systemError = 0;      ///< when it failed during the run
    bool checked = false;               ///< tried again at stop
    bool failsAgain = false;
    std::uint32_t againSystemError = 0;
};

class AudioEngine
{
public:
    AudioEngine(device::CardAudio& card, Clock& clock);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Prepares the card and starts both sides. The client must outlive the run.
    bool start(const EngineConfig& config, EngineClient& client, std::string& error);

    /// One block. Returns false once stopped (requestStop) or after too many
    /// consecutive card errors.
    bool runOnce();

    /// From any thread: the next runOnce() returns false.
    void requestStop() { stopRequested_.store(true, std::memory_order_relaxed); }

    /// Stops both sides of the audio system, then tries each retired
    /// buffer's failed DMA again (retiredBuffers). Call on the engine's
    /// thread, or once it has ended. The card's settings are put back by
    /// CardAudio::restore, not here.
    void stop();

    /// From inside EngineClient::process: send this block's output now
    /// instead of when process() returns (ASIOOutputReady).
    void outputReady();

    bool running() const { return running_; }
    const EngineConfig& config() const { return config_; }
    device::RingLayout layout() const { return layout_; }

    /// O and R, bytes into the playback and capture rings: where the next
    /// block goes and comes from. On the engine's thread, or after a run.
    std::uint32_t writePosition() const { return writePosition_; }
    std::uint32_t readPosition() const { return readPosition_; }

    /// From any thread.
    EngineStats stats() const;

    /// The last block's sample position and host time, from any thread
    /// (ASIOGetSamplePosition). False before the first block.
    bool lastBlockPosition(std::int64_t& samplePosition, std::int64_t& hostTimeNs) const;

    /// Why the last late output was late. On the engine's thread only: in
    /// EngineClient::onEngineEvent for EngineEvent::LateOutput.
    const LateOutputDetail& lastLateOutput() const { return lastLate_; }

    /// Which DMA failed last. On the engine's thread only: in
    /// EngineClient::onEngineEvent for EngineEvent::DmaError and
    /// EngineEvent::DmaRecovered.
    const DmaErrorDetail& lastDmaError() const { return lastDmaError_; }

    /// The buffers retired this run, and what trying their DMA again at
    /// stop said. After stop() only.
    std::vector<RetiredBuffer> retiredBuffers() const;

    /// What the check of the buffers at the last start found.
    const BufferCheck& bufferCheck() const { return bufferCheck_; }

    static constexpr unsigned kSpareBuffers = 4;
    static constexpr std::uint32_t kCheckBytes = 512;   ///< start's check: within the sizes that failed

private:
    enum class WaitResult
    {
        Ready,
        Overrun,
        Stalled,
        Stopped,
        Error,
    };

    WaitResult waitForBlock(std::uint64_t& available);
    bool processBlock(bool silent);
    bool runStalled();
    /// S + N: O less the playback head when a block is found complete, plus
    /// that block's backlog, is always this while the card's two heads run
    /// together ((O - R) - (P - W), with O placed by placeOutput).
    std::int64_t anchorFrames() const;
    void placeOutput(std::uint32_t readHead, std::int64_t elapsedFrames);
    void sendOutput();
    /// The DMAs for [offset, offset + bytes) of a ring: one inside it, or
    /// two across its end, the first running on past the end as far as
    /// the bytes and the room go (`pastEnd`), or stopping at it where the
    /// card leaves no room. Returns how many.
    int planDmas(std::uint32_t offset, std::uint32_t bytes, Segment out[2], bool& pastEnd) const;
    /// DMA `index` of a block, from or into the start of `buffer`. A
    /// failure is added to `failure` and tried again from the same buffer,
    /// then from a spare, which, if it goes through, takes the place of
    /// `buffer` (with the same contents) for good. False when the data did
    /// not make it.
    bool dmaPiece(const Segment& piece, unsigned index, std::unique_ptr<AlignedBuffer>& buffer,
                  DmaErrorDetail& failure);
    bool transfer(bool output, const Segment& piece, void* host);
    /// Start's check: every buffer and spare once, the failed ones swapped
    /// out and kept aside.
    void checkBuffers();
    bool bufferPasses(AlignedBuffer& buffer);
    /// A block's failed DMA, once it is known whether a second try saved it.
    void reportDmaFailure(const DmaErrorDetail& failure, const BlockInfo& info);
    void publishPosition(std::int64_t samplePosition, std::int64_t hostTimeNs);
    void notify(EngineEvent event, const BlockInfo& info);
    void recordMax(std::atomic<std::int64_t>& slot, std::int64_t value);
    std::int64_t framesToNs(std::int64_t frames) const;
    std::int64_t nsToFrames(std::int64_t ns) const;

    device::CardAudio& card_;
    Clock& clock_;
    EngineConfig config_;
    EngineClient* client_ = nullptr;
    device::RingLayout layout_;
    RingGeometry ring_;
    ClockModel model_;

    std::unique_ptr<AlignedBuffer> inBuffer_;
    std::unique_ptr<AlignedBuffer> outBuffer_;
    /// One buffer per part of a block that is not moved from its start, so
    /// that every DMA starts at the start of a buffer.
    std::unique_ptr<AlignedBuffer> inPieces_[2];
    std::unique_ptr<AlignedBuffer> outPieces_[2];
    /// For second tries; one that saves a DMA takes the failed buffer's
    /// place, which is kept, unused, until stop.
    std::unique_ptr<AlignedBuffer> spares_[kSpareBuffers];
    unsigned sparesLeft_ = 0;
    std::unique_ptr<AlignedBuffer> retiredData_[kSpareBuffers];
    RetiredBuffer retired_[kSpareBuffers];
    unsigned retiredCount_ = 0;
    unsigned retriesFailedInARow_ = 0;   ///< blocks whose second tries all failed, since one last helped
    bool retryFailedThisBlock_ = false;
    BufferCheck bufferCheck_;
    /// Buffers that failed start's check, for the engine's life: freed, the
    /// next allocation could hand one back.
    std::vector<std::unique_ptr<AlignedBuffer>> rejected_;

    bool running_ = false;
    std::atomic<bool> stopRequested_{false};

    // Engine-thread state
    std::uint32_t readPosition_ = 0;      ///< R, bytes in the capture ring
    std::uint32_t writePosition_ = 0;     ///< O, bytes in the playback ring
    bool outputPlaced_ = false;
    std::uint64_t blockBacklog_ = 0;      ///< frames available when this block was found complete (after a skip)
    std::uint64_t backlog_ = 0;           ///< frames available at the last reading, less what was consumed since
    std::int64_t lastReadNs_ = 0;         ///< time of the last reading of the write head
    std::int64_t blockReadyNs_ = 0;       ///< time of the reading that found this block complete
    std::int64_t quarterRingNs_ = 0;
    std::int64_t inputFrames_ = 0;        ///< frames consumed since start (sample position)
    std::uint64_t blockIndex_ = 0;
    std::uint32_t lastHead_ = 0;
    std::int64_t lastProgressNs_ = 0;
    bool stalled_ = false;
    std::int64_t nextSilentBlockNs_ = 0;
    unsigned consecutiveErrors_ = 0;
    /// Blocks on end that were late with the playback head further along
    /// than the input's timing accounts for, and how many make the output
    /// out of step (a quarter of a second of them).
    std::int64_t lateOutOfStep_ = 0;
    std::int64_t lateOutOfStepLimit_ = 0;

    bool inProcess_ = false;
    bool outputSent_ = false;
    std::int64_t blockDmaInNs_ = 0;       ///< this block's DMA in
    std::int64_t hostStartNs_ = 0;        ///< when this block's callback began
    std::int64_t blockHostNs_ = 0;        ///< what it took, once it returned
    LateOutputDetail lastLate_;
    DmaErrorDetail lastDmaError_;
    BlockBuffers currentBuffers_;
    BlockInfo currentInfo_;

    // Shared with other threads
    std::atomic<std::uint64_t> blocks_{0}, silentBlocks_{0}, lateOutputs_{0}, inputOverruns_{0}, stalls_{0};
    std::atomic<std::uint64_t> droppedOutputFrames_{0}, outputMoves_{0};
    std::atomic<std::uint64_t> dmaErrors_{0}, dmaRecovered_{0}, registerErrors_{0}, polls_{0};
    std::atomic<std::int64_t> maxDmaInNs_{0}, maxDmaOutNs_{0}, maxClientNs_{0};
    std::atomic<std::int64_t> minMarginFrames_{0};
    std::atomic<bool> haveMargin_{false};
    std::atomic<std::int64_t> minStepFrames_{0}, maxStepFrames_{0};
    std::atomic<bool> haveStep_{false};
    std::atomic<double> clockPpm_{0.0};
    std::atomic<std::uint32_t> positionSequence_{0};
    std::atomic<std::int64_t> lastSamplePosition_{0};
    std::atomic<std::int64_t> lastHostTimeNs_{0};
    std::atomic<bool> havePosition_{false};
};

} // namespace ajaduplex::engine
