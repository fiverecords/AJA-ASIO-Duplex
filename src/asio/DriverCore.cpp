// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/DriverCore.h"

#include "DuplexVersion.h"
#include "platform/Realtime.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ajaduplex::asio
{

namespace
{

/// The core whose host callback is running on this thread, if any: only
/// then does outputReady() mean "send this block now".
thread_local const DriverCore* tlsCallbackCore = nullptr;

void copyName(char* target, std::size_t size, const std::string& name)
{
    const std::size_t n = std::min(name.size(), size - 1);
    std::memcpy(target, name.data(), n);
    target[n] = '\0';
}

bool is48k(ASIOSampleRate rate)
{
    return std::fabs(rate - static_cast<double>(DriverCore::kSampleRate)) < 0.5;
}

/// The first few events of a kind in a run go to the log one by one, then
/// one in a thousand: a burst of late blocks must not flood it.
bool worthLogging(std::uint64_t count)
{
    return count <= 10 || count % 1000 == 0;
}

unsigned bitCount(unsigned bits)
{
    unsigned n = 0;
    for (; bits != 0; bits &= bits - 1)
        ++n;
    return n;
}

std::string oneLine(const DriverSettings& s)
{
    std::string text = describe(s);
    for (char& c : text)
    {
        if (c == '\n')
            c = ',';
    }
    while (!text.empty() && (text.back() == ',' || text.back() == ' '))
        text.pop_back();
    std::string spaced;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        spaced += text[i];
        if (text[i] == ',')
            spaced += ' ';
    }
    return spaced;
}

} // namespace

void DriverCore::logf(const char* format, ...)
{
    if (options_.log == nullptr)
        return;
    char buffer[LogRecord::kTextBytes];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    options_.log->line(buffer);
}

void DriverCore::logLines(const std::string& prefix, const std::string& text)
{
    if (options_.log == nullptr)
        return;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
            options_.log->line((prefix + text.substr(start, end - start)).c_str());
        start = end + 1;
    }
}

DriverCore::DriverCore(CardOpener& opener, Clock& clock, DriverOptions options)
    : opener_(opener), clock_(clock), options_(std::move(options))
{
}

DriverCore::~DriverCore()
{
    stop();
    if (thread_.joinable())
        thread_.join();
    releaseBuffers();
    routing_.reset();
    engine_.reset();
    releaseCard();
}

void DriverCore::releaseCard()
{
    if (!card_)
        return;
    // The card's settings go back now, and the log says what stayed.
    card_->restore();
    const std::string note = card_->restoreNote();
    if (!note.empty())
        logf("release: %s", note.c_str());
    card_.reset();
    // The task mode is out of the driver's hands: back as it was, or left
    // to a program that holds the card.
    if (taskModeHeld_)
    {
        taskModeHeld_ = false;
        if (options_.taskModeHeld)
            options_.taskModeHeld(false, taskModeHeldCard_);
    }
}

long DriverCore::driverVersion() const
{
    return (AJADUPLEX_VERSION_MAJOR << 16) | (AJADUPLEX_VERSION_MINOR << 8) | AJADUPLEX_VERSION_PATCH;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool DriverCore::init(const DriverSettings& settings)
{
    if (initialized())
    {
        // Called again: start from scratch with the settings given now.
        stop();
        if (thread_.joinable())
            thread_.join();
        releaseBuffers();
        routing_.reset();
        engine_.reset();
        releaseCard();
    }
    error_.clear();
    settings_ = settings;
    requested_ = settings;
    latenciesFromOpener_ = false;
    keepRouting_.store(settings.keepRouting, std::memory_order_relaxed);
    routingWatched_.store(false, std::memory_order_relaxed);
    logf("init: %s %s; %s", options_.name.c_str(), AJADUPLEX_VERSION_STRING, oneLine(settings_).c_str());
    std::string error;
    card_ = opener_.open(settings_, error);
    if (!card_)
    {
        error_ = error.empty() ? std::string("The AJA card could not be opened.") : error;
        logf("init failed: %s", error_.c_str());
        return false;
    }
    // Every input and output the driver offers is on the Xpand.
    bool breakout = false;
    if (card_->breakoutBoardPresent(breakout) && !breakout)
    {
        error_ = "The Xpand (the KONA X's breakout board) is not connected: the driver's inputs and outputs are "
                 "on it.";
        logf("init failed: %s", error_.c_str());
        card_.reset();
        return false;
    }
    logLines("card: ", card_->description());
    // An embedded input must be one the card has.
    const device::CardCapabilities caps = card_->capabilities();
    if ((settings_.input == InputChoice::Sdi && (settings_.sdiInput < 1 || settings_.sdiInput > caps.sdiInputs)) ||
        (settings_.input == InputChoice::Hdmi && caps.hdmiInputs == 0))
    {
        error_ = settings_.input == InputChoice::Hdmi
                     ? std::string("The card has no HDMI input to take audio from.")
                     : "The card has " + std::to_string(caps.sdiInputs) + " SDI inputs; the settings ask for SDI " +
                           std::to_string(settings_.sdiInput) + ".";
        logf("init failed: %s", error_.c_str());
        card_.reset();
        return false;
    }
    // So must the SDI and HDMI outputs it plays to.
    unsigned missingSdi = 0;
    for (unsigned n = caps.sdiOutputs; n < 8 && missingSdi == 0; ++n)
    {
        if ((settings_.sdiOutputs & (1u << n)) != 0)
            missingSdi = n + 1;
    }
    if (missingSdi != 0 || (settings_.hdmiOutput && caps.hdmiOutputs == 0))
    {
        error_ = missingSdi != 0 ? "The card has " + std::to_string(caps.sdiOutputs) +
                                       " SDI outputs; the settings ask for SDI out " + std::to_string(missingSdi) + "."
                                 : std::string("The card has no HDMI output to play to.");
        logf("init failed: %s", error_.c_str());
        card_.reset();
        return false;
    }
    sdiNote_ = checkSdiConnectors();
    if (!sdiNote_.empty())
        logf("sdi: %s", sdiNote_.c_str());
    std::uint32_t inputLatency = 0, outputLatency = 0;
    if (opener_.hardwareLatencies(settings_, inputLatency, outputLatency))
    {
        settings_.inputLatencyFrames = inputLatency;
        settings_.outputLatencyFrames = outputLatency;
        latenciesFromOpener_ = true;
    }
    engine_ = std::make_unique<engine::AudioEngine>(*card_, clock_);
    routing_ = std::make_unique<engine::RoutingWatch>(*card_);
    channels_ = ChannelMap::forSettings(settings_);
    // The reference is read now, while nothing runs on the card; hosts ask
    // for the clock sources before they start. With an embedded input the
    // audio clock is the video input's (the SDK sets it so).
    if (settings_.input == InputChoice::Sdi)
        clockName_ = "Video input: SDI " + std::to_string(settings_.sdiInput);
    else if (settings_.input == InputChoice::Hdmi)
        clockName_ = "Video input: HDMI";
    else
    {
        const std::string reference = card_->clockReference();
        clockName_ = reference.empty() ? opener_.clockSourceName() : "Card reference: " + reference;
    }
    logf("ready: %llu inputs, %llu outputs, clock %s", static_cast<unsigned long long>(channels_.inputs.size()),
         static_cast<unsigned long long>(channels_.outputs.size()), clockName_.c_str());
    return true;
}

std::string DriverCore::checkSdiConnectors() const
{
    std::string note;
    auto add = [&note](const std::string& text) { note += (note.empty() ? "" : " ") + text; };
    const bool sdiInput = settings_.input == InputChoice::Sdi;
    const unsigned input = settings_.sdiInput - 1;
    bool sends = false;
    for (unsigned n = 0; n < 8; ++n)
    {
        if ((settings_.sdiOutputs & (1u << n)) == 0 || !card_->sdiSends(n, sends))
            continue;
        const std::string name = "SDI " + std::to_string(n + 1);
        if (sdiInput && n == input)
            add(name + " cannot be the driver's input and one of its outputs: the connector is one or the other (it "
                       "is set as an " +
                (sends ? "output" : "input") + " now).");
        else if (!sends)
            add(name + " is set as an input, so the driver's audio does not go out on it: set it as an output in "
                       "the AJA Control Panel.");
    }
    if (sdiInput && (settings_.sdiOutputs & (1u << input)) == 0 && card_->sdiSends(input, sends) && sends)
        add("SDI " + std::to_string(input + 1) +
            " is set as an output, so the driver's input hears nothing: set it as an input in the AJA Control "
            "Panel.");
    return note;
}

DriverCore::SettingsApplied DriverCore::settingsChanged(const DriverSettings& updated)
{
    if (!initialized())
        return SettingsApplied::NextOpen;
    const SettingsChange change = compareSettings(requested_, updated);
    if (!change.any)
        return SettingsApplied::Nothing;
    if (change.needsReset)
    {
        // The buffer size offered first is only an offer, read when the host
        // asks for the sizes: from now on, as a host may ask the driver it
        // still has open before opening it again (Resolume does, and made
        // the buffers with the old size), or ask again without opening it
        // again. What runs keeps the size it was created with.
        if (updated.bufferFrames != requested_.bufferFrames)
        {
            requested_.bufferFrames = updated.bufferFrames;
            settings_.bufferFrames = updated.bufferFrames;
            logf("buffer size offered first: %u frames from now on", updated.bufferFrames);
        }
        // The source, the buffer or S: the host has to stop and open the
        // driver again, and init reads them then.
        if (prepared_ && host_.reset)
        {
            logf("settings changed (%s): asking the host to reset", change.what.c_str());
            resetsSent_.fetch_add(1, std::memory_order_relaxed);
            hostMessage(kAsioResetRequest, 0);
            return SettingsApplied::WhenTheHostResets;
        }
        logf("settings changed (%s): they apply when the host opens the driver again", change.what.c_str());
        return SettingsApplied::NextOpen;
    }
    // The latencies and Keep my routing: in use at once.
    if (change.latencies)
    {
        requested_.inputLatencyFrames = updated.inputLatencyFrames;
        requested_.outputLatencyFrames = updated.outputLatencyFrames;
        if (!latenciesFromOpener_)
        {
            settings_.inputLatencyFrames = updated.inputLatencyFrames;
            settings_.outputLatencyFrames = updated.outputLatencyFrames;
        }
        logf("latencies changed: the card's share is now in %u, out %u frames", settings_.inputLatencyFrames,
             settings_.outputLatencyFrames);
        if (prepared_ && host_.latenciesChanged)
            hostMessage(kAsioLatenciesChanged, 0);
    }
    if (change.keepRouting)
    {
        requested_.keepRouting = updated.keepRouting;
        settings_.keepRouting = updated.keepRouting;
        keepRouting_.store(updated.keepRouting, std::memory_order_relaxed);
        logf("Keep my routing is now %s", updated.keepRouting ? "on: what another program changes is put back"
                                                               : "off: changes are only reported");
    }
    if (change.analogLevel)
    {
        // The card is the engine's while it runs: its thread sets the level
        // after the next block. Stopped, the next start does.
        requested_.analogLevelDbu = updated.analogLevelDbu;
        settings_.analogLevelDbu = updated.analogLevelDbu;
        if (running())
            pendingLevel_.store(static_cast<int>(updated.analogLevelDbu), std::memory_order_release);
        else if (updated.analogLevelDbu == 0)
            logf("analog level: left as the card has it from the next start");
        else
            logf("analog level: %+d dBu from the next start", static_cast<int>(updated.analogLevelDbu));
    }
    return SettingsApplied::Now;
}

// ---------------------------------------------------------------------------
// Channels, sizes, rates, clocks
// ---------------------------------------------------------------------------

ASIOError DriverCore::getChannels(long* numInputs, long* numOutputs) const
{
    if (!initialized())
        return ASE_NotPresent;
    if (numInputs == nullptr || numOutputs == nullptr)
        return ASE_InvalidParameter;
    *numInputs = static_cast<long>(channels_.inputs.size());
    *numOutputs = static_cast<long>(channels_.outputs.size());
    return ASE_OK;
}

unsigned DriverCore::leadFrames(unsigned blockFrames) const
{
    return settings_.aggressive ? blockFrames / 2 + settings_.guardFrames : blockFrames + settings_.guardFrames;
}

unsigned DriverCore::activeBlockFrames() const
{
    return prepared_ ? blockFrames_ : settings_.bufferFrames;
}

ASIOError DriverCore::getLatencies(long* inputLatency, long* outputLatency) const
{
    if (!initialized())
        return ASE_NotPresent;
    if (inputLatency == nullptr || outputLatency == nullptr)
        return ASE_InvalidParameter;
    // Input: a block takes N frames to fill. Output: a block sounds S frames
    // after it was found complete (the engine places it so, and keeps it so
    // after a late block, D1). Plus what the card and converters add.
    const unsigned n = activeBlockFrames();
    *inputLatency = static_cast<long>(n + settings_.inputLatencyFrames);
    *outputLatency = static_cast<long>(leadFrames(n) + settings_.outputLatencyFrames);
    return ASE_OK;
}

ASIOError DriverCore::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) const
{
    if (!initialized())
        return ASE_NotPresent;
    if (minSize == nullptr || maxSize == nullptr || preferredSize == nullptr || granularity == nullptr)
        return ASE_InvalidParameter;
    *minSize = static_cast<long>(DriverSettings::kMinBufferFrames);
    *maxSize = static_cast<long>(DriverSettings::kMaxBufferFrames);
    *preferredSize = static_cast<long>(settings_.bufferFrames);
    *granularity = -1;   // powers of two
    return ASE_OK;
}

ASIOError DriverCore::canSampleRate(ASIOSampleRate rate) const
{
    if (!initialized())
        return ASE_NotPresent;
    return is48k(rate) ? ASE_OK : ASE_NoClock;
}

ASIOError DriverCore::getSampleRate(ASIOSampleRate* rate) const
{
    if (!initialized())
        return ASE_NotPresent;
    if (rate == nullptr)
        return ASE_InvalidParameter;
    *rate = static_cast<ASIOSampleRate>(kSampleRate);
    return ASE_OK;
}

ASIOError DriverCore::setSampleRate(ASIOSampleRate rate)
{
    if (!initialized())
        return ASE_NotPresent;
    return is48k(rate) ? ASE_OK : ASE_NoClock;
}

ASIOError DriverCore::getClockSources(ASIOClockSource* clocks, long* numSources)
{
    if (!initialized())
        return ASE_NotPresent;
    if (clocks == nullptr || numSources == nullptr || *numSources < 1)
        return ASE_InvalidParameter;
    // One source: the card's reference, which the driver shows but never
    // changes, since video depends on it.
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    copyName(clocks[0].name, sizeof(clocks[0].name), clockName_);
    *numSources = 1;
    return ASE_OK;
}

ASIOError DriverCore::setClockSource(long reference)
{
    if (!initialized())
        return ASE_NotPresent;
    return reference == 0 ? ASE_OK : ASE_InvalidParameter;
}

ASIOError DriverCore::getChannelInfo(ASIOChannelInfo* info) const
{
    if (!initialized())
        return ASE_NotPresent;
    if (info == nullptr)
        return ASE_InvalidParameter;
    const std::vector<ChannelEntry>& list = info->isInput ? channels_.inputs : channels_.outputs;
    if (info->channel < 0 || static_cast<std::size_t>(info->channel) >= list.size())
        return ASE_InvalidParameter;
    const std::size_t c = static_cast<std::size_t>(info->channel);
    const std::vector<bool>& active = info->isInput ? inputActive_ : outputActive_;
    info->isActive = prepared_ && c < active.size() && active[c] ? ASIOTrue : ASIOFalse;
    info->channelGroup = 0;
    info->type = ASIOSTInt32LSB;
    copyName(info->name, sizeof(info->name), list[c].name);
    return ASE_OK;
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

ASIOError DriverCore::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks* callbacks)
{
    if (!initialized())
        return ASE_NotPresent;
    if (running())
        return ASE_InvalidMode;
    if (prepared_)
        releaseBuffers();
    if (bufferInfos == nullptr || numChannels <= 0 || callbacks == nullptr || callbacks->bufferSwitch == nullptr)
        return ASE_InvalidParameter;
    if (bufferSize <= 0 || !isValidBufferSize(static_cast<std::uint32_t>(bufferSize)))
    {
        error_ = "Buffer size " + std::to_string(bufferSize) + " is not offered: a power of two from " +
                 std::to_string(DriverSettings::kMinBufferFrames) + " to " +
                 std::to_string(DriverSettings::kMaxBufferFrames) + ".";
        logf("createBuffers refused: %s", error_.c_str());
        return ASE_InvalidMode;
    }

    std::vector<bool> inputs(channels_.inputs.size(), false);
    std::vector<bool> outputs(channels_.outputs.size(), false);
    for (long i = 0; i < numChannels; ++i)
    {
        const ASIOBufferInfo& b = bufferInfos[i];
        std::vector<bool>& used = b.isInput ? inputs : outputs;
        if (b.channelNum < 0 || static_cast<std::size_t>(b.channelNum) >= used.size() ||
            used[static_cast<std::size_t>(b.channelNum)])
        {
            error_ = std::string("createBuffers asked for ") + (b.isInput ? "input " : "output ") +
                     std::to_string(b.channelNum) + ", which does not exist or was asked for twice.";
            logf("createBuffers refused: %s", error_.c_str());
            return ASE_InvalidMode;
        }
        used[static_cast<std::size_t>(b.channelNum)] = true;
    }

    const unsigned n = static_cast<unsigned>(bufferSize);
    for (long i = 0; i < numChannels; ++i)
    {
        ASIOBufferInfo& b = bufferInfos[i];
        auto buffer = std::make_unique<AlignedBuffer>(2 * std::size_t(n) * sizeof(std::int32_t), 64);
        std::int32_t* halves = buffer->as<std::int32_t>();
        b.buffers[0] = halves;
        b.buffers[1] = halves + n;
        const std::size_t c = static_cast<std::size_t>(b.channelNum);
        if (b.isInput)
        {
            const unsigned cardChannel = channels_.inputs[c].cardChannel;
            inputPointers_[0][cardChannel] = halves;
            inputPointers_[1][cardChannel] = halves + n;
        }
        else
        {
            const unsigned cardChannel = channels_.outputs[c].cardChannel;
            outputPointers_[0][cardChannel] = halves;
            outputPointers_[1][cardChannel] = halves + n;
        }
        hostBuffers_.push_back(std::move(buffer));
    }
    inputActive_ = std::move(inputs);
    outputActive_ = std::move(outputs);
    callbacks_ = *callbacks;
    blockFrames_ = n;
    prepared_ = true;

    // What the host takes. kAsioSupportsTimeInfo is asked directly, as the
    // SDK's own example does: some hosts answer it but not the
    // kAsioSelectorSupported question about it.
    host_ = HostSupport{};
    host_.timeInfo = callbacks_.bufferSwitchTimeInfo != nullptr && hostMessage(kAsioSupportsTimeInfo, 0) == 1;
    host_.overload = hostSupports(kAsioOverload);
    host_.resync = hostSupports(kAsioResyncRequest);
    host_.reset = hostSupports(kAsioResetRequest);
    host_.latenciesChanged = hostSupports(kAsioLatenciesChanged);
    std::size_t activeInputs = 0;
    for (const bool on : inputActive_)
        activeInputs += on ? 1 : 0;
    logf("createBuffers: N = %u, %llu inputs and %llu outputs active; the host takes%s%s%s%s%s", n,
         static_cast<unsigned long long>(activeInputs),
         static_cast<unsigned long long>(static_cast<std::size_t>(numChannels) - activeInputs),
         host_.timeInfo ? " time info," : "",
         host_.overload ? " overload," : "", host_.resync ? " resync," : "", host_.reset ? " reset," : "",
         host_.latenciesChanged ? " latency changes" : "");
    return ASE_OK;
}

void DriverCore::releaseBuffers()
{
    hostBuffers_.clear();
    inputActive_.clear();
    outputActive_.clear();
    for (auto& half : inputPointers_)
        half.fill(nullptr);
    for (auto& half : outputPointers_)
        half.fill(nullptr);
    callbacks_ = ASIOCallbacks{};
    host_ = HostSupport{};
    blockFrames_ = 0;
    prepared_ = false;
}

ASIOError DriverCore::disposeBuffers()
{
    if (!initialized())
        return ASE_NotPresent;
    if (!prepared_)
        return ASE_InvalidMode;
    stop();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
        thread_.join();
    releaseBuffers();
    return ASE_OK;
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

std::int64_t DriverCore::systemTimeNow() const
{
    return options_.systemTimeNs ? options_.systemTimeNs() : clock_.nowNs();
}

ASIOError DriverCore::start()
{
    if (!initialized())
        return ASE_NotPresent;
    if (!prepared_)
        return ASE_InvalidMode;
    if (running())
        return ASE_OK;
    if (thread_.joinable())
        thread_.join();   // a run stopped from inside a callback

    // Silence in every buffer until the host writes its own.
    for (const auto& buffer : hostBuffers_)
        std::memset(buffer->data(), 0, buffer->size());

    engine::EngineConfig config;
    config.blockFrames = blockFrames_;
    config.guardFrames = settings_.guardFrames;
    config.aggressive = settings_.aggressive;
    config.audio.channels = kCardChannels;
    switch (settings_.input)
    {
        case InputChoice::Analog: config.audio.source = device::InputSource::Analog; break;
        case InputChoice::Aes:    config.audio.source = device::InputSource::Aes; break;
        case InputChoice::Hdmi:   config.audio.source = device::InputSource::Hdmi; break;
        case InputChoice::Sdi:
            config.audio.source = device::InputSource::Embedded;
            config.audio.embeddedInput = settings_.sdiInput - 1;
            break;
    }
    // The driver records the Xpand's inputs: the select follows them. With
    // an embedded input they are not the driver's, and the select goes back
    // to what it was right after, for whoever uses them.
    config.audio.keepXpandSelect = !isXpandInput(settings_.input);
    config.audio.routeAesOutputs = true;    // AES out 1-8 from this audio system
    config.audio.routeMonitor = true;       // and the monitor from channels 1-2, for the analog output (Q4)
    config.audio.monitorPair = 0;
    config.audio.analogLevelDbu = static_cast<int>(settings_.analogLevelDbu);   // 0: as the card has it
    config.audio.taskMode = settings_.oemTasks ? device::TaskMode::Oem : device::TaskMode::Unknown;
    config.audio.sdiOutputs = settings_.sdiOutputs;   // none by default: their video may be another program's
    config.audio.hdmiOutput = settings_.hdmiOutput;
    // The connectors' directions, as they are now: said again if they changed.
    const std::string sdiNote = checkSdiConnectors();
    if (sdiNote != sdiNote_)
    {
        logf("sdi: %s", sdiNote.empty() ? "the connectors are set as the settings need now" : sdiNote.c_str());
        sdiNote_ = sdiNote;
    }
    device::CardRouting before;
    const bool haveBefore = settings_.oemTasks && card_->readRouting(before) && before.haveTaskMode;

    std::string error;
    if (!engine_->start(config, *this, error))
    {
        error_ = "The card did not start: " + error;
        logf("start failed: %s", error_.c_str());
        return ASE_HWMalfunction;
    }
    // With OEM task mode on, the card's task mode is the driver's to put
    // back from here until it lets the card go, whatever the settings say
    // later: other programs are told (DriverOptions::taskModeHeld).
    if (settings_.oemTasks && !taskModeHeld_)
    {
        taskModeHeld_ = true;
        taskModeHeldCard_ = settings_.card;
        if (options_.taskModeHeld)
            options_.taskModeHeld(true, taskModeHeldCard_);
    }
    systemTimeOffsetNs_ = systemTimeNow() - clock_.nowNs();
    startedNs_ = clock_.nowNs();
    for (std::uint64_t& count : eventCounts_)
        count = 0;
    long inputLatency = 0, outputLatency = 0;
    getLatencies(&inputLatency, &outputLatency);
    logf("start: N = %u, G = %u, S = %u%s; latencies reported: in %ld, out %ld frames (the card's and its "
         "converters' share: %u in, %u out)",
         blockFrames_, settings_.guardFrames, leadFrames(blockFrames_), settings_.aggressive ? " (aggressive)" : "",
         inputLatency, outputLatency, settings_.inputLatencyFrames, settings_.outputLatencyFrames);
    // Said only when one failed: start checks them every time.
    const std::string bufferCheck = engine::describeBufferCheck(engine_->bufferCheck());
    if (!bufferCheck.empty())
        logf("buffers: %s", bufferCheck.c_str());
    if (settings_.sdiOutputs != 0 || settings_.hdmiOutput)
        logf("outputs: %s; the audio goes into whatever video they carry, which the driver never sets",
             describeOutputs(settings_).c_str());

    // The routing as configured, read back: ours from now on.
    routingLines_ = 0;
    routingChanges_.store(0, std::memory_order_relaxed);
    routingPutBack_.store(0, std::memory_order_relaxed);
    routingNotOurs_.store(0, std::memory_order_relaxed);
    pendingLevel_.store(-1, std::memory_order_relaxed);   // anything chosen before is in settings_ now
    // The analog level is the driver's only when the settings name one, and
    // the Xpand's select only when the driver records an Xpand input.
    // The task mode only with OEM mode on, and then reasserted always, the
    // rest of the driver's routing with it (RoutingWatch::check).
    const unsigned taskMode = settings_.oemTasks ? device::routing::kTaskMode : 0u;
    const unsigned owned = (device::routing::kRouting | taskMode) &
                           ~(settings_.analogLevelDbu == 0 ? device::routing::kAnalogLevel : 0u) &
                           ~(isXpandInput(settings_.input) ? 0u : device::routing::kXpandSelect);
    // Standard mode before this start: the driver has just taken the card
    // from AJA's services, which may have been in the middle of a pass and
    // write the outputs once more after it did. The first looks put that
    // right, Keep my routing or not.
    const bool tookFromServices = haveBefore && before.taskMode == device::TaskMode::Standard;
    const bool watched = routing_ && routing_->takeBaseline(owned, taskMode, tookFromServices);
    if (settings_.oemTasks)
        logf("task mode: OEM while the driver plays (it was %s); back as it was afterwards unless another program "
             "holds the card then",
             haveBefore ? device::toString(before.taskMode) : "unknown");
    routingWatched_.store(watched, std::memory_order_relaxed);
    lastRoutingCheckNs_ = clock_.nowNs();
    if (watched)
    {
        const device::CardRouting& ours = routing_->baseline();
        if (ours.haveTaskMode)
            taskMode_.store(static_cast<int>(ours.taskMode), std::memory_order_relaxed);
        analogLevel_.store(ours.haveAnalogLevel ? ours.analogLevelDbu : 0, std::memory_order_relaxed);
        logf("routing: %s; read back once a second, Keep my routing %s", engine::describeRouting(ours).c_str(),
             keepRouting_.load(std::memory_order_relaxed) ? "on" : "off");
    }
    else
    {
        logf("routing: the card cannot read it back, so it is not watched");
    }
    stopRequested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    if (options_.threaded)
        thread_ = std::thread([this] { engineThread(); });
    return ASE_OK;
}

void DriverCore::engineThread()
{
    std::unique_ptr<platform::ProAudioScope> priority;
    if (options_.proAudioPriority)
    {
        priority = std::make_unique<platform::ProAudioScope>();
        logf("thread: %s", priority->description().c_str());   // before the first block
    }
    while (engine_->runOnce())
    {
    }
    engine_->stop();
    const bool gaveUp = !stopRequested_.load(std::memory_order_relaxed);
    logRunSummary();
    running_.store(false, std::memory_order_release);
    if (gaveUp)
    {
        // Too many card errors in a row: the host should stop and start again.
        logf("the engine stopped on its own: %llu card register errors; asking the host to reset",
             static_cast<unsigned long long>(engine_->stats().registerErrors));
        resetsSent_.fetch_add(1, std::memory_order_relaxed);
        if (host_.reset)
            hostMessage(kAsioResetRequest, 0);
    }
}

void DriverCore::logRunSummary()
{
    const engine::EngineStats s = engine_->stats();
    const double seconds = static_cast<double>(clock_.nowNs() - startedNs_) / 1e9;
    logf("stop: %llu blocks in %.1f s; %llu late outputs (%llu frames dropped), %llu overruns, %llu stalls, %llu "
         "output moves, %llu DMA errors, %llu recovered",
         static_cast<unsigned long long>(s.blocks), seconds, static_cast<unsigned long long>(s.lateOutputs),
         static_cast<unsigned long long>(s.droppedOutputFrames), static_cast<unsigned long long>(s.inputOverruns),
         static_cast<unsigned long long>(s.stalls), static_cast<unsigned long long>(s.outputMoves),
         static_cast<unsigned long long>(s.dmaErrors), static_cast<unsigned long long>(s.dmaRecovered));
    for (const engine::RetiredBuffer& b : engine_->retiredBuffers())
    {
        // After the run: a bad buffer fails again, a passing fault does not.
        char verdict[160];
        if (!b.checked)
            std::snprintf(verdict, sizeof(verdict), "not tried again");
        else if (b.failsAgain)
            std::snprintf(verdict, sizeof(verdict),
                          "the same DMA from it failed again at stop (system error %u), with the card stopped: "
                          "the buffer is bad",
                          static_cast<unsigned>(b.againSystemError));
        else
            std::snprintf(verdict, sizeof(verdict),
                          "the same DMA from it went through at stop, with the card stopped: a passing fault, or "
                          "one that needs the card running");
        logf("stop: a buffer retired after a failed DMA (%s, %u bytes at ring offset %u, system error %u): %s",
             b.output ? "output" : "input", static_cast<unsigned>(b.bytes), static_cast<unsigned>(b.ringOffset),
             static_cast<unsigned>(b.systemError), verdict);
    }
    logf("stop: smallest output margin %lld frames; longest DMA in %lld us, out %lld us, host callback %lld us; "
         "card clock %+.1f ppm; the output within %+lld to %+lld frames of the input's timing",
         static_cast<long long>(s.minOutputMarginFrames), static_cast<long long>(s.maxDmaInNs / 1000),
         static_cast<long long>(s.maxDmaOutNs / 1000), static_cast<long long>(s.maxClientNs / 1000), s.clockPpm,
         static_cast<long long>(s.minOutputStepFrames), static_cast<long long>(s.maxOutputStepFrames));
    const RoutingStatus r = routingStatus();
    if (r.watched && (r.changes > 0 || r.putBack > 0 || r.notOurs != 0))
    {
        // After the run: the engine's thread may allocate again.
        const std::string names = engine::routingPartNames(r.notOurs);
        logf("stop: routing: changes by other programs %llu, parts put back %llu%s%s",
             static_cast<unsigned long long>(r.changes), static_cast<unsigned long long>(r.putBack),
             names.empty() ? "" : "; not the driver's at the end: ", names.c_str());
    }
}

ASIOError DriverCore::stop()
{
    if (!initialized())
        return ASE_NotPresent;
    if (!running() && !thread_.joinable())
        return ASE_OK;
    stopRequested_.store(true, std::memory_order_relaxed);
    engine_->requestStop();
    if (options_.threaded)
    {
        // From inside a callback the engine's thread cannot wait for itself:
        // it ends after the callback returns, and is joined later.
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
            thread_.join();
    }
    else if (running())
    {
        engine_->stop();
        logRunSummary();
        running_.store(false, std::memory_order_release);
    }
    return ASE_OK;
}

bool DriverCore::runBlocks(unsigned count)
{
    if (options_.threaded || !running())
        return false;
    for (unsigned i = 0; i < count; ++i)
    {
        if (!engine_->runOnce())
        {
            if (!stopRequested_.load(std::memory_order_relaxed))
            {
                engine_->stop();
                logRunSummary();
                running_.store(false, std::memory_order_release);
                resetsSent_.fetch_add(1, std::memory_order_relaxed);
                if (host_.reset)
                    hostMessage(kAsioResetRequest, 0);
            }
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// While running
// ---------------------------------------------------------------------------

ASIOError DriverCore::getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timeStamp) const
{
    if (!initialized())
        return ASE_NotPresent;
    if (position == nullptr || timeStamp == nullptr)
        return ASE_InvalidParameter;
    std::int64_t samples = 0, hostNs = 0;
    if (!running() || !engine_->lastBlockPosition(samples, hostNs))
        return ASE_SPNotAdvancing;
    setHiLo(*position, samples);
    setHiLo(*timeStamp, hostNs + systemTimeOffsetNs_);
    return ASE_OK;
}

ASIOError DriverCore::outputReady()
{
    // The block goes to the card at once; called from anywhere but inside a
    // callback it has nothing to send, and the block goes when the callback
    // returns, as it would anyway.
    if (tlsCallbackCore == this)
        engine_->outputReady();
    return ASE_OK;
}

ASIOError DriverCore::future(long selector, void* opt)
{
    (void)opt;
    switch (selector)
    {
        case kAsioCanTimeInfo:
        case kAsioCanReportOverload:
            return ASE_SUCCESS;
        default:
            return ASE_InvalidParameter;
    }
}

engine::BlockBuffers DriverCore::buffers(unsigned half)
{
    return engine::BlockBuffers{inputPointers_[half & 1u].data(), outputPointers_[half & 1u].data()};
}

void DriverCore::process(const engine::BlockInfo& info, unsigned half)
{
    tlsCallbackCore = this;
    const long index = static_cast<long>(half & 1u);
    if (host_.timeInfo)
    {
        AsioTimeInfo& t = time_.timeInfo;
        t.speed = 1.0;
        setHiLo(t.systemTime, info.hostTimeNs + systemTimeOffsetNs_);
        setHiLo(t.samplePosition, info.samplePosition);
        t.sampleRate = static_cast<ASIOSampleRate>(kSampleRate);
        t.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        time_.timeCode.flags = 0;
        callbacks_.bufferSwitchTimeInfo(&time_, index, ASIOTrue);
    }
    else
    {
        callbacks_.bufferSwitch(index, ASIOTrue);
    }
    tlsCallbackCore = nullptr;
}

void DriverCore::logEvent(engine::EngineEvent event, const engine::BlockInfo& info)
{
    const std::size_t kind = static_cast<std::size_t>(event);
    if (kind >= sizeof(eventCounts_) / sizeof(eventCounts_[0]))
        return;
    const std::uint64_t count = ++eventCounts_[kind];
    if (!worthLogging(count))
        return;
    const double at = static_cast<double>(info.samplePosition) / static_cast<double>(kSampleRate);
    if (event == engine::EngineEvent::LateOutput)
    {
        // Where the time went: found late is the engine waking (or the PC
        // waking it), the host is the host's callback.
        const engine::LateOutputDetail& d = engine_->lastLateOutput();
        const double frameMs = 1000.0 / static_cast<double>(kSampleRate);
        logf("late output at %.3f s (%llu so far, %llu frames dropped): %.2f ms late; a block has %.2f ms from "
             "complete to written, this one was found after %.2f ms, its DMA in took %.2f ms and the host %.2f ms",
             at, static_cast<unsigned long long>(count),
             static_cast<unsigned long long>(engine_->stats().droppedOutputFrames),
             static_cast<double>(d.lateFrames) * frameMs, static_cast<double>(d.leadFrames) * frameMs,
             static_cast<double>(d.foundLateFrames) * frameMs, static_cast<double>(d.dmaInNs) / 1e6,
             static_cast<double>(d.hostNs) / 1e6);
    }
    else if (event == engine::EngineEvent::DmaError || event == engine::EngineEvent::DmaRecovered)
    {
        // Which DMA, what the system said of it, and of a second try.
        char detail[400];
        engine::describeDmaError(engine_->lastDmaError(), detail, sizeof(detail));
        logf("%s at %.3f s (%llu so far): %s",
             event == engine::EngineEvent::DmaError ? "DMA error" : "DMA error, recovered", at,
             static_cast<unsigned long long>(count), detail);
    }
    else
        logf("%s at %.3f s (%llu so far)", engine::toString(event), at, static_cast<unsigned long long>(count));
}

void DriverCore::onEngineEvent(engine::EngineEvent event, const engine::BlockInfo& info)
{
    logEvent(event, info);
    switch (event)
    {
        case engine::EngineEvent::LateOutput:
        case engine::EngineEvent::DmaError:
            sendOverload();
            break;
        case engine::EngineEvent::InputOverrun:
            sendOverload();
            sendResync();
            break;
        case engine::EngineEvent::InputResumed:
        case engine::EngineEvent::OutputMoved:
            sendResync();
            break;
        case engine::EngineEvent::InputStalled:
            break;   // silence follows at the PC's pace; the resync comes when it resumes
        case engine::EngineEvent::DmaRecovered:
            break;   // nothing was lost
    }
}

void DriverCore::afterBlock(const engine::BlockInfo& info)
{
    if (pendingLevel_.load(std::memory_order_relaxed) >= 0)
    {
        const int level = pendingLevel_.exchange(-1, std::memory_order_acq_rel);
        if (level >= 0)
            applyAnalogLevel(level);
    }
    if (!routing_ || !routing_->active() || info.hostTimeNs - lastRoutingCheckNs_ < kRoutingCheckNs)
        return;
    lastRoutingCheckNs_ = info.hostTimeNs;
    watchRouting();
}

void DriverCore::applyAnalogLevel(int dbu)
{
    // On the engine's thread, the card's only user while it runs.
    if (!routing_ || !routing_->active())
    {
        logf("analog level: the card's routing cannot be read here, so it applies at the next start");
        return;
    }
    if (dbu == 0)
    {
        routing_->release(device::routing::kAnalogLevel);
        logf("analog level: left as the card has it from now on");
        return;
    }
    device::CardRouting wanted = routing_->baseline();
    wanted.haveAnalogLevel = true;
    wanted.analogLevelDbu = dbu;
    wanted.analogLevelCode = 0;
    if (routing_->change(wanted, device::routing::kAnalogLevel))
        logf("analog level set to %+d dBu", dbu);
    else
        logf("analog level: the card did not take %+d dBu", dbu);
    const device::CardRouting& now = routing_->baseline();
    analogLevel_.store(now.haveAnalogLevel ? now.analogLevelDbu : 0, std::memory_order_relaxed);
}

bool DriverCore::routingLineDue()
{
    ++routingLines_;
    return routingLines_ <= 20 || routingLines_ % 100 == 0;
}

void DriverCore::watchRouting()
{
    // On the engine's thread: a few register reads, text on the stack.
    const engine::RoutingWatch::Check c = routing_->check(keepRouting_.load(std::memory_order_relaxed));
    if (!c.read)
        return;
    const unsigned changes = engine::reportCheck(
        *routing_, c, "Keep my routing",
        [](void* context, const char* line) {
            auto* self = static_cast<DriverCore*>(context);
            if (self->options_.log != nullptr && self->routingLineDue())
                self->options_.log->line(line);
        },
        this);
    const unsigned putBack = c.putBack & ~c.putBackFailed;
    routingChanges_.fetch_add(changes, std::memory_order_relaxed);
    routingPutBack_.fetch_add(bitCount(putBack), std::memory_order_relaxed);
    routingNotOurs_.store(c.notOurs & ~putBack, std::memory_order_relaxed);
    if (c.now.haveTaskMode)
        taskMode_.store(static_cast<int>(c.now.taskMode), std::memory_order_relaxed);
    const device::CardRouting& last = routing_->last();
    analogLevel_.store(last.haveAnalogLevel ? last.analogLevelDbu : 0, std::memory_order_relaxed);
}

void DriverCore::sendOverload()
{
    overloadsSent_.fetch_add(1, std::memory_order_relaxed);
    if (host_.overload)
        hostMessage(kAsioOverload, 0);
}

void DriverCore::sendResync()
{
    resyncsSent_.fetch_add(1, std::memory_order_relaxed);
    if (host_.resync)
        hostMessage(kAsioResyncRequest, 0);
}

long DriverCore::hostMessage(long selector, long value)
{
    if (callbacks_.asioMessage == nullptr)
        return 0;
    return callbacks_.asioMessage(selector, value, nullptr, nullptr);
}

bool DriverCore::hostSupports(long selector)
{
    return hostMessage(kAsioSelectorSupported, selector) == 1;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

engine::EngineStats DriverCore::stats() const
{
    return engine_ ? engine_->stats() : engine::EngineStats{};
}

DriverCore::RoutingStatus DriverCore::routingStatus() const
{
    RoutingStatus r;
    r.watched = routingWatched_.load(std::memory_order_relaxed);
    r.keep = keepRouting_.load(std::memory_order_relaxed);
    r.changes = routingChanges_.load(std::memory_order_relaxed);
    r.putBack = routingPutBack_.load(std::memory_order_relaxed);
    r.notOurs = routingNotOurs_.load(std::memory_order_relaxed);
    r.taskMode = static_cast<device::TaskMode>(taskMode_.load(std::memory_order_relaxed));
    r.analogLevelDbu = analogLevel_.load(std::memory_order_relaxed);
    return r;
}

DriverCore::MessageCounts DriverCore::messagesSent() const
{
    MessageCounts counts;
    counts.overload = overloadsSent_.load(std::memory_order_relaxed);
    counts.resync = resyncsSent_.load(std::memory_order_relaxed);
    counts.reset = resetsSent_.load(std::memory_order_relaxed);
    return counts;
}

} // namespace ajaduplex::asio
