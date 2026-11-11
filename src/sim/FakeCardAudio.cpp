// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "sim/FakeCardAudio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace ajaduplex::sim
{

namespace
{

// The KONA X numbers with 4 MB audio buffers (NTV2_AUDIO_BUFFER_BIG).
constexpr std::uint32_t kRingBytes = 0x000FF000u * 4u;       // NTV2_AUDIO_WRAPADDRESS_BIG
constexpr std::uint32_t kCaptureOffset = 0x00100000u * 4u;   // NTV2_AUDIO_READBUFFEROFFSET_BIG
constexpr unsigned kMaxChannels = 16;
constexpr std::uint32_t kCounterOrigin = 123456789u;         // "ticks since power-up"

constexpr unsigned kAesGroups = 2;                           // AES out 1-4 and 5-8

std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

/// The routing as the model says the card is found.
device::CardRouting initialRouting(const FakeCardModel& m)
{
    const unsigned other = m.audioSystem == 1 ? 2u : 1u;
    device::CardRouting r;
    r.haveSource = true;
    r.source = m.sourceAtStart;
    r.haveXpandSelect = m.hasBreakoutBoard;
    r.xpandSelect = m.hasBreakoutBoard && m.analog.selectedAtStart ? 1u : 0u;
    r.haveAnalogLevel = m.hasBreakoutBoard;
    r.analogLevelDbu = m.hasBreakoutBoard ? m.analog.levelDbu : 0;
    r.aesGroups = kAesGroups;
    for (unsigned g = 0; g < kAesGroups; ++g)
        r.aes[g] = device::OutputSource{m.aesOutputsOnThisSystem ? m.audioSystem : other, 4 * g + 1};
    r.haveMonitor = true;
    r.monitor = device::OutputSource{m.monitorOnThisSystem ? m.audioSystem : other, 1};
    const unsigned outputsFrom = m.videoOutputsOnThisSystem ? m.audioSystem : other;
    for (unsigned n = 0; n < m.sdiOutputs && n < device::CardRouting::kMaxSdiOutputs; ++n)
    {
        r.sdiOutputs |= 1u << n;
        r.sdiOut[n] = device::SdiOutputAudio{outputsFrom, true};
    }
    r.haveHdmiOut = m.hdmiOutputs > 0;
    r.hdmiOut = device::HdmiOutputAudio{outputsFrom, 8, 1};
    r.haveTaskMode = true;
    r.taskMode = m.taskMode;
    return r;
}

/// What a part is called in the restore note.
const char* partName(unsigned part)
{
    switch (part)
    {
        case device::routing::kSource:            return "the input source";
        case device::routing::kXpandSelect:       return "the Xpand's input select";
        case device::routing::kAnalogLevel:       return "the Xpand's analog level";
        case device::routing::kAesGroup << 0:     return "AES out 1-4";
        case device::routing::kAesGroup << 1:     return "AES out 5-8";
        case device::routing::kMonitor:           return "the monitor";
        case device::routing::kSdiOut << 0:       return "SDI out 1";
        case device::routing::kSdiOut << 1:       return "SDI out 2";
        case device::routing::kHdmiOut:           return "the HDMI out";
        case device::routing::kTaskMode:          return "the task mode";
        default:                                  return "a setting";
    }
}

/// Audio systems 1-4, as the card's map can name them.
bool validSource(device::OutputSource source, unsigned step, unsigned lastFirst)
{
    return source.audioSystem >= 1 && source.audioSystem <= 4 && source.firstChannel >= 1 &&
           source.firstChannel <= lastFirst && (source.firstChannel - 1) % step == 0;
}

} // namespace

FakeCardModel FakeCardModel::withAesLoop()
{
    FakeCardModel model;
    for (unsigned ch = 0; ch < 8; ++ch)
        model.cable.push_back(Patch{ch, ch});
    return model;
}

FakeCardModel FakeCardModel::withAnalogLoop()
{
    FakeCardModel model = withAesLoop();
    model.analog.looped = true;
    return model;
}

FakeCardAudio::FakeCardAudio(VirtualClock& clock, FakeCardModel model)
    : clock_(clock), virtualClock_(&clock), model_(std::move(model)), originNs_(clock.nowNs()),
      rng_(model_.seed ? model_.seed : 1u), lockedWholeOnly_(model_.lockedDmasWholeOnly),
      routing_(initialRouting(model_)), saved_(routing_), heldByAnother_(model_.heldAtStart),
      heldAtStart_(model_.heldAtStart), analogRng_(model_.analog.seed ? model_.analog.seed : 3u)
{
}

FakeCardAudio::FakeCardAudio(Clock& clock, FakeCardModel model)
    : clock_(clock), virtualClock_(nullptr), model_(std::move(model)), originNs_(clock.nowNs()),
      rng_(model_.seed ? model_.seed : 1u), lockedWholeOnly_(model_.lockedDmasWholeOnly),
      routing_(initialRouting(model_)), saved_(routing_), heldByAnother_(model_.heldAtStart),
      heldAtStart_(model_.heldAtStart), analogRng_(model_.analog.seed ? model_.analog.seed : 3u)
{
}

bool FakeCardAudio::aesOutputsRouted() const
{
    for (unsigned g = 0; g < routing_.aesGroups; ++g)
    {
        if (routing_.aes[g] != device::OutputSource{model_.audioSystem, 4 * g + 1})
            return false;
    }
    return true;
}

FakeCardAudio::~FakeCardAudio()
{
    restore();
}

std::string FakeCardAudio::description() const
{
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "Simulated card (FakeCardAudio): audio clock %+.1f ppm, input head in %u-frame steps (%s), "
                  "output fetches %u frames ahead in %u-frame chunks (%s), %zu cable patch(es)%s",
                  model_.clockPpm, unsigned(model_.inputChunkFrames),
                  model_.inputHeadAtLastByte ? "last byte" : "next byte",
                  unsigned(model_.outputPrefetchFrames), unsigned(model_.outputChunkFrames),
                  model_.outputHeadAtLastByte ? "last byte" : "next byte", model_.cable.size(),
                  model_.aesOutputsOnThisSystem ? "" : ", AES outputs on another audio system");
    std::string text = buf;
    if (model_.sdiLoop.looped)
    {
        std::snprintf(buf, sizeof(buf), "; SDI loop: SDI out %u to SDI in %u, %u frames", model_.sdiLoop.output + 1,
                      model_.sdiLoop.input + 1, unsigned(model_.sdiLoop.delayFrames));
        text += buf;
    }
    if (model_.aesResampler.on)
    {
        std::snprintf(buf, sizeof(buf), "; AES inputs resample: %.1f frames more, noise %.0f dBFS",
                      model_.aesResampler.delayFrames, model_.aesResampler.noiseDbfs);
        text += buf;
    }
    if (model_.analog.looped)
    {
        std::snprintf(buf, sizeof(buf),
                      "; XLR loop: converters %u frames, gain %+.1f dB, noise %.0f dBFS%s, analog out from %s",
                      unsigned(model_.analog.delayFrames), model_.analog.gainDb, model_.analog.noiseDbfs,
                      model_.analog.inverted ? ", polarity inverted" : "",
                      model_.analog.outFollowsMonitor ? "the monitor" : "AES 1-2");
        text += buf;
    }
    return text;
}

bool FakeCardAudio::configure(const device::AudioConfig& config, std::string& error)
{
    if (outputRunning_ || inputRunning_)
    {
        error = "the audio system is running; stop it before configuring";
        return false;
    }
    if (config.channels == 0 || config.channels > kMaxChannels)
    {
        error = "channels must be 1-16";
        return false;
    }
    const std::uint32_t frameBytes = 4u * config.channels;
    if (kRingBytes % frameBytes != 0)
    {
        error = "the ring does not hold a whole number of " + std::to_string(config.channels) + "-channel frames";
        return false;
    }
    if (config.source == device::InputSource::Other)
    {
        error = "the input source cannot be set to another source";
        return false;
    }
    if (config.routeMonitor && config.monitorPair > 7)
    {
        error = "the monitor plays one of 8 channel pairs";
        return false;
    }
    if (config.taskMode != device::TaskMode::Unknown && !routing_.haveTaskMode)
    {
        error = "the card's task mode could not be read, so it would not be put back";
        return false;
    }
    const unsigned sdiOutputs = std::min(model_.sdiOutputs, device::CardRouting::kMaxSdiOutputs);
    if ((config.sdiOutputs >> sdiOutputs) != 0)
    {
        error = "the card has " + std::to_string(model_.sdiOutputs) + " SDI outputs";
        return false;
    }
    if (config.hdmiOutput && model_.hdmiOutputs == 0)
    {
        error = "the card has no HDMI output";
        return false;
    }
    if (config.analogLevelDbu != 0 && (!device::isAnalogLevel(config.analogLevelDbu) || !routing_.haveAnalogLevel))
    {
        error = routing_.haveAnalogLevel ? "the Xpand's analog level is +24, +18, +15 or +12 dBu"
                                         : "there is no Xpand to set the analog level of";
        return false;
    }

    config_ = config;
    layout_.ringBytes = kRingBytes;
    layout_.captureOffset = kCaptureOffset;
    layout_.frameBytes = frameBytes;
    layout_.roomBytes = model_.roomPastEnd ? kCaptureOffset : kRingBytes;
    playback_.assign(layout_.roomBytes, 0);
    capture_.assign(layout_.roomBytes, 0);
    // Past the end, what the card leaves there: nothing a frame is made of.
    std::fill(capture_.begin() + kRingBytes, capture_.end(), std::uint8_t(0xA5));

    // History of fetched frames: at least one ring and at least 2^17 frames
    // (2.7 s), so a cable read after a pause still finds its frames.
    std::int64_t frames = 1;
    while (frames < std::max<std::int64_t>(layout_.ringFrames(), std::int64_t(1) << 17))
        frames <<= 1;
    historyFrames_ = frames;
    history_.assign(static_cast<std::size_t>(frames) * config.channels, 0);

    captureEnable_ = config.captureEnable;
    if (config.routeAesOutputs)
    {
        for (unsigned g = 0; g < routing_.aesGroups; ++g)
            routing_.aes[g] = device::OutputSource{model_.audioSystem, 4 * g + 1};
        touched_ |= device::routing::kAesGroups;
    }
    if (config.routeMonitor)
    {
        routing_.monitor = device::OutputSource{model_.audioSystem, 2 * config.monitorPair + 1};
        touched_ |= device::routing::kMonitor;
    }
    if (config.analogLevelDbu != 0)
    {
        // In and out alike, the loop's gain stays as it was: only what is
        // plugged into the Xpand would hear the difference.
        routing_.analogLevelDbu = config.analogLevelDbu;
        routing_.analogLevelCode = 0;
        touched_ |= device::routing::kAnalogLevel;
    }
    if (config.taskMode != device::TaskMode::Unknown)
    {
        routing_.taskMode = config.taskMode;
        touched_ |= device::routing::kTaskMode;
    }
    for (unsigned n = 0; n < sdiOutputs; ++n)
    {
        if ((config.sdiOutputs & (1u << n)) == 0)
            continue;
        routing_.sdiOut[n] = device::SdiOutputAudio{model_.audioSystem, true};
        touched_ |= device::routing::kSdiOut << n;
        routedOutputs_ |= device::routing::kSdiOut << n;
    }
    if (config.hdmiOutput)
    {
        routing_.hdmiOut = device::HdmiOutputAudio{model_.audioSystem, 8, 1};
        touched_ |= device::routing::kHdmiOut;
        routedOutputs_ |= device::routing::kHdmiOut;
    }
    if (config.source != device::InputSource::Unchanged)
    {
        routing_.source = config.source;
        routing_.sourceCode = 0;
        routing_.embeddedInput = config.source == device::InputSource::Embedded ? config.embeddedInput : 0;
        touched_ |= device::routing::kSource;
        // Setting the source rewrites the shared select (the SDK writes 0 or
        // 1); keepXpandSelect puts the old one back right after, as
        // Ntv2CardAudio does.
        if (routing_.haveXpandSelect)
        {
            if (!config.keepXpandSelect)
                routing_.xpandSelect = config.source == device::InputSource::Analog ? 1u : 0u;
            touched_ |= device::routing::kXpandSelect;
        }
    }
    ours_ = routing_;
    configured_ = true;
    return true;
}

bool FakeCardAudio::setCaptureEnable(bool enable)
{
    if (!configured_)
        return false;
    update();
    captureEnable_ = enable;
    return true;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

std::int64_t FakeCardAudio::runningNsAt(std::int64_t ns) const
{
    std::int64_t running = ns - originNs_;
    for (const Stall& stall : stalls_)
    {
        if (ns <= stall.fromNs)
            break;
        running -= std::min(ns, stall.fromNs + stall.durationNs) - stall.fromNs;
    }
    return running;
}

std::int64_t FakeCardAudio::audioTicksAt(std::int64_t ns) const
{
    const long double rate = static_cast<long double>(model_.sampleRate) * (1.0L + model_.clockPpm * 1e-6L);
    return static_cast<std::int64_t>(std::floor(static_cast<long double>(runningNsAt(ns)) * rate / 1e9L));
}

std::int64_t FakeCardAudio::nsAtAudioTick(std::int64_t tick) const
{
    const long double rate = static_cast<long double>(model_.sampleRate) * (1.0L + model_.clockPpm * 1e-6L);
    std::int64_t ns = originNs_ + static_cast<std::int64_t>(std::ceil(static_cast<long double>(tick) * 1e9L / rate));
    for (const Stall& stall : stalls_)
    {
        if (stall.fromNs < ns)
            ns += stall.durationNs;
        else
            break;
    }
    return ns;
}

void FakeCardAudio::stallAudioClock(std::int64_t fromNs, std::int64_t durationNs)
{
    if (durationNs <= 0)
        return;
    update();   // settle everything up to now on the old clock
    stalls_.push_back(Stall{fromNs, durationNs});
    std::sort(stalls_.begin(), stalls_.end(), [](const Stall& a, const Stall& b) { return a.fromNs < b.fromNs; });
}

void FakeCardAudio::holdInput(std::int64_t frames)
{
    if (frames <= 0)
        return;
    update();   // settle both sides up to now
    // Input frame k is written at tick k + inputStartTick_: later by
    // `frames` from here on, and nothing is written until the tick catches
    // up with what is in the ring already.
    inputStartTick_ += frames;
}

void FakeCardAudio::delayNextDmas(unsigned count, std::int64_t extraNs)
{
    delayedDmas_ = count;
    dmaDelayNs_ = extraNs;
}

void FakeCardAudio::failNextDmas(unsigned count, std::uint32_t systemError)
{
    failingDmas_ = count;
    failingSystemError_ = systemError;
}

bool FakeCardAudio::dmaGoesThrough(device::Ring ring, bool write, std::uint32_t offset, const void* host,
                                   std::uint32_t bytes)
{
    bool through = true;
    std::uint32_t said = 0;
    if (badBufferArmed_ && ring == badBufferRing_ && offset <= badBufferOffset_ &&
        badBufferOffset_ - offset < bytes)
    {
        if (badBufferSpare_ > 0)
            --badBufferSpare_;
        else
        {
            badBufferArmed_ = false;
            badBuffers_[host] = BadBuffer{badBufferError_, badBufferLargest_};
        }
    }
    const auto bad = badBuffers_.find(host);
    if (failingDmas_ > 0)
    {
        --failingDmas_;
        through = false;
        said = failingSystemError_;
    }
    else if (bad != badBuffers_.end() && bytes <= bad->second.largestFailing)
    {
        through = false;
        said = bad->second.systemError;
    }
    else if (lockedWholeOnly_)
    {
        const auto* start = static_cast<const std::uint8_t*>(host);
        for (const auto& [address, size] : locked_)
        {
            const auto* lockedStart = static_cast<const std::uint8_t*>(address);
            const bool overlaps = start < lockedStart + size && lockedStart < start + bytes;
            if (overlaps && (start != lockedStart || bytes != size))
                through = false;
        }
    }
    if (through && write && failWritesAt_ && ring == device::Ring::Playback && offset == failWritesOffset_)
    {
        if (failWritesSpare_ > 0)
            --failWritesSpare_;
        else
        {
            through = false;
            said = failWritesError_;
        }
    }
    if (!through)
    {
        ++failedDmas_;
        lastDmaSystemError_ = said;
    }
    if (recordDmas_)
        dmaRecords_.push_back(DmaRecord{ring, write, offset, bytes, host, through});
    return through;
}

std::int64_t FakeCardAudio::dmaExtra()
{
    if (delayedDmas_ == 0)
        return 0;
    --delayedDmas_;
    return dmaDelayNs_;
}

std::int64_t FakeCardAudio::audioTicksNow()
{
    return audioTicksAt(clock_.nowNs());
}

std::int64_t FakeCardAudio::jitter()
{
    if (model_.jitterNs <= 0)
        return 0;
    // xorshift32: deterministic for a given seed.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<std::int64_t>(rng_ % static_cast<std::uint32_t>(model_.jitterNs + 1));
}

unsigned FakeCardAudio::analogPairs() const
{
    const unsigned select = routing_.xpandSelect;
    switch (model_.analog.select)
    {
        case FakeCardModel::AnalogPath::Select::Bit0:    return (select & 1u) ? 1u : 0u;
        case FakeCardModel::AnalogPath::Select::NonZero: return select != 0 ? 1u : 0u;
        case FakeCardModel::AnalogPath::Select::PerPair: return select & 0xFu;
    }
    return 0;
}

unsigned FakeCardAudio::silencedPairs() const
{
    // With analog on AES in 1-2 alone, the rest of AES is kept or not (Q5).
    if (model_.analog.select != FakeCardModel::AnalogPath::Select::PerPair && analogPairs() != 0 &&
        model_.analog.replacesAllAes)
        return 0xEu;
    return 0;
}

double FakeCardAudio::analogNoise()
{
    // Box-Muller on its own xorshift32, so the costs' jitter stays the same.
    auto uniform = [this] {
        analogRng_ ^= analogRng_ << 13;
        analogRng_ ^= analogRng_ >> 17;
        analogRng_ ^= analogRng_ << 5;
        return (static_cast<double>(analogRng_) + 1.0) / 4294967297.0;   // (0, 1)
    };
    const double u1 = uniform();
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

void FakeCardAudio::spend(std::int64_t ns)
{
    if (virtualClock_ != nullptr)
        virtualClock_->advance(ns + jitter());
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void FakeCardAudio::update()
{
    if (!configured_)
        return;
    const std::int64_t tick = audioTicksNow();

    // Output first: the input side may need what the output just fetched.
    if (outputRunning_)
    {
        const std::int64_t chunk = std::max<std::int64_t>(1, model_.outputChunkFrames);
        const std::int64_t wanted = tick - outputStartTick_ + model_.outputPrefetchFrames;
        const std::int64_t target = floorDiv(wanted, chunk) * chunk;
        if (target > fetched_)
            fetchOutputTo(target);
    }
    if (inputRunning_)
    {
        const std::int64_t chunk = std::max<std::int64_t>(1, model_.inputChunkFrames);
        const std::int64_t target = floorDiv(tick - inputStartTick_, chunk) * chunk;
        if (target > written_)
            writeInputTo(target);
    }
}

void FakeCardAudio::fetchOutputTo(std::int64_t target)
{
    const std::int64_t ringFrames = layout_.ringFrames();
    const std::size_t frameBytes = layout_.frameBytes;
    const bool erase = config_.eraseMode && model_.eraseSupported;

    // Frames older than the history are lost anyway; the history is at least
    // one ring long, so every ring frame is still visited (and erased).
    const std::int64_t from = std::max(fetched_, target - historyFrames_);
    for (std::int64_t k = from; k < target; ++k)
    {
        std::uint8_t* frame = &playback_[static_cast<std::size_t>(k % ringFrames) * frameBytes];
        std::int32_t* kept = &history_[static_cast<std::size_t>(k % historyFrames_) * config_.channels];
        std::memcpy(kept, frame, frameBytes);
        if (erase)
            std::memset(frame, 0, frameBytes);
    }
    fetched_ = target;
}

void FakeCardAudio::writeInputTo(std::int64_t target)
{
    const std::int64_t ringFrames = layout_.ringFrames();
    const std::size_t frameBytes = layout_.frameBytes;
    const bool writeMemory = captureEnable_ || !model_.captureEnableGatesWrites;

    if (writeMemory)
    {
        // Only the last lap can still be in the ring.
        const std::int64_t from = std::max(written_, target - ringFrames);
        const std::int64_t delay = model_.cableDelayFrames;
        const FakeCardModel::AnalogPath& analog = model_.analog;
        // The Xpand's inputs are only heard with an AES or analog source;
        // with SDI or HDMI, what the video input carries.
        const bool xpandInput =
            routing_.source == device::InputSource::Aes || routing_.source == device::InputSource::Analog;
        // An SDI input has no signal while its connector sends; with the
        // loop cabled, it hears what that SDI output plays, if the output
        // sends and its embedder plays this audio system (silence if not).
        const bool sdiInput = routing_.source == device::InputSource::Embedded;
        auto sends = [this](unsigned connector) {
            return !model_.bidirectionalSdi || ((model_.sdiSending >> connector) & 1u) != 0;
        };
        const bool inputReceives = !sdiInput || !model_.bidirectionalSdi || !sends(routing_.embeddedInput);
        const FakeCardModel::SdiLoop& loop = model_.sdiLoop;
        const bool looped = sdiInput && inputReceives && loop.looped && loop.input == routing_.embeddedInput &&
                            loop.output < model_.sdiOutputs && loop.output < device::CardRouting::kMaxSdiOutputs &&
                            sends(loop.output);
        const bool loopPlaysOurs = looped && routing_.sdiOut[loop.output].enabled &&
                                   routing_.sdiOut[loop.output].audioSystem == model_.audioSystem;
        const unsigned embeddedChannels =
            !model_.videoSignal || !inputReceives || looped ? 0u
            : sdiInput                                      ? 16u
            : routing_.source == device::InputSource::Hdmi  ? 8u
                                                            : 0u;
        const double toneAmplitude = std::pow(10.0, -30.0 / 20.0) * 2147483648.0;
        // The analog output plays AES out 1-2 or the monitor, depending on
        // the card (Q4): whatever channels they are routed from, if ours.
        const device::OutputSource analogFrom = analog.outFollowsMonitor ? routing_.monitor : routing_.aes[0];
        const bool analogOutIsOurs = analogFrom.audioSystem == model_.audioSystem;
        const unsigned analogSource = analogFrom.firstChannel > 0 ? analogFrom.firstChannel - 1 : 0;
        const unsigned analogOn = xpandInput ? analogPairs() : 0u;
        const unsigned silenced = silencedPairs();
        const double gain = (analog.inverted ? -1.0 : 1.0) * std::pow(10.0, analog.gainDb / 20.0);
        const double noise = std::pow(10.0, analog.noiseDbfs / 20.0) / std::sqrt(2.0) * 2147483648.0;
        const double dc = analog.dcOffset * 2147483648.0;
        const bool resample = model_.aesResampler.on;
        const std::int64_t resampledFrames =
            resample ? static_cast<std::int64_t>(std::floor(model_.aesResampler.delayFrames)) : 0;
        const double resampledFraction =
            resample ? model_.aesResampler.delayFrames - static_cast<double>(resampledFrames) : 0.0;
        const double resamplerNoise =
            std::pow(10.0, model_.aesResampler.noiseDbfs / 20.0) / std::sqrt(2.0) * 2147483648.0;
        auto playedFrame = [this](std::int64_t j) -> const std::int32_t* {
            if (!outputRunning_ || j < 0 || j >= fetched_ || j < fetched_ - historyFrames_)
                return nullptr;
            return &history_[static_cast<std::size_t>(j % historyFrames_) * config_.channels];
        };
        std::array<std::int32_t, kMaxChannels> samples{};
        for (std::int64_t k = from; k < target; ++k)
        {
            samples.fill(0);
            for (unsigned c = 0; c < embeddedChannels && c < config_.channels; ++c)
            {
                // 24 bits in the top of the word, as embedded audio comes.
                const double x =
                    toneAmplitude * std::sin(6.283185307179586 * 100.0 * (c + 1) * static_cast<double>(k) / 48000.0);
                samples[c] = static_cast<std::int32_t>(std::lround(x / 256.0)) * 256;
            }
            if (loopPlaysOurs)
            {
                const std::int32_t* sent =
                    playedFrame(k + inputStartTick_ - outputStartTick_ - std::int64_t(loop.delayFrames));
                for (unsigned c = 0; sent != nullptr && c < config_.channels; ++c)
                {
                    // 24 bits, as SDI embeds them.
                    samples[c] = static_cast<std::int32_t>(static_cast<std::uint32_t>(sent[c]) & 0xFFFFFF00u);
                }
            }
            // Input frame k is captured at audio tick inputStart + k; the
            // cable carries what the output played `delay` ticks earlier
            // (and, through the resampler, a fraction of a frame more,
            // between two played frames).
            const std::int64_t j = k + inputStartTick_ - outputStartTick_ - delay;
            const std::int32_t* played = xpandInput ? playedFrame(j - resampledFrames) : nullptr;
            const std::int32_t* before = resample && xpandInput ? playedFrame(j - resampledFrames - 1) : nullptr;
            if (played != nullptr)
            {
                for (const FakeCardModel::Patch& patch : model_.cable)
                {
                    // AES out o plays a channel of its group's source: of
                    // this audio system, or of another one, silent here.
                    const unsigned g = patch.output / 4;
                    if (g >= routing_.aesGroups || routing_.aes[g].audioSystem != model_.audioSystem)
                        continue;
                    const unsigned source = routing_.aes[g].firstChannel - 1 + patch.output % 4;
                    if (source >= config_.channels || patch.input >= config_.channels)
                        continue;
                    if (!resample)
                    {
                        samples[patch.input] = played[source];
                        continue;
                    }
                    const double previous = before != nullptr ? static_cast<double>(before[source]) : 0.0;
                    const double y = (1.0 - resampledFraction) * static_cast<double>(played[source]) +
                                     resampledFraction * previous + resamplerNoise * analogNoise();
                    // 24 bits, as AES carries them.
                    samples[patch.input] =
                        static_cast<std::int32_t>(std::clamp(std::round(y / 256.0), -8388608.0, 8388607.0)) * 256;
                }
            }
            if (analogOn != 0)
            {
                const std::int32_t* analogPlayed =
                    analog.looped && analogOutIsOurs
                        ? playedFrame(k + inputStartTick_ - outputStartTick_ - std::int64_t(analog.delayFrames))
                        : nullptr;
                double converted[2];
                for (unsigned c = 0; c < 2; ++c)
                {
                    const double y = analogPlayed != nullptr && analogSource + c < config_.channels
                                         ? static_cast<double>(analogPlayed[analogSource + c])
                                         : 0.0;
                    converted[c] = gain * y + dc + noise * analogNoise();
                }
                for (unsigned pair = 0; pair < 4; ++pair)
                {
                    for (unsigned c = 0; c < 2 && 2 * pair + c < config_.channels; ++c)
                    {
                        if (silenced & (1u << pair))
                            samples[2 * pair + c] = 0;
                        else if (analogOn & (1u << pair))
                        {
                            // 24 bits from the ADC, in the top of the word.
                            const double words =
                                std::clamp(std::round(converted[c] / 256.0), -8388608.0, 8388607.0);
                            samples[2 * pair + c] = static_cast<std::int32_t>(words) * 256;
                        }
                    }
                }
            }
            std::memcpy(&capture_[static_cast<std::size_t>(k % ringFrames) * frameBytes], samples.data(), frameBytes);
        }
    }
    written_ = target;
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool FakeCardAudio::startOutput()
{
    if (!configured_)
        return false;
    update();
    if (outputRunning_)
        return true;
    outputRunning_ = true;
    outputStartTick_ = audioTicksNow();
    fetched_ = 0;
    update();   // the first fetch happens at once
    return true;
}

bool FakeCardAudio::startInput()
{
    if (!configured_)
        return false;
    update();
    if (inputRunning_)
        return true;
    inputRunning_ = true;
    inputStartTick_ = audioTicksNow();
    written_ = 0;
    return true;
}

bool FakeCardAudio::stopOutput()
{
    if (!configured_)
        return false;
    update();
    outputRunning_ = false;
    fetched_ = 0;
    return true;
}

bool FakeCardAudio::stopInput()
{
    if (!configured_)
        return false;
    update();
    inputRunning_ = false;
    written_ = 0;
    return true;
}

std::int64_t FakeCardAudio::framesFetched()
{
    update();
    return fetched_;
}

std::int64_t FakeCardAudio::framesWritten()
{
    update();
    return written_;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

bool FakeCardAudio::readInputHead(std::uint32_t& byteOffset)
{
    if (!configured_)
        return false;
    update();
    if (!inputRunning_ || written_ == 0)
        byteOffset = 0;
    else
    {
        const std::uint64_t bytes = static_cast<std::uint64_t>(written_) * layout_.frameBytes;
        const std::uint64_t position = model_.inputHeadAtLastByte ? bytes - 1 : bytes;
        byteOffset = static_cast<std::uint32_t>(position % layout_.ringBytes);
    }
    spend(model_.registerReadNs);
    return true;
}

bool FakeCardAudio::readOutputHead(std::uint32_t& byteOffset)
{
    if (!configured_)
        return false;
    update();
    if (!outputRunning_ || fetched_ == 0)
        byteOffset = 0;
    else
    {
        const std::uint64_t bytes = static_cast<std::uint64_t>(fetched_) * layout_.frameBytes;
        const std::uint64_t position = model_.outputHeadAtLastByte ? bytes - 1 : bytes;
        byteOffset = static_cast<std::uint32_t>(position % layout_.ringBytes);
    }
    spend(model_.registerReadNs);
    return true;
}

bool FakeCardAudio::readSampleCounter(std::uint32_t& ticks)
{
    std::int64_t value = 0;
    if (model_.counterOnAudioClock)
        value = audioTicksNow();
    else
    {
        const long double rate = static_cast<long double>(model_.sampleRate) * (1.0L + model_.counterPpm * 1e-6L);
        value = static_cast<std::int64_t>(std::floor(static_cast<long double>(clock_.nowNs() - originNs_) * rate / 1e9L));
    }
    ticks = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) + kCounterOrigin);
    spend(model_.registerReadNs);
    return true;
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

bool FakeCardAudio::dmaRead(device::Ring ring, std::uint32_t offset, void* host, std::uint32_t bytes)
{
    if (!configured_ || host == nullptr || bytes == 0 ||
        static_cast<std::uint64_t>(offset) + bytes > layout_.roomBytes)
        return false;
    if (!dmaGoesThrough(ring, false, offset, host, bytes))
        return false;
    update();
    const std::vector<std::uint8_t>& memory = ring == device::Ring::Playback ? playback_ : capture_;
    std::memcpy(host, memory.data() + offset, bytes);
    const std::int64_t setup = locked_.count(host) ? model_.dmaSetupLockedNs : model_.dmaSetupNs;
    spend(setup + static_cast<std::int64_t>(model_.dmaNsPerByte * bytes) + dmaExtra());
    return true;
}

bool FakeCardAudio::dmaWrite(device::Ring ring, std::uint32_t offset, const void* host, std::uint32_t bytes)
{
    if (!configured_ || host == nullptr || bytes == 0 ||
        static_cast<std::uint64_t>(offset) + bytes > layout_.roomBytes)
        return false;
    if (!dmaGoesThrough(ring, true, offset, host, bytes))
        return false;
    update();
    // The write lands at the end of the transfer: what the card fetches
    // meanwhile still sees the old contents.
    const std::int64_t setup = locked_.count(host) ? model_.dmaSetupLockedNs : model_.dmaSetupNs;
    spend(setup + static_cast<std::int64_t>(model_.dmaNsPerByte * bytes) + dmaExtra());
    update();
    std::vector<std::uint8_t>& memory = ring == device::Ring::Playback ? playback_ : capture_;
    std::memcpy(memory.data() + offset, host, bytes);
    ++dmaWrites_;
    return true;
}

bool FakeCardAudio::lockBuffer(const void* host, std::uint32_t bytes)
{
    if (host == nullptr || bytes == 0)
        return false;
    locked_[host] = bytes;
    return true;
}

bool FakeCardAudio::unlockBuffer(const void* host, std::uint32_t)
{
    return locked_.erase(host) > 0;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

bool FakeCardAudio::eventSupported(device::CardEvent event) const
{
    return model_.wrapEventsSupported && event != device::CardEvent::Audio;
}

bool FakeCardAudio::subscribe(device::CardEvent event)
{
    if (!eventSupported(event))
        return false;
    subscribed_.insert(event);
    return true;
}

bool FakeCardAudio::unsubscribe(device::CardEvent event)
{
    return subscribed_.erase(event) > 0;
}

bool FakeCardAudio::waitForEvent(device::CardEvent event, unsigned timeoutMs)
{
    const std::int64_t deadline = clock_.nowNs() + std::int64_t(timeoutMs) * 1000000;
    if (!configured_ || subscribed_.count(event) == 0)
    {
        clock_.sleepUntilNs(deadline);
        return false;
    }
    update();
    const std::int64_t ringFrames = layout_.ringFrames();
    const bool input = event == device::CardEvent::AudioInWrap;
    const bool running = input ? inputRunning_ : outputRunning_;
    if (!running)
    {
        clock_.sleepUntilNs(deadline);
        return false;
    }

    // The next time the head crosses the end of the ring.
    const std::int64_t done = input ? written_ : fetched_;
    const std::int64_t nextWrap = (done / ringFrames + 1) * ringFrames;
    const std::int64_t tick = input ? inputStartTick_ + nextWrap
                                    : outputStartTick_ + nextWrap - model_.outputPrefetchFrames;
    const std::int64_t when = nsAtAudioTick(tick);
    if (when > deadline)
    {
        clock_.sleepUntilNs(deadline);
        return false;
    }
    clock_.sleepUntilNs(when);
    update();
    return true;
}

bool FakeCardAudio::interruptCount(device::CardEvent event, std::uint32_t& count)
{
    if (!configured_ || !eventSupported(event))
        return false;
    update();
    const std::int64_t ringFrames = layout_.ringFrames();
    const std::int64_t done = event == device::CardEvent::AudioInWrap ? written_ : fetched_;
    count = static_cast<std::uint32_t>(done / ringFrames);
    return true;
}

bool FakeCardAudio::readXpandSelect(unsigned& value)
{
    if (!routing_.haveXpandSelect)
        return false;
    value = routing_.xpandSelect;
    return true;
}

bool FakeCardAudio::writeXpandSelect(unsigned value)
{
    if (!routing_.haveXpandSelect || value > 15)
        return false;
    update();   // what came in so far, under the old select
    routing_.xpandSelect = value;
    touched_ |= device::routing::kXpandSelect;
    device::copyRoutingParts(ours_, routing_, device::routing::kXpandSelect);
    return true;
}

bool FakeCardAudio::readRouting(device::CardRouting& routing)
{
    // The source register, the Xpand's, the output map and the task mode;
    // two more for each SDI output and the HDMI output this program set.
    routing = routing_;
    unsigned reads = 4;
    for (unsigned n = 0; n < device::CardRouting::kMaxSdiOutputs; ++n)
    {
        if ((routedOutputs_ & (device::routing::kSdiOut << n)) == 0)
            routing.sdiOutputs &= ~(1u << n);
        else if ((routing.sdiOutputs & (1u << n)) != 0)
            reads += 2;
    }
    if ((routedOutputs_ & device::routing::kHdmiOut) == 0)
        routing.haveHdmiOut = false;
    else if (routing.haveHdmiOut)
        reads += 2;
    spend(reads * model_.registerReadNs);
    return true;
}

bool FakeCardAudio::sdiSends(unsigned connector, bool& sends)
{
    if (!model_.bidirectionalSdi || connector >= model_.sdiOutputs)
        return false;
    spend(model_.registerReadNs);
    sends = ((model_.sdiSending >> connector) & 1u) != 0;
    return true;
}

unsigned FakeCardAudio::applyRouting(const device::CardRouting& r, unsigned parts, bool& ok)
{
    update();   // what came in so far, under the old routing
    ok = true;
    unsigned written = 0;
    parts &= device::routing::kWritable;
    const unsigned has = device::routingParts(r);
    if ((parts & ~has) != 0)
        ok = false;   // asked for a part it was not given
    parts &= has;

    if ((parts & device::routing::kSource) != 0)
    {
        if (r.source == device::InputSource::Unchanged)
            ok = false;
        else
        {
            routing_.source = r.source;
            routing_.sourceCode = r.source == device::InputSource::Other ? r.sourceCode : 0;
            routing_.embeddedInput = r.source == device::InputSource::Embedded ? r.embeddedInput : 0;
            written |= device::routing::kSource;
            // As SetAudioSystemInputSource: the shared select follows a
            // source it names.
            if (routing_.haveXpandSelect && r.source != device::InputSource::Other)
            {
                routing_.xpandSelect = r.source == device::InputSource::Analog ? 1u : 0u;
                written |= device::routing::kXpandSelect;
            }
        }
    }
    if ((parts & device::routing::kXpandSelect) != 0)
    {
        if (!routing_.haveXpandSelect || r.xpandSelect > 15)
            ok = false;
        else
        {
            routing_.xpandSelect = r.xpandSelect;
            written |= device::routing::kXpandSelect;
        }
    }
    if ((parts & device::routing::kAnalogLevel) != 0)
    {
        const bool named = device::isAnalogLevel(r.analogLevelDbu);
        if (!routing_.haveAnalogLevel || (!named && (r.analogLevelDbu != 0 || r.analogLevelCode > 15)))
            ok = false;
        else
        {
            routing_.analogLevelDbu = r.analogLevelDbu;
            routing_.analogLevelCode = named ? 0u : r.analogLevelCode;
            written |= device::routing::kAnalogLevel;
        }
    }
    for (unsigned g = 0; g < device::CardRouting::kMaxAesGroups; ++g)
    {
        const unsigned bit = device::routing::kAesGroup << g;
        if ((parts & bit) == 0)
            continue;
        if (g >= routing_.aesGroups || !validSource(r.aes[g], 4, 13))
        {
            ok = false;
            continue;
        }
        routing_.aes[g] = r.aes[g];
        written |= bit;
    }
    if ((parts & device::routing::kMonitor) != 0)
    {
        if (!validSource(r.monitor, 2, kMaxChannels - 1))
            ok = false;
        else
        {
            routing_.monitor = r.monitor;
            written |= device::routing::kMonitor;
        }
    }
    for (unsigned n = 0; n < device::CardRouting::kMaxSdiOutputs; ++n)
    {
        const unsigned bit = device::routing::kSdiOut << n;
        if ((parts & bit) == 0)
            continue;
        if ((routing_.sdiOutputs & (1u << n)) == 0 || r.sdiOut[n].audioSystem < 1 || r.sdiOut[n].audioSystem > 8)
        {
            ok = false;
            continue;
        }
        routing_.sdiOut[n] = r.sdiOut[n];
        written |= bit;
    }
    if ((parts & device::routing::kHdmiOut) != 0)
    {
        const device::HdmiOutputAudio h = r.hdmiOut;
        const bool valid =
            h.audioSystem >= 1 && h.audioSystem <= 8 &&
            ((h.channels == 8 && (h.firstChannel == 1 || h.firstChannel == 9)) ||
             (h.channels == 2 && h.firstChannel >= 1 && h.firstChannel <= 15 && h.firstChannel % 2 == 1));
        if (!routing_.haveHdmiOut || !valid)
            ok = false;
        else
        {
            routing_.hdmiOut = h;
            written |= device::routing::kHdmiOut;
        }
    }
    if ((parts & device::routing::kTaskMode) != 0)
    {
        if (!routing_.haveTaskMode || r.taskMode == device::TaskMode::Unknown)
            ok = false;
        else
        {
            routing_.taskMode = r.taskMode;
            written |= device::routing::kTaskMode;
        }
    }
    return written;
}

bool FakeCardAudio::writeRouting(const device::CardRouting& routing, unsigned parts)
{
    bool ok = true;
    const unsigned written = applyRouting(routing, parts, ok);
    touched_ |= written;
    routedOutputs_ |= written & (device::routing::kSdiOuts | device::routing::kHdmiOut);
    device::copyRoutingParts(ours_, routing_, written);
    return ok;
}

bool FakeCardAudio::anotherProgramSets(const device::CardRouting& routing, unsigned parts)
{
    bool ok = true;
    applyRouting(routing, parts, ok);
    return ok;
}

bool FakeCardAudio::readAnalogLevel(int& fullScaleDbu)
{
    if (!routing_.haveAnalogLevel || routing_.analogLevelDbu == 0)
        return false;
    fullScaleDbu = routing_.analogLevelDbu;
    return true;
}

void FakeCardAudio::restore()
{
    if (configured_)
    {
        update();
        outputRunning_ = false;
        inputRunning_ = false;
        fetched_ = 0;
        written_ = 0;
        captureEnable_ = config_.captureEnable;
    }
    // What this program set goes back as it was, where the card still holds
    // it; the source is this audio system's own and always goes back. The
    // task mode stays while another program holds the card; an OEM mode
    // found while one held it was that program's, and with the card held no
    // longer it goes to standard instead.
    restoreNote_.clear();
    auto note = [this](const std::string& text) { restoreNote_ += (restoreNote_.empty() ? "" : "; ") + text; };
    const unsigned theirs = device::routingDifferences(routing_, ours_) & ~device::routing::kSource;
    unsigned back = touched_ & ~theirs & device::routing::kWritable;
    for (unsigned part = 1; part <= device::routing::kLast; part <<= 1)
    {
        if ((touched_ & theirs & part) != 0)
            note(std::string(partName(part)) + " left as another program set it");
    }
    if ((back & device::routing::kTaskMode) != 0 && heldByAnother_)
    {
        back &= ~device::routing::kTaskMode;
        note("the task mode left as it is: AJIN, process 4242, holds the card");
    }
    device::copyRoutingParts(routing_, saved_, back);
    if ((back & device::routing::kTaskMode) != 0 && heldAtStart_ && saved_.taskMode == device::TaskMode::Oem)
    {
        routing_.taskMode = device::TaskMode::Standard;
        note("the task mode put to standard, not back to OEM: AJIN, process 4242, which held the card at the start, "
             "holds it no longer");
    }
    touched_ = 0;
    ours_ = device::CardRouting{};
    locked_.clear();
    subscribed_.clear();
}

} // namespace ajaduplex::sim
