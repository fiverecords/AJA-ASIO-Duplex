// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "engine/AudioEngine.h"

#include "engine/SampleConvert.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ajaduplex::engine
{

namespace
{

constexpr double kNominalRate = 48000.0;
constexpr unsigned kMinBlockFrames = 16;
constexpr unsigned kMaxBlockFrames = 8192;
constexpr unsigned kMaxConsecutiveErrors = 100;
constexpr std::uint32_t kZeroChunk = 64 * 1024;

/// How far the playback head may be from where the input's timing puts it
/// before the output counts as out of step. Head steps, both sides, and the
/// rounding of times to frames come to far less on any card that streams
/// at all; restarting the output moves the head by much more.
constexpr std::int64_t kOutOfStepFrames = 1024;

/// A block is late beyond its own account when the playback head is this
/// much further along than the input's timing puts it: more than the heads'
/// steps make of it (8 frames each on the KONA X), far less than a guard.
constexpr std::int64_t kStepSlackFrames = 32;

/// Blocks late like that for this long on end are the output out of step,
/// by less than kOutOfStepFrames: it is placed anew. Shorter, it is a
/// reading taken a moment after the other.
constexpr std::int64_t kLateOutOfStepNs = 250000000;

/// How much earlier than the reading just taken says the clock model may
/// put a block before it counts as stale. The reading is short by up to a
/// step of the head (8 frames on the KONA X, 0.17 ms), so a good model is
/// earlier by up to that; 2 ms is far beyond any step.
constexpr std::int64_t kModelEarlyNs = 2000000;

/// value + k * span for the k that brings it nearest to `target`.
std::int64_t nearestCongruent(std::int64_t target, std::int64_t value, std::int64_t span)
{
    const std::int64_t difference = target - value;
    std::int64_t laps = difference / span;
    const std::int64_t rest = difference - laps * span;
    if (rest > span / 2)
        ++laps;
    else if (rest < -span / 2)
        --laps;
    return value + laps * span;
}

std::int64_t magnitude(std::int64_t value)
{
    return value < 0 ? -value : value;
}

} // namespace

const char* toString(EngineEvent event)
{
    switch (event)
    {
        case EngineEvent::LateOutput:   return "late output";
        case EngineEvent::InputOverrun: return "input overrun";
        case EngineEvent::InputStalled: return "input stalled";
        case EngineEvent::InputResumed: return "input resumed";
        case EngineEvent::OutputMoved:  return "output moved";
        case EngineEvent::DmaError:     return "DMA error";
        case EngineEvent::DmaRecovered: return "DMA recovered";
    }
    return "?";
}

AudioEngine::AudioEngine(device::CardAudio& card, Clock& clock) : card_(card), clock_(clock) {}

AudioEngine::~AudioEngine()
{
    stop();
}

std::int64_t AudioEngine::framesToNs(std::int64_t frames) const
{
    return static_cast<std::int64_t>(static_cast<double>(frames) * 1e9 / kNominalRate);
}

std::int64_t AudioEngine::nsToFrames(std::int64_t ns) const
{
    return static_cast<std::int64_t>(static_cast<double>(ns) * kNominalRate / 1e9);
}

void AudioEngine::recordMax(std::atomic<std::int64_t>& slot, std::int64_t value)
{
    if (value > slot.load(std::memory_order_relaxed))
        slot.store(value, std::memory_order_relaxed);
}

void AudioEngine::notify(EngineEvent event, const BlockInfo& info)
{
    if (client_ != nullptr)
        client_->onEngineEvent(event, info);
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool AudioEngine::start(const EngineConfig& config, EngineClient& client, std::string& error)
{
    if (running_)
    {
        error = "the engine is already running";
        return false;
    }
    const unsigned n = config.blockFrames;
    if (n < kMinBlockFrames || n > kMaxBlockFrames)
    {
        error = "block size must be " + std::to_string(kMinBlockFrames) + "-" + std::to_string(kMaxBlockFrames) + " frames";
        return false;
    }
    if (!card_.configure(config.audio, error))
        return false;

    config_ = config;
    client_ = &client;
    layout_ = card_.layout();
    ring_ = RingGeometry(layout_.ringBytes, layout_.frameBytes);
    if (!ring_.valid())
    {
        error = "the card reported an unusable ring layout";
        return false;
    }
    const std::uint32_t blockBytes = n * layout_.frameBytes;
    if (std::uint64_t(n) + config_.outputLeadFrames() > ring_.frames() / 2)
    {
        error = "block and output lead do not fit in half a ring";
        return false;
    }

    // Both sides stopped puts both heads back at 0.
    card_.stopInput();
    card_.stopOutput();

    // The whole playback ring zeroed once; erase mode keeps it that way.
    {
        AlignedBuffer zeros(kZeroChunk);
        for (std::uint32_t offset = 0; offset < layout_.ringBytes; offset += kZeroChunk)
        {
            const std::uint32_t bytes = std::min(kZeroChunk, layout_.ringBytes - offset);
            if (!card_.dmaWrite(device::Ring::Playback, offset, zeros.data(), bytes))
            {
                error = "zeroing the playback ring failed";
                return false;
            }
        }
    }

    // None of them is locked (DMABufferLock): on the KONA X under Windows a
    // DMA of a locked buffer took as long as one of a plain buffer (cost,
    // 2026-11-10: 110 us either way), and DMAs of part of a locked buffer
    // failed (0.1.13).
    inBuffer_ = std::make_unique<AlignedBuffer>(blockBytes);
    outBuffer_ = std::make_unique<AlignedBuffer>(blockBytes);
    for (unsigned i = 0; i < 2; ++i)
    {
        inPieces_[i] = std::make_unique<AlignedBuffer>(blockBytes);
        outPieces_[i] = std::make_unique<AlignedBuffer>(blockBytes);
    }
    for (unsigned i = 0; i < kSpareBuffers; ++i)
    {
        spares_[i] = std::make_unique<AlignedBuffer>(blockBytes);
        retiredData_[i].reset();
        retired_[i] = RetiredBuffer{};
    }
    sparesLeft_ = kSpareBuffers;
    retiredCount_ = 0;
    retriesFailedInARow_ = 0;
    retryFailedThisBlock_ = false;
    checkBuffers();

    readPosition_ = 0;
    writePosition_ = 0;
    outputPlaced_ = false;
    blockBacklog_ = 0;
    backlog_ = 0;
    quarterRingNs_ = framesToNs(ring_.frames() / 4);
    inputFrames_ = 0;
    blockIndex_ = 0;
    lastHead_ = 0;
    stalled_ = false;
    nextSilentBlockNs_ = 0;
    consecutiveErrors_ = 0;
    lateOutOfStep_ = 0;
    lateOutOfStepLimit_ =
        std::max<std::int64_t>(4, (nsToFrames(kLateOutOfStepNs) + config_.blockFrames - 1) / config_.blockFrames);
    haveStep_ = false;
    minStepFrames_ = 0;
    maxStepFrames_ = 0;
    inProcess_ = false;
    model_.reset();
    blocks_ = 0;
    silentBlocks_ = 0;
    lateOutputs_ = 0;
    droppedOutputFrames_ = 0;
    outputMoves_ = 0;
    inputOverruns_ = 0;
    stalls_ = 0;
    dmaErrors_ = 0;
    dmaRecovered_ = 0;
    registerErrors_ = 0;
    polls_ = 0;
    maxDmaInNs_ = 0;
    maxDmaOutNs_ = 0;
    maxClientNs_ = 0;
    minMarginFrames_ = 0;
    haveMargin_ = false;
    clockPpm_ = 0.0;
    havePosition_ = false;
    stopRequested_ = false;

    if (!card_.startOutput())
    {
        error = "StartAudioOutput failed";
        return false;
    }
    if (!card_.startInput())
    {
        card_.stopOutput();
        error = "StartAudioInput failed";
        return false;
    }
    lastProgressNs_ = clock_.nowNs();
    lastReadNs_ = lastProgressNs_;
    running_ = true;
    return true;
}

void AudioEngine::stop()
{
    if (!running_)
        return;
    running_ = false;
    card_.stopInput();
    card_.stopOutput();

    // A bad buffer fails again; a passing fault does not.
    for (unsigned i = 0; i < retiredCount_; ++i)
    {
        RetiredBuffer& r = retired_[i];
        if (!retiredData_[i])
            continue;
        r.checked = true;
        r.failsAgain = !transfer(r.output, Segment{r.ringOffset, r.bytes}, retiredData_[i]->data());
        r.againSystemError = r.failsAgain ? card_.lastDmaSystemError() : 0;
    }
}

std::vector<RetiredBuffer> AudioEngine::retiredBuffers() const
{
    return std::vector<RetiredBuffer>(retired_, retired_ + retiredCount_);
}

// ---------------------------------------------------------------------------
// The cycle
// ---------------------------------------------------------------------------

bool AudioEngine::runOnce()
{
    if (!running_ || stopRequested_.load(std::memory_order_relaxed))
        return false;
    if (stalled_)
        return runStalled();

    std::uint64_t available = 0;
    switch (waitForBlock(available))
    {
        case WaitResult::Stopped:
            return false;

        case WaitResult::Error:
            registerErrors_.fetch_add(1, std::memory_order_relaxed);
            if (++consecutiveErrors_ > kMaxConsecutiveErrors)
            {
                stopRequested_ = true;
                return false;
            }
            clock_.sleepForNs(config_.pollNs);
            return true;

        case WaitResult::Stalled:
        {
            stalled_ = true;
            stalls_.fetch_add(1, std::memory_order_relaxed);
            BlockInfo info;
            info.index = blockIndex_;
            info.samplePosition = inputFrames_;
            info.hostTimeNs = clock_.nowNs();
            info.frames = config_.blockFrames;
            notify(EngineEvent::InputStalled, info);
            nextSilentBlockNs_ = info.hostTimeNs;
            return runStalled();
        }

        case WaitResult::Overrun:
        {
            // Too far behind (the host hung, say): keep only the newest block,
            // and move the output as far, so the latency stays what it was.
            const std::uint64_t skip = available - config_.blockFrames;
            const std::uint64_t skipBytes = skip * layout_.frameBytes;
            readPosition_ = ring_.advance(readPosition_, skipBytes);
            if (outputPlaced_)
                writePosition_ = ring_.advance(writePosition_, skipBytes);
            inputFrames_ += static_cast<std::int64_t>(skip);
            available = config_.blockFrames;
            inputOverruns_.fetch_add(1, std::memory_order_relaxed);
            BlockInfo info;
            info.index = blockIndex_;
            info.samplePosition = inputFrames_;
            info.hostTimeNs = clock_.nowNs();
            info.frames = config_.blockFrames;
            notify(EngineEvent::InputOverrun, info);
            break;
        }

        case WaitResult::Ready:
            break;
    }
    consecutiveErrors_ = 0;
    blockBacklog_ = available;
    return processBlock(false);
}

AudioEngine::WaitResult AudioEngine::waitForBlock(std::uint64_t& available)
{
    const std::uint32_t n = config_.blockFrames;
    const std::int64_t ringFrames = ring_.frames();
    const std::uint64_t halfRing = ring_.frames() / 2;
    for (;;)
    {
        if (stopRequested_.load(std::memory_order_relaxed))
            return WaitResult::Stopped;

        std::uint32_t head = 0;
        if (!card_.readInputHead(head))
            return WaitResult::Error;
        polls_.fetch_add(1, std::memory_order_relaxed);
        const std::int64_t now = clock_.nowNs();
        const std::int64_t sinceLastRead = now - lastReadNs_;
        std::uint64_t avail = ring_.framesAvailable(head, readPosition_, config_.headMode);

        if (sinceLastRead > quarterRingNs_ && head != lastHead_)
        {
            // A long gap since the last reading (a host that hung): the head
            // may have gone round the ring meanwhile. Time says how many
            // times, when it roughly agrees with the reading.
            const std::int64_t expected = static_cast<std::int64_t>(backlog_) + nsToFrames(sinceLastRead);
            const std::int64_t unwrapped = nearestCongruent(expected, static_cast<std::int64_t>(avail), ringFrames);
            if (unwrapped >= 0 && magnitude(unwrapped - expected) < ringFrames / 4)
                avail = static_cast<std::uint64_t>(unwrapped);
        }
        lastReadNs_ = now;
        backlog_ = avail;

        if (head != lastHead_)
        {
            // Only a reading shortly after the previous one dates a change
            // well enough for the model.
            if (sinceLastRead <= 2 * config_.pollNs)
                model_.observe(now, inputFrames_ + static_cast<std::int64_t>(avail));
            lastHead_ = head;
            lastProgressNs_ = now;
        }
        if (avail > halfRing)
        {
            available = avail;
            blockReadyNs_ = now;
            return WaitResult::Overrun;
        }
        if (avail >= n)
        {
            available = avail;
            blockReadyNs_ = now;
            return WaitResult::Ready;
        }
        if (now - lastProgressNs_ > config_.stallTimeoutNs)
            return WaitResult::Stalled;

        // Sleep until shortly before the block is due, then poll. The
        // reading just taken says when: what is still missing, at the
        // nominal rate, which is late by up to a step of the head. The
        // model, fitted to earlier readings, knows where the steps fall and
        // says it better, as long as the two agree: one that puts the block
        // later than the reading by more than two polls, or earlier by more
        // than any step explains, is stale or a poor fit, and is dropped
        // (the next readings start another). On a PC whose timer wakes late
        // the engine seldom polls, so the model gets a reading only now and
        // then; trusted whatever it said, once the card's clock had moved
        // away from its fit it held every block back a little more each
        // second, with no polling left to correct it.
        const std::int64_t byReading = now + framesToNs(static_cast<std::int64_t>(n - avail));
        std::int64_t due = byReading;
        if (model_.ready())
        {
            const std::int64_t byModel = model_.hostTimeOf(inputFrames_ + n);
            if (byModel - byReading > 2 * config_.pollNs || byReading - byModel > kModelEarlyNs)
            {
                model_.reset();
                clockPpm_.store(0.0, std::memory_order_relaxed);   // no fit to report until the next one
            }
            else
                due = byModel;
        }
        const std::int64_t wake = due - config_.earlyWakeNs;
        if (wake - now > config_.pollNs)
            clock_.sleepUntilNs(wake);
        else
            clock_.sleepForNs(config_.pollNs);
    }
}

bool AudioEngine::processBlock(bool silent)
{
    const unsigned n = config_.blockFrames;
    const std::uint32_t blockBytes = n * layout_.frameBytes;
    const unsigned channels = config_.audio.channels;

    BlockInfo info;
    info.index = blockIndex_;
    info.samplePosition = inputFrames_;
    info.hostTimeNs = clock_.nowNs();
    info.frames = n;
    info.silent = silent;
    publishPosition(info.samplePosition, info.hostTimeNs);

    blockDmaInNs_ = 0;
    if (silent)
    {
        std::memset(inBuffer_->data(), 0, blockBytes);
    }
    else
    {
        if (!outputPlaced_)
        {
            // The output is placed by the playback head as it stands now,
            // when the block is found complete, before the host's callback:
            // whatever that takes adds nothing to the latency.
            std::uint32_t readHead = 0;
            if (card_.readOutputHead(readHead))
                placeOutput(readHead, nsToFrames(clock_.nowNs() - blockReadyNs_));
            else
                registerErrors_.fetch_add(1, std::memory_order_relaxed);   // tried again with the next block
        }

        Segment pieces[2];
        DmaErrorDetail failure;
        const int count = planDmas(readPosition_, blockBytes, pieces, failure.pastEnd);
        failure.output = false;
        failure.pieces = unsigned(count);
        bool ok = true;
        const std::int64_t t0 = clock_.nowNs();
        if (count == 1)
            ok = dmaPiece(pieces[0], 0, inBuffer_, failure);
        else if (failure.pastEnd)
        {
            // Across the ring's end: the block in one DMA that runs on past
            // the end (what it brings from there is no audio), then what
            // wrapped, from the ring's start, into place.
            const std::uint32_t before = layout_.ringBytes - readPosition_;
            ok = dmaPiece(pieces[0], 0, inBuffer_, failure);
            if (dmaPiece(pieces[1], 1, inPieces_[1], failure))
                std::memcpy(inBuffer_->as<std::uint8_t>() + before, inPieces_[1]->data(), pieces[1].bytes);
            else
                ok = false;
        }
        else
        {
            // No room past the end: each piece into a buffer of its own,
            // then into place.
            std::uint32_t at = 0;
            for (int i = 0; i < count; ++i)
            {
                if (dmaPiece(pieces[i], unsigned(i), inPieces_[i], failure))
                    std::memcpy(inBuffer_->as<std::uint8_t>() + at, inPieces_[i]->data(), pieces[i].bytes);
                else
                    ok = false;
                at += pieces[i].bytes;
            }
        }
        blockDmaInNs_ = clock_.nowNs() - t0;
        recordMax(maxDmaInNs_, blockDmaInNs_);
        if (!ok)
            std::memset(inBuffer_->data(), 0, blockBytes);
        if (failure.failed != 0)
            reportDmaFailure(failure, info);
        readPosition_ = ring_.advance(readPosition_, blockBytes);
        backlog_ = blockBacklog_ > n ? blockBacklog_ - n : 0;
    }
    inputFrames_ += n;

    const unsigned half = static_cast<unsigned>(blockIndex_ & 1u);
    currentBuffers_ = client_->buffers(half);
    if (currentBuffers_.inputs != nullptr)
        deinterleave(inBuffer_->as<std::int32_t>(), channels, n, currentBuffers_.inputs);

    currentInfo_ = info;
    // While stalled the output side shares the stopped clock: nothing is sent.
    outputSent_ = silent;
    inProcess_ = true;
    const std::int64_t c0 = clock_.nowNs();
    hostStartNs_ = c0;
    blockHostNs_ = 0;
    client_->process(info, half);
    blockHostNs_ = clock_.nowNs() - c0;
    recordMax(maxClientNs_, blockHostNs_);
    inProcess_ = false;
    if (!outputSent_)
        sendOutput();

    ++blockIndex_;
    blocks_.fetch_add(1, std::memory_order_relaxed);
    if (silent)
        silentBlocks_.fetch_add(1, std::memory_order_relaxed);
    if (model_.ready())
        clockPpm_.store(model_.ppm(), std::memory_order_relaxed);
    client_->afterBlock(info);
    return true;
}

void AudioEngine::outputReady()
{
    if (inProcess_ && !outputSent_)
        sendOutput();
}

std::int64_t AudioEngine::anchorFrames() const
{
    return static_cast<std::int64_t>(config_.outputLeadFrames()) + config_.blockFrames;
}

void AudioEngine::placeOutput(std::uint32_t readHead, std::int64_t elapsedFrames)
{
    // O goes where it would be had the block been found the moment it was
    // complete: S ahead of the playback head then, which is the head now
    // less what the card played since, and less the frames past N the
    // block was found late by. So O - R is S + N + (P - W), the same on
    // every start however late the first block was found (on a PC whose
    // timer wakes up late, by as much as a millisecond), and the latency
    // is the one reported.
    const std::uint32_t fetched = ring_.frameFloor(readHead % layout_.ringBytes);
    const std::int64_t ahead = anchorFrames() - static_cast<std::int64_t>(blockBacklog_) - elapsedFrames;
    const std::uint64_t bytes = static_cast<std::uint64_t>(ahead < 0 ? -ahead : ahead) * layout_.frameBytes;
    writePosition_ = ahead >= 0 ? ring_.advance(fetched, bytes) : ring_.retreat(fetched, bytes);
    outputPlaced_ = true;
}

void AudioEngine::sendOutput()
{
    outputSent_ = true;
    if (!outputPlaced_)
        return;   // the playback head could not be read when the block came in

    const unsigned n = config_.blockFrames;
    const std::uint32_t frameBytes = layout_.frameBytes;
    const std::uint32_t blockBytes = n * frameBytes;
    const unsigned channels = config_.audio.channels;

    if (currentBuffers_.outputs != nullptr)
        interleave(currentBuffers_.outputs, channels, n, outBuffer_->as<std::int32_t>());
    else
        std::memset(outBuffer_->data(), 0, blockBytes);

    std::uint32_t readHead = 0;
    if (!card_.readOutputHead(readHead))
    {
        // Where the card is, is not known: the block is dropped rather than
        // written where it may have passed already. O moves on.
        registerErrors_.fetch_add(1, std::memory_order_relaxed);
        droppedOutputFrames_.fetch_add(n, std::memory_order_relaxed);
        writePosition_ = ring_.advance(writePosition_, blockBytes);
        return;
    }
    const std::uint32_t fetched = ring_.frameFloor(readHead % layout_.ringBytes);
    const std::int64_t ringFrames = ring_.frames();
    const std::int64_t elapsed = nsToFrames(clock_.nowNs() - blockReadyNs_);

    // The input's timing says where the playback head is against O: with
    // the two heads running together O - P is S + N less the block's
    // backlog and less what the card played since it was found. The
    // reading, which is modulo the ring, is taken nearest to that, so a
    // block held up for more than half a ring still counts as late.
    const std::int64_t expected = anchorFrames() - static_cast<std::int64_t>(blockBacklog_) - elapsed;
    const std::int64_t reading = ring_.distance(fetched, writePosition_) / frameBytes;
    std::int64_t margin = nearestCongruent(expected, reading, ringFrames);
    std::uint32_t skip = 0;
    const std::int64_t step = margin - expected;   // 0 while the two heads run together
    bool outOfStep = magnitude(step) > kOutOfStepFrames;

    // Late, and by more than this block's own time accounts for: the
    // playback head is further along than the input says. Once, that is a
    // reading taken a moment after the other. Block after block, it is the
    // two heads out of step by less than kOutOfStepFrames: the input's
    // clock ran slow for a while (an embedded source without a signal, or
    // another program changed the input's source and back), and left alone
    // every block from here on would be late and dropped. A host that is
    // late by its own time is not this: its blocks are late by what the
    // input's timing says.
    if (!outOfStep && margin < 0 && step < -kStepSlackFrames)
        outOfStep = ++lateOutOfStep_ >= lateOutOfStepLimit_;
    else
        lateOutOfStep_ = 0;

    if (outOfStep)
    {
        // The playback head is not where it must be if the card's output
        // and input ran together, which they do on one audio system: its
        // output was stopped or restarted behind our back, say, or its
        // input held back. Without placing it anew every block would be
        // late, or far early. Placed as on a start, O is where the input
        // says, and the block is as late as that makes it.
        lateOutOfStep_ = 0;
        placeOutput(readHead, elapsed);
        margin = expected;
        outputMoves_.fetch_add(1, std::memory_order_relaxed);
        notify(EngineEvent::OutputMoved, currentInfo_);
    }
    {
        if (margin < 0)
        {
            // The card has read past O: what it passed is dropped (it plays
            // the zeros erase mode left there), and G frames after it, since
            // what is written closer to the head may land behind it and come
            // out a lap later; the rest of the block goes out on time. O
            // stays on its timeline, so the latency stays what the host was
            // told (D1).
            skip = static_cast<std::uint32_t>(
                std::min<std::int64_t>(n, -margin + static_cast<std::int64_t>(config_.guardFrames)));
            lateOutputs_.fetch_add(1, std::memory_order_relaxed);
            droppedOutputFrames_.fetch_add(skip, std::memory_order_relaxed);
            lastLate_.lateFrames = -margin;
            lastLate_.leadFrames = static_cast<std::int64_t>(config_.outputLeadFrames());
            lastLate_.foundLateFrames = blockBacklog_ > n ? static_cast<std::int64_t>(blockBacklog_ - n) : 0;
            lastLate_.dmaInNs = blockDmaInNs_;
            lastLate_.hostNs = inProcess_ ? clock_.nowNs() - hostStartNs_ : blockHostNs_;
            notify(EngineEvent::LateOutput, currentInfo_);
        }
        if (!outOfStep && (!haveMargin_.load(std::memory_order_relaxed) ||
                           margin < minMarginFrames_.load(std::memory_order_relaxed)))
        {
            minMarginFrames_.store(margin, std::memory_order_relaxed);
            haveMargin_.store(true, std::memory_order_relaxed);
        }
        if (!outOfStep)
        {
            const bool first = !haveStep_.load(std::memory_order_relaxed);
            if (first || step < minStepFrames_.load(std::memory_order_relaxed))
                minStepFrames_.store(step, std::memory_order_relaxed);
            if (first || step > maxStepFrames_.load(std::memory_order_relaxed))
                maxStepFrames_.store(step, std::memory_order_relaxed);
            haveStep_.store(true, std::memory_order_relaxed);
        }
    }

    if (skip < n)
    {
        const std::uint32_t offset = skip * frameBytes;
        const std::uint32_t bytes = blockBytes - offset;
        Segment pieces[2];
        DmaErrorDetail failure;
        const int count = planDmas(ring_.advance(writePosition_, offset), bytes, pieces, failure.pastEnd);
        failure.output = true;
        failure.pieces = unsigned(count);
        failure.whole = offset == 0;
        const std::int64_t t0 = clock_.nowNs();
        if (count == 1 || failure.pastEnd)
        {
            // From the block's buffer, or what is left of a late block from
            // a buffer of its own, at its start; across the ring's end in
            // one DMA that runs on past it (where nothing is played), and
            // what wraps again at the ring's start, from another buffer.
            std::unique_ptr<AlignedBuffer>& first = failure.whole ? outBuffer_ : outPieces_[0];
            if (!failure.whole)
                std::memcpy(first->data(), outBuffer_->as<std::uint8_t>() + offset, bytes);
            dmaPiece(pieces[0], 0, first, failure);
            if (count == 2)
            {
                const std::uint32_t before = layout_.ringBytes - pieces[0].offset;
                std::memcpy(outPieces_[1]->data(), first->as<std::uint8_t>() + before, pieces[1].bytes);
                dmaPiece(pieces[1], 1, outPieces_[1], failure);
            }
        }
        else
        {
            // No room past the end: each piece from a buffer of its own.
            const std::uint8_t* source = outBuffer_->as<std::uint8_t>() + offset;
            for (int i = 0; i < count; ++i)
            {
                std::memcpy(outPieces_[i]->data(), source, pieces[i].bytes);
                dmaPiece(pieces[i], unsigned(i), outPieces_[i], failure);
                source += pieces[i].bytes;
            }
        }
        recordMax(maxDmaOutNs_, clock_.nowNs() - t0);
        if (failure.failed != 0)
            reportDmaFailure(failure, currentInfo_);
    }
    writePosition_ = ring_.advance(writePosition_, blockBytes);
}

void describeDmaError(const DmaErrorDetail& d, char* text, std::size_t size)
{
    if (text == nullptr || size == 0)
        return;
    // The first DMA that failed, then whether the block's other one did too.
    const unsigned firstBit = d.failed & (0u - d.failed);
    const unsigned otherBits = d.failed & ~firstBit;
    const char* block = d.whole ? "the block" : "what was left of a late block";
    char what[96];
    if (d.pieces == 1)
        std::snprintf(what, sizeof(what), "%s", d.whole ? "the whole block" : block);
    else if (d.pastEnd && firstBit == 1u)
        std::snprintf(what, sizeof(what), "%s at the ring's end, running on past it", block);
    else if (d.pastEnd)
        std::snprintf(what, sizeof(what), "the part of %s past the ring's end, at the ring's start", block);
    else
        std::snprintf(what, sizeof(what), "piece %u of 2 at the ring's end", firstBit == 1u ? 1u : 2u);

    char retry[160];
    auto result = [](RetryResult r, std::uint32_t error, char* out, std::size_t room) {
        if (r == RetryResult::WentThrough)
            std::snprintf(out, room, "went through");
        else
            std::snprintf(out, room, "failed, system error %u", static_cast<unsigned>(error));
    };
    char same[48], spare[48];
    result(d.sameBuffer, d.sameBufferError, same, sizeof(same));
    result(d.spareBuffer, d.spareBufferError, spare, sizeof(spare));
    if (d.retriesOff)
        std::snprintf(retry, sizeof(retry), "; not tried again (second tries had failed three blocks in a row)");
    else if (d.sameBuffer == RetryResult::NotTried)
        retry[0] = '\0';
    else if (d.spareBuffer != RetryResult::NotTried)
        std::snprintf(retry, sizeof(retry), "; again from the same buffer: %s; from a spare buffer: %s", same, spare);
    else
        std::snprintf(retry, sizeof(retry), "; again from the same buffer: %s%s", same,
                      d.noSpare ? "; no spare buffer left" : "");

    const char* other = otherBits == 0             ? ""
                        : (d.lost & otherBits) != 0 ? "; the block's other DMA failed too, for good"
                                                    : "; the block's other DMA failed too, and a second try saved it";
    std::snprintf(text, size, "%s, %s: %u bytes at ring offset %u; system error %u%s%s%s%s",
                  d.output ? "output" : "input", what, static_cast<unsigned>(d.bytes),
                  static_cast<unsigned>(d.ringOffset), static_cast<unsigned>(d.systemError),
                  d.systemError == 0 ? " (none: refused before the card's driver)" : "", retry, other,
                  d.recovered() ? "; nothing lost" : "");
}

int AudioEngine::planDmas(std::uint32_t offset, std::uint32_t bytes, Segment out[2], bool& pastEnd) const
{
    const int count = ring_.split(offset, bytes, out);
    pastEnd = count == 2 && layout_.roomBytes > layout_.ringBytes;
    if (pastEnd)
        out[0].bytes = std::min(bytes, layout_.roomBytes - offset);
    return count;
}

bool AudioEngine::bufferPasses(AlignedBuffer& buffer)
{
    // At the middle of the rings, which start just zeroed: zeros written
    // there change nothing, and the read only fills the buffer.
    const std::uint32_t offset = ring_.frameFloor(layout_.ringBytes / 2);
    std::memset(buffer.data(), 0, bufferCheck_.bytes);
    bool output = true;
    bool through = card_.dmaWrite(device::Ring::Playback, offset, buffer.data(), bufferCheck_.bytes);
    if (through)
    {
        output = false;
        through = card_.dmaRead(device::Ring::Capture, offset, buffer.data(), bufferCheck_.bytes);
    }
    ++bufferCheck_.checked;
    if (!through)
    {
        if (bufferCheck_.failed == 0)
        {
            bufferCheck_.firstWasOutput = output;
            bufferCheck_.systemError = card_.lastDmaSystemError();
        }
        ++bufferCheck_.failed;
    }
    return through;
}

void AudioEngine::checkBuffers()
{
    bufferCheck_ = BufferCheck{};
    bufferCheck_.bytes = std::min<std::uint32_t>(kCheckBytes, static_cast<std::uint32_t>(outBuffer_->size()));

    // A spare that passes, from the end of the pool, as second tries take
    // them; one that fails is kept aside.
    auto passingSpare = [this]() -> std::unique_ptr<AlignedBuffer> {
        while (sparesLeft_ > 0)
        {
            std::unique_ptr<AlignedBuffer> spare = std::move(spares_[--sparesLeft_]);
            if (bufferPasses(*spare))
                return spare;
            rejected_.push_back(std::move(spare));
        }
        return nullptr;
    };
    std::unique_ptr<AlignedBuffer>* const own[] = {&inBuffer_,      &outBuffer_,     &inPieces_[0],
                                                   &inPieces_[1],   &outPieces_[0],  &outPieces_[1]};
    for (std::unique_ptr<AlignedBuffer>* slot : own)
    {
        if (bufferPasses(**slot))
            continue;
        std::unique_ptr<AlignedBuffer> spare = passingSpare();
        if (!spare)
        {
            ++bufferCheck_.keptBad;   // a failed DMA through it still gets its second tries
            continue;
        }
        rejected_.push_back(std::move(*slot));
        *slot = std::move(spare);
        ++bufferCheck_.replaced;
    }
    // The spares not reached yet, so that no second try takes a bad one.
    unsigned kept = 0;
    for (unsigned i = 0; i < sparesLeft_; ++i)
    {
        if (!bufferPasses(*spares_[i]))
            rejected_.push_back(std::move(spares_[i]));
        else if (kept++ != i)
            spares_[kept - 1] = std::move(spares_[i]);
    }
    sparesLeft_ = kept;
    bufferCheck_.sparesLeft = sparesLeft_;
}

std::string describeBufferCheck(const BufferCheck& check)
{
    if (check.failed == 0)
        return std::string();
    std::string text = std::to_string(check.failed) + " of " + std::to_string(check.checked) + " buffers failed a " +
                       std::to_string(check.bytes) + "-byte DMA before the run (" +
                       (check.firstWasOutput ? "a write to the card" : "a read from it") + ", system error " +
                       std::to_string(check.systemError) + ")";
    if (check.replaced)
        text += ": " + std::to_string(check.replaced) + " replaced by a spare that passed";
    if (check.keptBad)
        text += std::string(check.replaced ? ", " : ": ") + std::to_string(check.keptBad) +
                " kept, no spare passing (second tries from the same buffer are left)";
    text += "; " + std::to_string(check.sparesLeft) + " spares left";
    return text;
}

bool AudioEngine::transfer(bool output, const Segment& piece, void* host)
{
    return output ? card_.dmaWrite(device::Ring::Playback, piece.offset, host, piece.bytes)
                  : card_.dmaRead(device::Ring::Capture, piece.offset, host, piece.bytes);
}

bool AudioEngine::dmaPiece(const Segment& piece, unsigned index, std::unique_ptr<AlignedBuffer>& buffer,
                           DmaErrorDetail& failure)
{
    if (transfer(failure.output, piece, buffer->data()))
        return true;
    const std::uint32_t said = card_.lastDmaSystemError();
    // The block's first failure says where and how its second try went; a
    // later one only adds its bits.
    const bool first = failure.failed == 0;
    const unsigned bit = 1u << index;
    if (first)
    {
        failure.ringOffset = piece.offset;
        failure.bytes = piece.bytes;
        failure.systemError = said;
    }
    failure.failed |= bit;

    // Second tries at once, unless they have failed block after block (then
    // it is neither a passing fault nor the buffer, and each try costs a
    // DMA's time): from the same buffer, then from a spare.
    constexpr unsigned kRetriesFailedMax = 3;
    if (retriesFailedInARow_ >= kRetriesFailedMax)
    {
        failure.retriesOff = failure.retriesOff || first;
        failure.lost |= bit;
        return false;
    }
    if (transfer(failure.output, piece, buffer->data()))
    {
        if (first)
            failure.sameBuffer = RetryResult::WentThrough;
        return true;
    }
    if (first)
    {
        failure.sameBuffer = RetryResult::Failed;
        failure.sameBufferError = card_.lastDmaSystemError();
    }
    if (sparesLeft_ == 0)
    {
        failure.noSpare = failure.noSpare || first;
        failure.lost |= bit;
        retryFailedThisBlock_ = true;
        return false;
    }
    std::unique_ptr<AlignedBuffer>& spare = spares_[sparesLeft_ - 1];
    if (failure.output)
        std::memcpy(spare->data(), buffer->data(), buffer->size());
    if (!transfer(failure.output, piece, spare->data()))
    {
        if (first)
        {
            failure.spareBuffer = RetryResult::Failed;
            failure.spareBufferError = card_.lastDmaSystemError();
        }
        failure.lost |= bit;
        retryFailedThisBlock_ = true;
        return false;
    }
    if (first)
        failure.spareBuffer = RetryResult::WentThrough;

    // The buffer: the spare, which holds the same now, stays in its place,
    // and the old one is kept for stop().
    RetiredBuffer& r = retired_[retiredCount_];
    r = RetiredBuffer{};
    r.output = failure.output;
    r.ringOffset = piece.offset;
    r.bytes = piece.bytes;
    r.systemError = said;
    retiredData_[retiredCount_] = std::move(buffer);
    ++retiredCount_;
    buffer = std::move(spare);
    --sparesLeft_;
    return true;
}

void AudioEngine::reportDmaFailure(const DmaErrorDetail& failure, const BlockInfo& info)
{
    lastDmaError_ = failure;
    if (retryFailedThisBlock_)
        ++retriesFailedInARow_;
    else if (failure.recovered())
        retriesFailedInARow_ = 0;
    retryFailedThisBlock_ = false;
    if (failure.recovered())
    {
        dmaRecovered_.fetch_add(1, std::memory_order_relaxed);
        notify(EngineEvent::DmaRecovered, info);
    }
    else
    {
        dmaErrors_.fetch_add(1, std::memory_order_relaxed);
        notify(EngineEvent::DmaError, info);
    }
}

bool AudioEngine::runStalled()
{
    // The write head stopped: the host keeps getting blocks, of silence, at
    // the PC's pace, until it moves again.
    if (stopRequested_.load(std::memory_order_relaxed))
        return false;
    clock_.sleepUntilNs(nextSilentBlockNs_);
    nextSilentBlockNs_ += framesToNs(config_.blockFrames);

    std::uint32_t head = 0;
    if (card_.readInputHead(head) && head != lastHead_)
    {
        // Moving again: start from the newest data and place the output anew.
        stalled_ = false;
        lastHead_ = head;
        lastProgressNs_ = clock_.nowNs();
        readPosition_ = ring_.frameFloor(ring_.dataEnd(head, config_.headMode));
        outputPlaced_ = false;
        backlog_ = 0;
        lastReadNs_ = lastProgressNs_;
        model_.reset();
        BlockInfo info;
        info.index = blockIndex_;
        info.samplePosition = inputFrames_;
        info.hostTimeNs = lastProgressNs_;
        info.frames = config_.blockFrames;
        notify(EngineEvent::InputResumed, info);
        return true;
    }
    return processBlock(true);
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

void AudioEngine::publishPosition(std::int64_t samplePosition, std::int64_t hostTimeNs)
{
    // Seqlock: odd while writing.
    const std::uint32_t sequence = positionSequence_.load(std::memory_order_relaxed);
    positionSequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    lastSamplePosition_.store(samplePosition, std::memory_order_relaxed);
    lastHostTimeNs_.store(hostTimeNs, std::memory_order_relaxed);
    positionSequence_.store(sequence + 2, std::memory_order_release);
    havePosition_.store(true, std::memory_order_release);
}

bool AudioEngine::lastBlockPosition(std::int64_t& samplePosition, std::int64_t& hostTimeNs) const
{
    if (!havePosition_.load(std::memory_order_acquire))
        return false;
    for (;;)
    {
        const std::uint32_t before = positionSequence_.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        samplePosition = lastSamplePosition_.load(std::memory_order_relaxed);
        hostTimeNs = lastHostTimeNs_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (positionSequence_.load(std::memory_order_relaxed) == before)
            return true;
    }
}

EngineStats AudioEngine::stats() const
{
    EngineStats s;
    s.blocks = blocks_.load(std::memory_order_relaxed);
    s.silentBlocks = silentBlocks_.load(std::memory_order_relaxed);
    s.lateOutputs = lateOutputs_.load(std::memory_order_relaxed);
    s.droppedOutputFrames = droppedOutputFrames_.load(std::memory_order_relaxed);
    s.outputMoves = outputMoves_.load(std::memory_order_relaxed);
    s.inputOverruns = inputOverruns_.load(std::memory_order_relaxed);
    s.stalls = stalls_.load(std::memory_order_relaxed);
    s.dmaErrors = dmaErrors_.load(std::memory_order_relaxed);
    s.dmaRecovered = dmaRecovered_.load(std::memory_order_relaxed);
    s.registerErrors = registerErrors_.load(std::memory_order_relaxed);
    s.polls = polls_.load(std::memory_order_relaxed);
    s.maxDmaInNs = maxDmaInNs_.load(std::memory_order_relaxed);
    s.maxDmaOutNs = maxDmaOutNs_.load(std::memory_order_relaxed);
    s.maxClientNs = maxClientNs_.load(std::memory_order_relaxed);
    s.minOutputMarginFrames = minMarginFrames_.load(std::memory_order_relaxed);
    s.minOutputStepFrames = minStepFrames_.load(std::memory_order_relaxed);
    s.maxOutputStepFrames = maxStepFrames_.load(std::memory_order_relaxed);
    s.clockPpm = clockPpm_.load(std::memory_order_relaxed);
    return s;
}

} // namespace ajaduplex::engine
