// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "LoopProbe.h"

#include "common/DriverSettings.h"
#include "common/Stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace ajaduplex::probe
{

namespace
{

using engine::AudioEngine;
using engine::BlockBuffers;
using engine::BlockInfo;
using engine::EngineClient;
using engine::EngineEvent;

constexpr unsigned kEngineChannels = 16;   // the audio system's, of which AES 1-8 are cabled
constexpr std::int64_t kSpan = LoopSignal::kCounterSpan;

/// x modulo the counter span, in [0, span).
std::int64_t modSpan(std::int64_t x)
{
    const std::int64_t r = x % kSpan;
    return r < 0 ? r + kSpan : r;
}

std::string fixed(double value, int decimals)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

std::string framesAndMs(std::int64_t frames)
{
    return std::to_string(frames) + " frames = " + fixed(static_cast<double>(frames) / 48.0, 2) + " ms";
}

std::string aesName(unsigned channel)
{
    return "AES " + std::to_string(channel + 1);
}

std::string sideName(unsigned channel)
{
    return channel == 0 ? "L" : "R";
}

/// A tone channel as the report names it: L and R on the analog path.
std::string toneName(LoopPath path, unsigned channel)
{
    return path == LoopPath::Analog ? sideName(channel) : aesName(channel);
}

std::string framesAndMs(double frames)
{
    return fixed(frames, 1) + " frames = " + fixed(frames / 48.0, 2) + " ms";
}

/// With its sign, and a value that rounds to zero (-0.0 included) as +0.
std::string signedFixed(double value, int decimals)
{
    std::string text = fixed(value, decimals);
    if (text[0] == '-' && text.find_first_not_of("-0.") == std::string::npos)
        text.erase(0, 1);
    return text[0] == '-' ? text : "+" + text;
}

/// A count of frames with its sign: +3, -2, +0.
std::string signedFrames(std::int64_t frames)
{
    return (frames < 0 ? "" : "+") + std::to_string(frames);
}

/// 0-based channels as the user types them: "1-8", "1,3,5-6".
std::string channelList(const std::vector<unsigned>& channels)
{
    std::string text;
    for (std::size_t i = 0; i < channels.size();)
    {
        std::size_t j = i;
        while (j + 1 < channels.size() && channels[j + 1] == channels[j] + 1)
            ++j;
        text += (text.empty() ? "" : ",") + std::to_string(channels[i] + 1);
        if (j > i)
            text += "-" + std::to_string(channels[j] + 1);
        i = j + 1;
    }
    return text;
}

/// What both loop clients share: the engine's events, for the report.
class RecordingClient : public EngineClient
{
public:
    RecordingClient() { firstEvents.reserve(LoopResult::kMaxEvents); }   // nothing allocates on the engine's path

    void onEngineEvent(EngineEvent event, const BlockInfo& info) override
    {
        ++events;
        if (firstEvents.size() < LoopResult::kMaxEvents)
        {
            LoopEvent e{event, info.samplePosition, {}};
            if ((event == EngineEvent::DmaError || event == EngineEvent::DmaRecovered) && engine != nullptr)
                e.dma = engine->lastDmaError();
            firstEvents.push_back(e);
        }
    }

    std::vector<LoopEvent> firstEvents;
    std::uint64_t events = 0;
    const AudioEngine* engine = nullptr;   ///< for which DMA failed
};

/// The engine's client for an AES loop run: writes the counter on the
/// looped outputs (the others stay silent) and checks what the looped
/// inputs bring.
class LoopClient final : public RecordingClient
{
public:
    LoopClient(unsigned frames, const std::vector<unsigned>& looped) : frames_(frames), looped_(looped)
    {
        for (unsigned h = 0; h < 2; ++h)
        {
            storage_[h].assign(2 * looped_.size(), std::vector<std::int32_t>(frames, 0));
            for (std::size_t i = 0; i < looped_.size(); ++i)
            {
                inputs_[h][looped_[i]] = storage_[h][2 * i].data();
                outputs_[h][looped_[i]] = storage_[h][2 * i + 1].data();
            }
        }
        for (const unsigned channel : looped_)
            verifiers.emplace_back(channel);
    }

    BlockBuffers buffers(unsigned half) override
    {
        return BlockBuffers{inputs_[half], outputs_[half]};
    }

    void process(const BlockInfo& info, unsigned half) override
    {
        const std::int64_t first = static_cast<std::int64_t>(info.index) * frames_;
        for (std::size_t i = 0; i < looped_.size(); ++i)
        {
            const std::int32_t* in = storage_[half][2 * i].data();
            for (unsigned f = 0; f < frames_; ++f)
                verifiers[i].feed(info.samplePosition + f, in[f]);

            std::int32_t* out = storage_[half][2 * i + 1].data();
            for (unsigned f = 0; f < frames_; ++f)
                out[f] = LoopSignal::encode(first + f, looped_[i]);
        }
    }

    std::vector<ChannelVerifier> verifiers;

private:
    unsigned frames_;
    std::vector<unsigned> looped_;
    std::vector<std::vector<std::int32_t>> storage_[2];      // per looped channel: input, output
    std::int32_t* inputs_[2][kEngineChannels] = {};          // null: not looped, not filled
    const std::int32_t* outputs_[2][kEngineChannels] = {};   // null: silence
};

/// The engine's client for a tone loop run: a tone on each looped output
/// (the others stay silent), the same inputs checked.
class ToneLoopClient final : public RecordingClient
{
public:
    /// `tones`: how many are in play (2 on the analog path, 8 through AES),
    /// for the verifiers to tell crossed cables.
    ToneLoopClient(unsigned frames, const std::vector<unsigned>& looped, unsigned tones, double toneDbfs)
        : frames_(frames), looped_(looped), scale_(ToneSignal::scaleFor(toneDbfs))
    {
        for (unsigned h = 0; h < 2; ++h)
        {
            storage_[h].assign(2 * looped_.size(), std::vector<std::int32_t>(frames, 0));
            for (std::size_t i = 0; i < looped_.size(); ++i)
            {
                inputs_[h][looped_[i]] = storage_[h][2 * i].data();
                outputs_[h][looped_[i]] = storage_[h][2 * i + 1].data();
            }
        }
        verifiers.reserve(looped_.size());
        for (const unsigned channel : looped_)
            verifiers.emplace_back(channel, tones, toneDbfs);
    }

    BlockBuffers buffers(unsigned half) override
    {
        return BlockBuffers{inputs_[half], outputs_[half]};
    }

    void process(const BlockInfo& info, unsigned half) override
    {
        const std::int64_t first = static_cast<std::int64_t>(info.index) * frames_;
        for (std::size_t i = 0; i < looped_.size(); ++i)
        {
            const std::int32_t* in = storage_[half][2 * i].data();
            for (unsigned f = 0; f < frames_; ++f)
                verifiers[i].feed(info.samplePosition + f, in[f]);

            std::int32_t* out = storage_[half][2 * i + 1].data();
            for (unsigned f = 0; f < frames_; ++f)
                out[f] = ToneSignal::word(ToneSignal::value(first + f, looped_[i]) * scale_);
        }
    }

    std::vector<ToneVerifier> verifiers;

private:
    unsigned frames_;
    std::vector<unsigned> looped_;
    double scale_;
    std::vector<std::vector<std::int32_t>> storage_[2];      // per looped channel: input, output
    std::int32_t* inputs_[2][kEngineChannels] = {};
    const std::int32_t* outputs_[2][kEngineChannels] = {};
};

} // namespace

// ---------------------------------------------------------------------------
// Signal and verifier
// ---------------------------------------------------------------------------

std::int32_t LoopSignal::encode(std::int64_t frame, unsigned channel)
{
    const std::uint32_t payload = 1u | ((channel % kChannels) << 1) |
                                  ((static_cast<std::uint32_t>(modSpan(frame)) & kCounterMask) << 4);
    // As a signed 16-bit value in bits 8-23, the bits above copying its sign.
    const std::int32_t value = payload >= 0x8000u ? static_cast<std::int32_t>(payload) - 0x10000
                                                  : static_cast<std::int32_t>(payload);
    return value * 256;
}

LoopSignal::Decoded LoopSignal::decode(std::int32_t sample)
{
    // Drop the low byte: floor(sample / 256), the 24 bits AES carries.
    const std::int64_t wide = sample;
    const std::int64_t value = wide >= 0 ? wide / 256 : -((-wide + 255) / 256);

    Decoded d;
    d.silent = value == 0;
    if (value < -32768 || value > 32767)
        return d;   // above 16 bits: not the signal
    const std::uint32_t payload = static_cast<std::uint32_t>(value + 65536) & 0xFFFFu;
    d.valid = (payload & 1u) != 0;
    d.channel = (payload >> 1) & 0x7u;
    d.counter = (payload >> 4) & kCounterMask;
    return d;
}

void ChannelVerifier::feed(std::int64_t inputFrame, std::int32_t sample)
{
    const LoopSignal::Decoded d = LoopSignal::decode(sample);
    if (!d.valid)
    {
        // Silence until the first block comes round is expected.
        if (!d.silent)
            ++garbage;
        else if (locked_)
            ++silent;
        return;
    }
    if (d.channel != outputChannel_ % LoopSignal::kChannels)
    {
        ++wrongChannel;
        heardChannel = static_cast<int>(d.channel);
        return;
    }

    const std::int64_t trip = modSpan(inputFrame - static_cast<std::int64_t>(d.counter));
    if (!locked_)
    {
        locked_ = true;
        expected_ = d.counter;
        roundTrip = minRoundTrip = maxRoundTrip = trip;
    }

    const std::uint32_t forward = (d.counter - expected_) & LoopSignal::kCounterMask;
    if (forward == 0)
        ++good;
    else if (forward < LoopSignal::kCounterSpan / 2)
    {
        lost += forward;   // the frames in between never arrived
        ++jumps;
    }
    else
    {
        if (((expected_ - d.counter) & LoopSignal::kCounterMask) == 1)
            ++repeated;    // the previous frame once more
        else
            ++jumpsBack;
        ++jumps;
    }
    expected_ = (d.counter + 1) & LoopSignal::kCounterMask;

    // The round trip only moves at a discontinuity (a gap of silence, a
    // jump); it is followed as long as each move is under half the span.
    std::int64_t delta = modSpan(trip - roundTrip);
    if (delta >= kSpan / 2)
        delta -= kSpan;
    if (delta != 0)
    {
        ++roundTripChanges;
        roundTrip += delta;
        minRoundTrip = std::min(minRoundTrip, roundTrip);
        maxRoundTrip = std::max(maxRoundTrip, roundTrip);
    }
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

namespace
{

template <typename F>
std::uint64_t sum(const std::vector<ChannelVerifier>& channels, F field)
{
    std::uint64_t total = 0;
    for (const ChannelVerifier& c : channels)
        total += field(c);
    return total;
}

} // namespace

std::uint64_t LoopResult::lost() const { return sum(channels, [](const ChannelVerifier& c) { return c.lost; }); }
std::uint64_t LoopResult::repeated() const { return sum(channels, [](const ChannelVerifier& c) { return c.repeated; }); }
std::uint64_t LoopResult::silent() const { return sum(channels, [](const ChannelVerifier& c) { return c.silent; }); }
std::uint64_t LoopResult::wrong() const { return sum(channels, [](const ChannelVerifier& c) { return c.wrongChannel; }); }
std::uint64_t LoopResult::garbage() const { return sum(channels, [](const ChannelVerifier& c) { return c.garbage; }); }

bool LoopResult::clean() const
{
    if (!started || interrupted || engine.lateOutputs || engine.outputMoves || engine.inputOverruns || engine.stalls ||
        engine.dmaErrors)
        return false;
    if (playsTones(settings.path))
    {
        const std::size_t looped =
            settings.path == LoopPath::Analog ? std::size_t(ToneSignal::kChannels) : settings.channels.size();
        if (tones.empty() || tones.size() != looped)
            return false;
        for (const ToneVerifier& a : tones)
        {
            if (!a.clean())
                return false;
        }
        return true;
    }
    if (channels.empty())
        return false;
    for (const ChannelVerifier& c : channels)
    {
        if (!c.clean())
            return false;
    }
    return true;
}

LoopResult runLoop(device::CardAudio& card, Clock& clock, const LoopSettings& settings,
                   const std::function<bool()>& stopRequested, LoopProgress* progress)
{
    LoopResult result;
    result.settings = settings;
    const bool analogPath = settings.path == LoopPath::Analog;
    if (!analogPath)
    {
        if (settings.channels.empty())
        {
            result.error = "no channels to loop";
            return result;
        }
        for (const unsigned c : settings.channels)
        {
            if (c >= LoopSignal::kChannels)
            {
                result.error = "AES channels are 1-8, not " + std::to_string(c + 1);
                return result;
            }
        }
    }

    engine::EngineConfig config;
    config.blockFrames = settings.frames;
    config.guardFrames = settings.guard;
    config.headMode = settings.headMode;
    config.audio.channels = kEngineChannels;
    config.audio.source = analogPath ? device::InputSource::Analog : device::InputSource::Aes;
    config.audio.keepXpandSelect = false;   // the input must really be what we loop
    config.audio.routeAesOutputs = true;    // AES out (and maybe the analog out, Q4) plays this audio system
    config.audio.routeMonitor = analogPath; // the analog out may follow the monitor instead
    if (settings.oemTasks)
        config.audio.taskMode = device::TaskMode::Oem;   // AJA's services keep off the routing meanwhile
    // The XLR outputs play the tones too, AES 1-2's on the KONA X (Q4):
    // the report says how loud.
    result.haveAnalogLevel = playsTones(settings.path) && card.readAnalogLevel(result.analogLevelDbu);

    std::unique_ptr<LoopClient> wordsClient;
    std::unique_ptr<ToneLoopClient> toneClient;
    RecordingClient* client = nullptr;
    if (analogPath)
        client = (toneClient = std::make_unique<ToneLoopClient>(settings.frames, std::vector<unsigned>{0, 1},
                                                                ToneSignal::kChannels, ToneSignal::kToneDbfs))
                     .get();
    else if (settings.path == LoopPath::Aes)
        client = (toneClient = std::make_unique<ToneLoopClient>(settings.frames, settings.channels,
                                                                ToneSignal::kMaxTones, ToneSignal::kAesToneDbfs))
                     .get();
    else
        client = (wordsClient = std::make_unique<LoopClient>(settings.frames, settings.channels)).get();

    AudioEngine engine(card, clock);
    client->engine = &engine;
    if (!engine.start(config, *client, result.error))
        return result;
    result.started = true;

    // A block is found complete at or after it is due, never before: the
    // soonest one near each end of the run dates the card's clock best.
    struct Anchor
    {
        bool set = false;
        std::int64_t position = 0;
        std::int64_t hostNs = 0;
        double offsetNs = 0.0;   // host time minus the nominal time of the position
    };
    Anchor early, late;
    auto consider = [](Anchor& a, std::int64_t position, std::int64_t hostNs) {
        const double offset = static_cast<double>(hostNs) - static_cast<double>(position) * (1e9 / 48000.0);
        if (!a.set || offset < a.offsetNs)
            a = Anchor{true, position, hostNs, offset};
    };

    const std::int64_t begin = clock.nowNs();
    const std::int64_t end = begin + static_cast<std::int64_t>(settings.seconds * 1e9);
    const std::int64_t kWindowNs = 500000000;
    while (clock.nowNs() < end)
    {
        if (stopRequested && stopRequested())
        {
            result.interrupted = true;
            break;
        }
        if (!engine.runOnce())
            break;
        if (progress != nullptr)
        {
            const engine::EngineStats stats = engine.stats();
            std::uint64_t problems =
                stats.lateOutputs + stats.outputMoves + stats.inputOverruns + stats.stalls + stats.dmaErrors;
            bool allLocked = true;
            if (wordsClient)
            {
                for (const ChannelVerifier& c : wordsClient->verifiers)
                {
                    problems += c.lost + c.repeated + c.jumpsBack + c.silent + c.wrongChannel + c.garbage;
                    allLocked = allLocked && c.locked();
                }
            }
            else
            {
                for (const ToneVerifier& a : toneClient->verifiers)
                {
                    problems += a.glitches + a.slips + a.levelChanges;
                    allLocked = allLocked && a.heard;
                }
            }
            progress->blocks.store(stats.blocks, std::memory_order_relaxed);
            progress->problems.store(problems, std::memory_order_relaxed);
            progress->allLocked.store(allLocked, std::memory_order_relaxed);
        }
        std::int64_t position = 0, hostNs = 0;
        if (end - begin >= 4 * kWindowNs && engine.lastBlockPosition(position, hostNs))
        {
            if (hostNs - begin >= kWindowNs && hostNs - begin < 2 * kWindowNs)
                consider(early, position, hostNs);
            else if (end - hostNs <= kWindowNs)
                consider(late, position, hostNs);
        }
    }
    result.seconds = static_cast<double>(clock.nowNs() - begin) / 1e9;
    if (early.set && late.set && late.hostNs > early.hostNs)
    {
        const double frames = static_cast<double>(late.position - early.position);
        const double nominal = static_cast<double>(late.hostNs - early.hostNs) * 48000.0 / 1e9;
        result.clockPpm = (frames / nominal - 1.0) * 1e6;
        result.clockMeasured = true;
    }
    {
        // Where the blocks meet the ring's end now: O and R modulo a block.
        const device::RingLayout layout = engine.layout();
        const std::uint32_t frame = layout.frameBytes ? layout.frameBytes : 1u;
        const unsigned n = settings.frames;
        const std::uint32_t endFrames = layout.ringBytes / frame;
        auto before = [&](std::uint32_t position) {
            const std::uint32_t start = position / frame;
            const std::uint32_t phase = (endFrames - start % n) % n;   // frames from a block start to the end
            return phase == 0 ? n : phase;
        };
        result.outputFramesBeforeEnd = before(engine.writePosition());
        result.inputFramesBeforeEnd = before(engine.readPosition());
    }
    engine.stop();
    result.retired = engine.retiredBuffers();
    result.bufferCheck = engine.bufferCheck();
    result.engine = engine.stats();
    if (wordsClient)
        result.channels = wordsClient->verifiers;
    else
    {
        // Outside the engine's time: the sweep's correlation.
        for (ToneVerifier& a : toneClient->verifiers)
            a.finish();
        result.tones = toneClient->verifiers;
    }
    result.firstEvents = client->firstEvents;
    result.events = client->events;
    return result;
}

std::vector<std::pair<unsigned, unsigned>> sweepGrid(unsigned frames, int guard)
{
    const std::vector<unsigned> allFrames = {256, 128, 64, 32};
    const std::vector<unsigned> allGuards = {64, 32, 16, 8};
    const std::vector<unsigned> ns = frames ? std::vector<unsigned>{frames} : allFrames;
    const std::vector<unsigned> gs = guard >= 0 ? std::vector<unsigned>{static_cast<unsigned>(guard)} : allGuards;
    std::vector<std::pair<unsigned, unsigned>> grid;
    for (const unsigned n : ns)
    {
        for (const unsigned g : gs)
            grid.emplace_back(n, g);
    }
    return grid;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

namespace
{

/// What went wrong, comma separated; empty when nothing did.
std::string problems(const LoopResult& r)
{
    std::string text;
    auto add = [&text](const std::string& item) { text += (text.empty() ? "" : ", ") + item; };
    auto count = [&add](std::uint64_t value, const char* what) {
        if (value)
            add(std::to_string(value) + " " + what);
    };
    if (r.engine.lateOutputs)
        add(std::to_string(r.engine.lateOutputs) + " late outputs (" + std::to_string(r.engine.droppedOutputFrames) +
            " frames dropped)");
    count(r.engine.outputMoves, "output moves (out of step with the input: the latency changed)");
    count(r.engine.inputOverruns, "input overruns");
    count(r.engine.stalls, "input stalls");
    count(r.engine.dmaErrors, "DMA errors");
    count(r.engine.dmaRecovered, "DMA errors saved by a second try from a spare buffer (nothing lost)");

    if (playsTones(r.settings.path))
    {
        const bool analog = r.settings.path == LoopPath::Analog;
        for (const ToneVerifier& a : r.tones)
        {
            const std::string name = toneName(r.settings.path, a.channel());
            if (!a.checked)
                add(name + ": the run ended before a whole second could be checked");
            else if (a.crossed && analog)
                add("in " + name + " hears the other tone: L and R crossed");
            else if (a.crossed)
                add(name + " in hears " + aesName(a.channel() ^ 1u) + "'s tone: crossed cables");
            else if (!a.heard && analog)
                add("in " + name + " never heard its tone: check the XLR cable from out " + name + " to in " + name +
                    ", and where the analog output takes its signal from (Q4)");
            else if (!a.heard)
                add(name + " in never heard " + name + " out: check that cable");
            else
            {
                if (a.glitches)
                {
                    std::string at;
                    for (const std::int64_t frame : a.glitchFrames)
                        at += (at.empty() ? "" : ", ") + fixed(static_cast<double>(frame) / 48000.0, 3) + " s";
                    if (a.glitches > a.glitchFrames.size())
                        at += ", ...";
                    add(std::to_string(a.glitches) + " glitches on " + name + " (at " + at + ")");
                }
                if (a.slips)
                {
                    std::string slip = name + " slipped " + std::to_string(a.slips) + (a.slips == 1 ? " time" : " times");
                    if (a.worstSlipFrames != 0)
                        slip += std::string(a.slips == 1 ? " (" : " (worst ") +
                                std::to_string(std::llabs(static_cast<long long>(a.worstSlipFrames))) +
                                (a.worstSlipFrames > 0 ? " frames lost" : " frames late") +
                                (a.slipsOfUnknownSize ? ", " + std::to_string(a.slipsOfUnknownSize) +
                                                            " of unknown size)"
                                                      : std::string(")"));
                    else if (a.slipsOfUnknownSize)
                        slip += " (size unknown: no clean second before it, or the delay wandered meanwhile)";
                    add(slip);
                }
                if (a.levelChanges)
                    add("level on " + name + " off by up to " + signedFixed(a.worstLevelChangeDb, 1) + " dB in " +
                        std::to_string(a.levelChanges) + " seconds");
                if (!a.roundTripFound)
                    add("the sweep was not found on " + name);
            }
        }
        if (r.interrupted)
            add("interrupted");
        return text;
    }

    // Words that are mostly not the signal were changed on the way: what
    // else was counted on them means nothing (the KONA X's AES inputs,
    // 2026-11-10: half garbage, the rest from any channel).
    std::vector<unsigned> changed;
    for (const ChannelVerifier& c : r.channels)
    {
        if (c.garbage > c.good)
            changed.push_back(c.outputChannel());
    }
    if (!changed.empty())
    {
        add("AES " + channelList(changed) + " in " + (changed.size() == 1 ? "brings" : "bring") +
            " words changed (resampled? a path under 24 bits?): the KONA X's AES inputs resample, and --path aes "
            "checks them with tones");
        if (r.interrupted)
            add("interrupted");
        return text;
    }

    count(r.lost(), "frames lost");
    count(r.repeated(), "repeated");
    count(r.silent(), "silent");
    count(r.wrong(), "from the wrong channel");
    count(r.garbage(), "garbage words");

    for (const ChannelVerifier& c : r.channels)
    {
        const unsigned k = c.outputChannel();
        if (c.heardChannel >= 0 && !c.locked())
            add(aesName(k) + " in hears " + aesName(static_cast<unsigned>(c.heardChannel)) +
                " out: crossed cables");
        else if (!c.locked())
            add(aesName(k) + " in never heard " + aesName(k) + " out: check that cable");
        else if (c.jumpsBack)
            add(std::to_string(c.jumpsBack) + " jumps back on " + aesName(k));
    }
    if (r.silent() && !r.engine.lateOutputs && !r.engine.outputMoves && !r.engine.stalls && !r.engine.inputOverruns &&
        !r.engine.dmaErrors)
        add("silence without a late output: did something re-route AES out or the input meanwhile "
            "(AJA services, Control Panel)?");
    if (r.interrupted)
        add("interrupted");
    return text;
}

/// The run's card clock: measured over the run when it could be, else the
/// engine's fit.
double clockOf(const LoopResult& r)
{
    return r.clockMeasured ? r.clockPpm : r.engine.clockPpm;
}

/// The tones' round trip: the smallest of them, or -1.
double toneRoundTrip(const LoopResult& r)
{
    double trip = -1.0;
    for (const ToneVerifier& a : r.tones)
    {
        if (a.roundTripFound && (trip < 0.0 || a.roundTrip < trip))
            trip = a.roundTrip;
    }
    return trip;
}

std::string roundTripText(const LoopResult& r)
{
    if (playsTones(r.settings.path))
    {
        std::vector<unsigned> found;
        double low = 0.0, high = 0.0;
        for (const ToneVerifier& a : r.tones)
        {
            if (!a.roundTripFound)
                continue;
            low = found.empty() ? a.roundTrip : std::min(low, a.roundTrip);
            high = found.empty() ? a.roundTrip : std::max(high, a.roundTrip);
            found.push_back(a.channel());
        }
        if (found.empty())
            return std::string();
        const bool analog = r.settings.path == LoopPath::Analog;
        const std::string where = analog ? (found.size() == 2 ? std::string("L and R") : sideName(found[0]))
                                         : "AES " + channelList(found);
        if (high - low < 0.05)
            return "round trip " + framesAndMs(low) + " on " + where;
        if (analog)
            return "round trip L " + framesAndMs(r.tones[0].roundTrip) + ", R " + framesAndMs(r.tones[1].roundTrip);
        return "round trip " + fixed(low, 1) + " to " + framesAndMs(high) + " on " + where;
    }
    for (const ChannelVerifier& c : r.channels)
    {
        if (!c.locked())
            continue;
        std::string text = "round trip " + framesAndMs(c.minRoundTrip);
        if (c.maxRoundTrip != c.minRoundTrip)
            text += " to " + framesAndMs(c.maxRoundTrip) + " (" + std::to_string(c.roundTripChanges) + " changes)";
        return text;
    }
    return std::string();
}

/// How the blocks met the ring's end: "output 40 + 88 frames (one DMA
/// running on past the end, then 88 at the start); input blocks end at it".
std::string ringEndText(const LoopResult& r)
{
    const unsigned n = r.settings.frames;
    auto side = [n](unsigned before, const char* name) {
        if (before == 0 || before >= n)
            return std::string(name) + " blocks end at it";
        return std::string(name) + " " + std::to_string(before) + " + " + std::to_string(n - before) +
               " frames (one DMA running on past the end, then " + std::to_string(n - before) + " at the start)";
    };
    return side(r.outputFramesBeforeEnd, "output") + "; " + side(r.inputFramesBeforeEnd, "input");
}

std::string runLine(const LoopResult& r)
{
    if (!r.started)
        return "did not start: " + r.error;

    std::string line = std::string(r.clean() ? "CLEAN" : "FAILED") + " over " + fixed(r.seconds, 1) + " s, " +
                       std::to_string(r.engine.blocks) + " blocks";
    const std::string wrong = problems(r);
    if (!wrong.empty())
        line += "\n" + wrong;

    const std::string trip = roundTripText(r);
    if (!trip.empty() && playsTones(r.settings.path))
    {
        const double engineFrames = static_cast<double>(r.settings.frames + r.leadFrames());
        line += "\n" + trip + "; N + S = " + fixed(engineFrames, 0) +
                (r.settings.path == LoopPath::Analog ? ", card, converters and cable "
                                                     : ", card, cable and the AES inputs' resampler ") +
                fixed(toneRoundTrip(r) - engineFrames, 1) + " frames";
    }
    for (const ToneVerifier& a : r.tones)
    {
        if (!a.heard)
            continue;
        line += "\n" + toneName(r.settings.path, a.channel()) + ": back at " + fixed(a.levelDbfs, 1) +
                " dBFS (gain " + signedFixed(a.gainDb, 2) + " dB), noise and distortion " + fixed(a.noiseDbfs, 1) +
                " dBFS (worst second " + fixed(a.worstNoiseDbfs, 1) + "), delay drifted " +
                signedFixed(a.driftUs, 2) + " us, wander " + fixed(a.wanderUs, 2) + " us" +
                (a.inverted ? ", polarity inverted" : "");
    }
    if (!trip.empty() && r.settings.path == LoopPath::AesWords)
    {
        std::int64_t minTrip = -1;
        for (const ChannelVerifier& c : r.channels)
        {
            if (c.locked())
                minTrip = minTrip < 0 ? c.minRoundTrip : std::min(minTrip, c.minRoundTrip);
        }
        const std::int64_t engineFrames = static_cast<std::int64_t>(r.settings.frames) + r.leadFrames();
        line += "\n" + trip + "; N + S = " + std::to_string(engineFrames) + ", card and cable " +
                std::to_string(minTrip - engineFrames) + " frames";
    }
    line += "\nmin output margin " + std::to_string(r.engine.minOutputMarginFrames) + " frames; max DMA in " +
            formatNs(r.engine.maxDmaInNs) + ", out " + formatNs(r.engine.maxDmaOutNs) + ", client " +
            formatNs(r.engine.maxClientNs) + "; card clock " + signedFixed(clockOf(r), 1) + " ppm" +
            (r.clockMeasured ? "" : " (engine's estimate)") + "; output within " +
            signedFrames(r.engine.minOutputStepFrames) + " to " + signedFrames(r.engine.maxOutputStepFrames) +
            " frames of the input's timing";
    if (!r.firstEvents.empty())
    {
        std::string events;
        for (const LoopEvent& e : r.firstEvents)
        {
            events += (events.empty() ? "" : ", ") + std::string(engine::toString(e.event)) + " at " +
                      fixed(static_cast<double>(e.samplePosition) / 48000.0, 3) + " s";
            if (e.event == engine::EngineEvent::DmaError || e.event == engine::EngineEvent::DmaRecovered)
            {
                char detail[400];
                engine::describeDmaError(e.dma, detail, sizeof(detail));
                events += std::string(" (") + detail + ")";
            }
        }
        if (r.events > r.firstEvents.size())
            events += ", ... (" + std::to_string(r.events) + " in all)";
        line += "\nevents: " + events;
    }
    line += "\nat the ring's end: " + ringEndText(r);
    const std::string bufferCheck = engine::describeBufferCheck(r.bufferCheck);
    if (!bufferCheck.empty())
        line += "\nbuffers: " + bufferCheck;
    for (const engine::RetiredBuffer& b : r.retired)
    {
        line += "\nbuffer retired after a failed DMA (" + std::string(b.output ? "output" : "input") + ", " +
                std::to_string(b.bytes) + " bytes at ring offset " + std::to_string(b.ringOffset) +
                ", system error " + std::to_string(b.systemError) + "): ";
        if (!b.checked)
            line += "not tried again";
        else if (b.failsAgain)
            line += "the same DMA from it failed again at stop (system error " + std::to_string(b.againSystemError) +
                    "), the card stopped: the buffer is bad";
        else
            line += "the same DMA from it went through at stop, the card stopped: a passing fault, or one that "
                    "needs the card running";
    }
    return line;
}

std::string runKey(const LoopResult& r)
{
    return "N " + std::to_string(r.settings.frames) + ", G " + std::to_string(r.settings.guard) + " (S " +
           std::to_string(r.leadFrames()) + ")";
}

} // namespace

std::string loopSummaryLine(const LoopResult& r)
{
    if (!r.started)
        return "did not start: " + r.error;
    std::string line = r.clean() ? "CLEAN" : "FAILED";
    const std::string wrong = problems(r);
    if (!wrong.empty())
        line += ": " + wrong;
    const std::string trip = roundTripText(r);
    if (!trip.empty())
        line += (wrong.empty() ? ", " : "; ") + trip;
    return line;
}

void addLoopReport(const LoopRun& run, Report& report)
{
    Report::Section& overview = report.addSection("loop");
    overview.add("device", run.description);
    const LoopPath path = run.results.empty() ? LoopPath::Aes : run.results.front().settings.path;
    const bool analogPath = path == LoopPath::Analog;
    if (!run.results.empty())
    {
        const LoopResult& first = run.results.front();
        const LoopSettings& s = first.settings;
        // How loud the tones are where the XLR outputs take them.
        auto dbu = [&first](double dbfs) {
            return first.haveAnalogLevel ? " (" + signedFixed(first.analogLevelDbu + dbfs, 0) +
                                               " dBu with the Xpand at " + signedFixed(first.analogLevelDbu, 0) +
                                               " dBu)"
                                         : std::string();
        };
        if (analogPath)
        {
            overview.add("path", "analog: XLR out L/R cabled to XLR in L/R, through the Xpand's converters");
            overview.add("signal", std::to_string(ToneSignal::toneHz(0)) + " Hz on L, " +
                                       std::to_string(ToneSignal::toneHz(1)) + " Hz on R, at -20 dBFS" +
                                       dbu(ToneSignal::kToneDbfs) +
                                       ", after a 21 ms sweep for the round trip; the other outputs are silent");
            overview.add("analog level",
                         first.haveAnalogLevel ? signedFixed(first.analogLevelDbu, 0) + " dBu at 0 dBFS"
                                               : std::string("not read (no breakout board?)"));
        }
        else
        {
            std::string channels;
            for (const unsigned c : s.channels)
                channels += (channels.empty() ? "" : ", ") + std::to_string(c + 1);
            overview.add("looped channels", "AES " + channels + ", each out to the same in");
            if (path == LoopPath::Aes)
            {
                std::string tones;
                for (const unsigned c : s.channels)
                    tones += (tones.empty() ? "" : ", ") + std::to_string(ToneSignal::toneHz(c)) + " Hz on " +
                             aesName(c);
                overview.add("path", "tones: the KONA X's AES inputs resample, so no word comes back as it was "
                                     "sent (--path aes-words checks word by word where they do not)");
                overview.add("signal", tones + ", at -40 dBFS, after a 21 ms sweep for the round trip; the other "
                                       "outputs are silent. The analog output plays AES out 1-2 too (Q4)" +
                                       dbu(ToneSignal::kAesToneDbfs));
            }
            else
                overview.add("signal", "frame counter on the looped outputs only, peak " +
                                           fixed(LoopSignal::kPeakDbfs, 1) + " dBFS; the other outputs are silent");
        }
        overview.add("head mode", s.headMode == engine::HeadMode::Conservative ? "conservative (W & ~3)"
                                  : s.headMode == engine::HeadMode::LastByte   ? "last byte"
                                                                               : "next byte");
        overview.add("routing", analogPath ? "input source analog (the Xpand's select, shared by every audio "
                                             "system), AES out 1-8 and the monitor from this audio system; put "
                                             "back afterwards"
                                           : "input source AES, AES out 1-8 from this audio system; put back "
                                             "afterwards");
        overview.add("task mode", s.oemTasks ? "OEM while each run lasts, so AJA's services leave the routing "
                                               "alone; put back afterwards"
                                             : "as the card had it (--tasks keep): AJA's services may re-route "
                                               "meanwhile");
    }
    if (run.interrupted)
        overview.add("interrupted", "yes (Ctrl+C): the results are partial");

    Report::Section& runs = report.addSection("runs");
    for (const LoopResult& r : run.results)
        runs.add(runKey(r), runLine(r));

    // Per N, the smallest guard that came out clean.
    Report::Section& verdict = report.addSection("verdict");
    std::vector<unsigned> framesSeen;
    for (const LoopResult& r : run.results)
    {
        if (std::find(framesSeen.begin(), framesSeen.end(), r.settings.frames) == framesSeen.end())
            framesSeen.push_back(r.settings.frames);
    }
    unsigned bestN = 0, bestG = 0, guardAt128 = 0;
    bool clean128 = false;
    for (const unsigned n : framesSeen)
    {
        bool any = false;
        unsigned smallest = 0;
        for (const LoopResult& r : run.results)
        {
            if (r.settings.frames == n && r.clean() && (!any || r.settings.guard < smallest))
            {
                smallest = r.settings.guard;
                any = true;
            }
        }
        // A failure above the smallest clean G means the runs were too short
        // to catch what is rare: say so rather than draw a line.
        std::string failedAbove;
        for (const LoopResult& r : run.results)
        {
            if (any && r.settings.frames == n && !r.clean() && r.settings.guard > smallest)
                failedAbove += (failedAbove.empty() ? "" : ", ") + std::to_string(r.settings.guard);
        }
        std::string text = !any ? std::string("not clean with any G tried")
                                : "clean from G " + std::to_string(smallest);
        if (!failedAbove.empty())
            text += ", but not with G " + failedAbove + ": too rare for runs this short, try longer ones";
        verdict.add("N " + std::to_string(n), text);
        if (any && (bestN == 0 || n < bestN))
        {
            bestN = n;
            bestG = smallest;
        }
        if (any && n == 128)
        {
            clean128 = true;
            guardAt128 = smallest;
        }
    }
    verdict.add("smallest clean", bestN ? "N = " + std::to_string(bestN) + " with G = " + std::to_string(bestG)
                                        : std::string("none"));

    bool exitTest = false;
    for (const LoopResult& r : run.results)
        exitTest = exitTest || (r.settings.frames == 128 && r.clean() && r.seconds >= 3600.0);
    std::string options;
    if (analogPath)
        options = " --path analog";
    else
    {
        if (path == LoopPath::AesWords)
            options = " --path aes-words";
        if (!run.results.empty() && run.results.front().settings.channels != std::vector<unsigned>{0, 1})
            options += " --channels " + channelList(run.results.front().settings.channels);
    }
    // The hour runs at the driver's own guard, or at the smallest one that
    // came out clean here if that is larger: runs of seconds say which
    // guards fail, not which one holds for an hour, and the hour is the
    // driver's to pass as hosts will run it. (On the KONA X, 2026-11-11,
    // the sweeps were clean from G = 8, and ten minutes at G = 8 with
    // video being captured were not: an audio DMA waited 2.8 ms.)
    const unsigned driverGuard = DriverSettings{}.guardFrames;
    const unsigned exitGuard = std::max(guardAt128, driverGuard);
    std::string next = "not run yet: duplex-probe loop" + options + " --frames 128 --guard " +
                       (clean128 ? std::to_string(exitGuard) : std::string("<G>")) + " --seconds 3600";
    if (clean128)
        next += exitGuard == driverGuard ? " (the driver's own guard: runs of seconds say which guards fail, not "
                                           "which one holds for an hour)"
                                         : " (the smallest guard clean here, above the driver's own " +
                                               std::to_string(driverGuard) + ")";
    verdict.add(analogPath              ? "exit test, analog (N = 128, one hour, no glitch)"
                : path == LoopPath::Aes ? "exit test, AES (N = 128, one hour, no glitch)"
                                        : "exit test, AES words (N = 128, one hour, nothing lost)",
                exitTest ? std::string("PASSED") : next);
    if (analogPath)
        verdict.add("converters (Q12)", "card, converters and cable here: what InputLatency plus OutputLatency "
                                        "must add for the XLRs; through AES the inputs' resampler adds its own");
}

namespace
{

std::string analogCsv(const LoopRun& run)
{
    std::string csv =
        "frames,guard,lead,seconds,blocks,late_outputs,dropped_frames,output_moves,overruns,stalls,dma_errors,"
        "dma_recovered,glitches_l,glitches_r,slips_l,slips_r,level_changes_l,level_changes_r,level_l_dbfs,level_r_dbfs,"
        "noise_l_dbfs,noise_r_dbfs,worst_noise_l_dbfs,worst_noise_r_dbfs,drift_l_us,drift_r_us,wander_l_us,"
        "wander_r_us,round_trip_l,round_trip_r,inverted_l,inverted_r,min_margin,max_dma_in_ns,max_dma_out_ns,"
        "max_client_ns,clock_ppm,clock_ppm_engine,clean\n";
    char line[768];
    for (const LoopResult& r : run.results)
    {
        const ToneVerifier none;
        const ToneVerifier& left = r.tones.size() > 0 ? r.tones[0] : none;
        const ToneVerifier& right = r.tones.size() > 1 ? r.tones[1] : none;
        auto trip = [](const ToneVerifier& a) { return a.roundTripFound ? fixed(a.roundTrip, 2) : std::string(); };
        std::snprintf(line, sizeof(line),
                      "%u,%u,%u,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%.2f,"
                      "%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%s,%s,%d,%d,%lld,%lld,%lld,%lld,%s,%.2f,%d\n",
                      r.settings.frames, r.settings.guard, r.leadFrames(), r.seconds,
                      static_cast<unsigned long long>(r.engine.blocks),
                      static_cast<unsigned long long>(r.engine.lateOutputs),
                      static_cast<unsigned long long>(r.engine.droppedOutputFrames),
                      static_cast<unsigned long long>(r.engine.outputMoves),
                      static_cast<unsigned long long>(r.engine.inputOverruns),
                      static_cast<unsigned long long>(r.engine.stalls),
                      static_cast<unsigned long long>(r.engine.dmaErrors),
                      static_cast<unsigned long long>(r.engine.dmaRecovered),
                      static_cast<unsigned long long>(left.glitches), static_cast<unsigned long long>(right.glitches),
                      static_cast<unsigned long long>(left.slips), static_cast<unsigned long long>(right.slips),
                      static_cast<unsigned long long>(left.levelChanges),
                      static_cast<unsigned long long>(right.levelChanges), left.levelDbfs, right.levelDbfs,
                      left.noiseDbfs, right.noiseDbfs, left.worstNoiseDbfs, right.worstNoiseDbfs, left.driftUs,
                      right.driftUs, left.wanderUs, right.wanderUs, trip(left).c_str(), trip(right).c_str(),
                      left.inverted ? 1 : 0, right.inverted ? 1 : 0,
                      static_cast<long long>(r.engine.minOutputMarginFrames),
                      static_cast<long long>(r.engine.maxDmaInNs), static_cast<long long>(r.engine.maxDmaOutNs),
                      static_cast<long long>(r.engine.maxClientNs),
                      r.clockMeasured ? fixed(r.clockPpm, 2).c_str() : "", r.engine.clockPpm, r.clean() ? 1 : 0);
        csv += line;
    }
    return csv;
}

/// AES through tones: up to eight channels, so the worst of them per run
/// (the report has each one).
std::string aesToneCsv(const LoopRun& run)
{
    std::string csv =
        "frames,guard,lead,seconds,blocks,late_outputs,dropped_frames,output_moves,overruns,stalls,dma_errors,"
        "dma_recovered,channels,heard,glitches,slips,level_changes,level_min_dbfs,level_max_dbfs,worst_noise_dbfs,"
        "round_trip_min,round_trip_max,drift_min_us,drift_max_us,wander_max_us,min_margin,max_dma_in_ns,"
        "max_dma_out_ns,max_client_ns,clock_ppm,clock_ppm_engine,clean\n";
    char line[768];
    for (const LoopResult& r : run.results)
    {
        std::uint64_t heard = 0, glitches = 0, slips = 0, levelChanges = 0;
        double levelMin = 0.0, levelMax = 0.0, worstNoise = -200.0, driftMin = 0.0, driftMax = 0.0, wander = 0.0;
        double tripMin = -1.0, tripMax = -1.0;
        for (const ToneVerifier& a : r.tones)
        {
            glitches += a.glitches;
            slips += a.slips;
            levelChanges += a.levelChanges;
            if (a.roundTripFound)
            {
                tripMin = tripMin < 0.0 ? a.roundTrip : std::min(tripMin, a.roundTrip);
                tripMax = std::max(tripMax, a.roundTrip);
            }
            if (!a.heard)
                continue;
            levelMin = heard ? std::min(levelMin, a.levelDbfs) : a.levelDbfs;
            levelMax = heard ? std::max(levelMax, a.levelDbfs) : a.levelDbfs;
            driftMin = heard ? std::min(driftMin, a.driftUs) : a.driftUs;
            driftMax = heard ? std::max(driftMax, a.driftUs) : a.driftUs;
            worstNoise = std::max(worstNoise, a.worstNoiseDbfs);
            wander = std::max(wander, a.wanderUs);
            ++heard;
        }
        auto trip = [](double frames) { return frames >= 0.0 ? fixed(frames, 2) : std::string(); };
        std::snprintf(line, sizeof(line),
                      "%u,%u,%u,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%llu,%llu,%llu,%llu,%.2f,%.2f,%.1f,"
                      "%s,%s,%.3f,%.3f,%.3f,%lld,%lld,%lld,%lld,%s,%.2f,%d\n",
                      r.settings.frames, r.settings.guard, r.leadFrames(), r.seconds,
                      static_cast<unsigned long long>(r.engine.blocks),
                      static_cast<unsigned long long>(r.engine.lateOutputs),
                      static_cast<unsigned long long>(r.engine.droppedOutputFrames),
                      static_cast<unsigned long long>(r.engine.outputMoves),
                      static_cast<unsigned long long>(r.engine.inputOverruns),
                      static_cast<unsigned long long>(r.engine.stalls),
                      static_cast<unsigned long long>(r.engine.dmaErrors),
                      static_cast<unsigned long long>(r.engine.dmaRecovered), r.tones.size(),
                      static_cast<unsigned long long>(heard), static_cast<unsigned long long>(glitches),
                      static_cast<unsigned long long>(slips), static_cast<unsigned long long>(levelChanges), levelMin,
                      levelMax, worstNoise, trip(tripMin).c_str(), trip(tripMax).c_str(), driftMin, driftMax, wander,
                      static_cast<long long>(r.engine.minOutputMarginFrames),
                      static_cast<long long>(r.engine.maxDmaInNs), static_cast<long long>(r.engine.maxDmaOutNs),
                      static_cast<long long>(r.engine.maxClientNs),
                      r.clockMeasured ? fixed(r.clockPpm, 2).c_str() : "", r.engine.clockPpm, r.clean() ? 1 : 0);
        csv += line;
    }
    return csv;
}

} // namespace

std::string loopCsv(const LoopRun& run)
{
    const LoopPath path = run.results.empty() ? LoopPath::AesWords : run.results.front().settings.path;
    if (path == LoopPath::Analog)
        return analogCsv(run);
    if (path == LoopPath::Aes)
        return aesToneCsv(run);
    std::string csv =
        "frames,guard,lead,seconds,blocks,late_outputs,dropped_frames,output_moves,overruns,stalls,dma_errors,"
        "dma_recovered,lost,repeated,silent,wrong,garbage,"
        "round_trip_min,round_trip_max,round_trip_changes,min_margin,max_dma_in_ns,max_dma_out_ns,max_client_ns,"
        "clock_ppm,clock_ppm_engine,clean\n";
    char line[768];
    for (const LoopResult& r : run.results)
    {
        std::int64_t tripMin = -1, tripMax = -1;
        std::uint64_t tripChanges = 0;
        for (const ChannelVerifier& c : r.channels)
        {
            if (!c.locked())
                continue;
            tripMin = tripMin < 0 ? c.minRoundTrip : std::min(tripMin, c.minRoundTrip);
            tripMax = std::max(tripMax, c.maxRoundTrip);
            tripChanges += c.roundTripChanges;
        }
        std::snprintf(line, sizeof(line),
                      "%u,%u,%u,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%lld,%llu,"
                      "%lld,%lld,%lld,%lld,%s,%.2f,%d\n",
                      r.settings.frames, r.settings.guard, r.leadFrames(), r.seconds,
                      static_cast<unsigned long long>(r.engine.blocks),
                      static_cast<unsigned long long>(r.engine.lateOutputs),
                      static_cast<unsigned long long>(r.engine.droppedOutputFrames),
                      static_cast<unsigned long long>(r.engine.outputMoves),
                      static_cast<unsigned long long>(r.engine.inputOverruns),
                      static_cast<unsigned long long>(r.engine.stalls),
                      static_cast<unsigned long long>(r.engine.dmaErrors),
                      static_cast<unsigned long long>(r.engine.dmaRecovered), static_cast<unsigned long long>(r.lost()),
                      static_cast<unsigned long long>(r.repeated()), static_cast<unsigned long long>(r.silent()),
                      static_cast<unsigned long long>(r.wrong()), static_cast<unsigned long long>(r.garbage()),
                      static_cast<long long>(tripMin), static_cast<long long>(tripMax),
                      static_cast<unsigned long long>(tripChanges),
                      static_cast<long long>(r.engine.minOutputMarginFrames),
                      static_cast<long long>(r.engine.maxDmaInNs), static_cast<long long>(r.engine.maxDmaOutNs),
                      static_cast<long long>(r.engine.maxClientNs),
                      r.clockMeasured ? fixed(r.clockPpm, 2).c_str() : "", r.engine.clockPpm, r.clean() ? 1 : 0);
        csv += line;
    }
    return csv;
}

} // namespace ajaduplex::probe
