// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    DriverCore.h
 * @brief   Everything IASIO does, without COM.
 *
 * The COM class (Windows only, src/asio/win) forwards each IASIO call here.
 * This class keeps the ASIO state (loaded, initialized, buffers prepared,
 * running), the host's double buffers and callbacks, and runs AudioEngine
 * on a thread of its own at MMCSS "Pro Audio" priority, calling the host's
 * bufferSwitch or bufferSwitchTimeInfo once per block.
 *
 * None of it needs Windows: the unit tests drive it on the simulated card
 * with a virtual clock, block by block (DriverOptions::threaded = false),
 * the way a host would, callbacks included.
 *
 * The routing it set up (the input source, the Xpand's select, the AES
 * outputs, the monitor, and the Xpand's analog level when the settings
 * name one) is read back at start and once a second while it runs, from
 * the engine's thread after a block's output (the design's coexistence
 * rules, engine/RoutingWatch.h): a change another program made goes to the
 * log and the panel, and with Keep my routing, ours is put back. The level
 * when it is left as the card has it, and the card's task mode with OEM
 * task mode off, are reported the same way, never changed. With OEM task
 * mode on (the default) the driver holds the card in it: a change is put
 * back at the next look, and with it everything else of the driver's that
 * is not as it set it, since AJA's services had the card meanwhile. The
 * first looks after a start that took the card out of standard mode do
 * the same, for what the services wrote from a pass under way then.
 *
 * What the host is told (asioMessage), when it supports the message:
 *   late output      -> kAsioOverload (samples lost, latency unchanged: D1)
 *   input overrun    -> kAsioOverload and kAsioResyncRequest
 *   input resumed    -> kAsioResyncRequest (both sides placed anew)
 *   output moved     -> kAsioResyncRequest
 *   DMA error        -> kAsioOverload
 *   engine gave up   -> kAsioResetRequest (too many card errors in a row)
 *
 * Threads: the host's calls come from its own threads, the callbacks and
 * the messages from the engine's. getSamplePosition and the statistics may
 * be read from any thread; outputReady only counts from inside a callback.
 */

#pragma once

#include "asio/AsioSdk.h"
#include "asio/ChannelMap.h"
#include "asio/SettingsChange.h"
#include "common/AlignedBuffer.h"
#include "common/Clock.h"
#include "common/DriverSettings.h"
#include "common/Log.h"
#include "device/CardAudio.h"
#include "engine/AudioEngine.h"
#include "engine/RoutingWatch.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ajaduplex::asio
{

/// Opens the card the driver runs on: the real one, or the simulated one.
class CardOpener
{
public:
    virtual ~CardOpener() = default;

    /// The card and audio system `settings` name, opened and checked; null
    /// with a reason the user can act on if not.
    virtual std::unique_ptr<device::CardAudio> open(const DriverSettings& settings, std::string& error) = 0;

    /// The one clock source shown to hosts.
    virtual std::string clockSourceName() { return "Card reference"; }

    /// The card's and converters' share of the latency, when the opener
    /// knows it better than the settings (the simulated card does).
    virtual bool hardwareLatencies(const DriverSettings& settings, std::uint32_t& inputFrames,
                                   std::uint32_t& outputFrames)
    {
        (void)settings;
        (void)inputFrames;
        (void)outputFrames;
        return false;
    }
};

struct DriverOptions
{
    std::string name = "AJA Duplex ASIO";

    /// false: no engine thread; the caller runs blocks with runBlocks() (tests).
    bool threaded = true;

    /// MMCSS "Pro Audio" for the engine thread (Windows).
    bool proAudioPriority = true;

    /// "Now" in the ASIO system time base, in ns. On Windows that is
    /// timeGetTime's base, as the ASIO specification requires. Empty: the
    /// clock's own time.
    std::function<std::int64_t()> systemTimeNs;

    /// Where the driver says what it does: init, buffers, start, the
    /// engine's events (from its thread, which never waits on the log),
    /// stop with the run's numbers. May be null.
    LogTarget* log = nullptr;

    /// Told when the driver takes the card's task mode in hand and when it
    /// lets it go: `held` from the first start with OEM task mode on until
    /// the card is let go (when the mode goes back, or is left to a
    /// program that holds the card), with the card's index in the SDK's
    /// scan. Both from the host's threads, never the engine's. On Windows
    /// the driver keeps a named mutex meanwhile (win/InstanceLock.h), which
    /// is how another program on the card knows to leave the task mode to
    /// the driver (AJA Input does). May be empty.
    std::function<void(bool held, unsigned card)> taskModeHeld;
};

class DriverCore final : private engine::EngineClient
{
public:
    static constexpr unsigned kCardChannels = 16;   ///< channels per frame in the card's rings
    static constexpr long kSampleRate = 48000;
    static constexpr std::int64_t kRoutingCheckNs = 1000000000;   ///< how often the routing is read back

    DriverCore(CardOpener& opener, Clock& clock, DriverOptions options = DriverOptions{});
    ~DriverCore() override;
    DriverCore(const DriverCore&) = delete;
    DriverCore& operator=(const DriverCore&) = delete;

    // -- IASIO, one call each --------------------------------------------

    /// Opens the card. False with errorMessage() set if it cannot.
    bool init(const DriverSettings& settings);
    const std::string& driverName() const { return options_.name; }
    long driverVersion() const;
    const std::string& errorMessage() const { return error_; }

    ASIOError start();
    ASIOError stop();
    ASIOError getChannels(long* numInputs, long* numOutputs) const;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) const;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) const;
    ASIOError canSampleRate(ASIOSampleRate rate) const;
    ASIOError getSampleRate(ASIOSampleRate* rate) const;
    ASIOError setSampleRate(ASIOSampleRate rate);
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources);
    ASIOError setClockSource(long reference);
    ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timeStamp) const;
    ASIOError getChannelInfo(ASIOChannelInfo* info) const;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks);
    ASIOError disposeBuffers();
    ASIOError future(long selector, void* opt);
    ASIOError outputReady();

    // -- For the panel, the log and the tests --------------------------------

    bool initialized() const { return card_ != nullptr; }
    bool prepared() const { return prepared_; }
    bool running() const { return running_.load(std::memory_order_acquire); }
    /// The settings in use (with the card's latencies the opener knows, if
    /// it does) and the ones init was given.
    const DriverSettings& settings() const { return settings_; }
    const DriverSettings& requestedSettings() const { return requested_; }
    /// True when the opener reports the latencies (the simulated card) and
    /// the settings' own are not used.
    bool latenciesFromCard() const { return latenciesFromOpener_; }

    enum class SettingsApplied
    {
        Nothing,             ///< nothing changed
        Now,                 ///< latencies only: in use at once, the host told
        WhenTheHostResets,   ///< the host was asked to reset and will reopen with them
        NextOpen,            ///< the host cannot be asked: they apply when it opens the driver again
    };

    /// The panel saved `updated` (already stored where init reads it).
    /// Latencies apply at once and the host is told (kAsioLatenciesChanged);
    /// Keep my routing and the analog level apply at once too, the level on
    /// the engine's thread after the next block; anything else asks the host
    /// for a reset (kAsioResetRequest), after which init reads the new
    /// settings.
    SettingsApplied settingsChanged(const DriverSettings& updated);
    const ChannelMap& channels() const { return channels_; }
    engine::EngineStats stats() const;

    /// Without an engine thread: runs `count` blocks on the caller's thread,
    /// which then counts as the engine's. False if not running or stopped.
    bool runBlocks(unsigned count);

    /// How many times each message was due, whether or not the host takes it.
    struct MessageCounts
    {
        std::uint64_t overload = 0;
        std::uint64_t resync = 0;
        std::uint64_t reset = 0;
    };
    MessageCounts messagesSent() const;

    /// What the host said it supports, asked in createBuffers.
    struct HostSupport
    {
        bool timeInfo = false;
        bool overload = false;
        bool resync = false;
        bool reset = false;
        bool latenciesChanged = false;
    };
    const HostSupport& hostSupport() const { return host_; }

    /// What the routing watch has seen this run, from any thread (the panel).
    struct RoutingStatus
    {
        bool watched = false;            ///< read back at start: checked once a second while running
        bool keep = false;               ///< Keep my routing
        std::uint64_t changes = 0;       ///< parts another program changed this run
        std::uint64_t putBack = 0;       ///< parts put back
        unsigned notOurs = 0;            ///< device::routing bits that were not the driver's at the last check
        device::TaskMode taskMode = device::TaskMode::Unknown;
        int analogLevelDbu = 0;          ///< the Xpand's, as last read; 0: not known
    };
    RoutingStatus routingStatus() const;

    /// What is wrong with the SDI connectors the settings use, in sentences
    /// ("SDI 2 is set as an input, so ..."); empty when nothing is. On the
    /// KONA X each connector is an input or an output, as the AJA Control
    /// Panel set it, and the driver only reads which. Read at init and at
    /// each start, on the host's thread.
    const std::string& sdiNote() const { return sdiNote_; }

private:
    // engine::EngineClient, on the engine's thread
    engine::BlockBuffers buffers(unsigned half) override;
    void process(const engine::BlockInfo& info, unsigned half) override;
    void onEngineEvent(engine::EngineEvent event, const engine::BlockInfo& info) override;
    void afterBlock(const engine::BlockInfo& info) override;

    void watchRouting();
    std::string checkSdiConnectors() const;
    void applyAnalogLevel(int dbu);
    bool routingLineDue();
    void engineThread();
    void releaseCard();
    void logf(const char* format, ...) AJADUPLEX_PRINTF(2, 3);
    void logLines(const std::string& prefix, const std::string& text);
    void logEvent(engine::EngineEvent event, const engine::BlockInfo& info);
    void logRunSummary();
    long hostMessage(long selector, long value);
    bool hostSupports(long selector);
    void sendOverload();
    void sendResync();
    unsigned leadFrames(unsigned blockFrames) const;
    unsigned activeBlockFrames() const;
    std::int64_t systemTimeNow() const;
    void releaseBuffers();

    CardOpener& opener_;
    Clock& clock_;
    DriverOptions options_;

    DriverSettings settings_;
    DriverSettings requested_;
    bool latenciesFromOpener_ = false;
    ChannelMap channels_;
    std::string clockName_;
    std::string sdiNote_;
    std::string error_;
    std::unique_ptr<device::CardAudio> card_;
    std::unique_ptr<engine::AudioEngine> engine_;
    std::unique_ptr<engine::RoutingWatch> routing_;
    /// The card's task mode is in the driver's hands (DriverOptions::
    /// taskModeHeld was told so), and which card that is.
    bool taskModeHeld_ = false;
    unsigned taskModeHeldCard_ = 0;

    // Prepared by createBuffers
    bool prepared_ = false;
    unsigned blockFrames_ = 0;
    ASIOCallbacks callbacks_{};
    HostSupport host_;
    std::vector<std::unique_ptr<AlignedBuffer>> hostBuffers_;   ///< one per created channel, both halves
    std::vector<bool> inputActive_;
    std::vector<bool> outputActive_;
    std::array<std::array<std::int32_t*, kCardChannels>, 2> inputPointers_{};
    std::array<std::array<const std::int32_t*, kCardChannels>, 2> outputPointers_{};

    // Running
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};   ///< by the host, as opposed to the engine giving up
    std::thread thread_;
    std::int64_t systemTimeOffsetNs_ = 0;
    ASIOTime time_{};

    std::int64_t startedNs_ = 0;
    std::uint64_t eventCounts_[8] = {};   ///< per EngineEvent, this run: the log names the first few

    std::atomic<std::uint64_t> overloadsSent_{0};
    std::atomic<std::uint64_t> resyncsSent_{0};
    std::atomic<std::uint64_t> resetsSent_{0};

    // The routing watch: the engine's thread checks, any thread reads.
    std::atomic<bool> keepRouting_{false};
    std::int64_t lastRoutingCheckNs_ = 0;
    std::uint64_t routingLines_ = 0;   ///< this run: the log names the first few, then one in a hundred
    std::atomic<bool> routingWatched_{false};
    std::atomic<std::uint64_t> routingChanges_{0};
    std::atomic<std::uint64_t> routingPutBack_{0};
    std::atomic<unsigned> routingNotOurs_{0};
    std::atomic<int> taskMode_{0};
    std::atomic<int> analogLevel_{0};
    std::atomic<int> pendingLevel_{-1};   ///< a level the panel chose while running, for the engine's thread; -1 none
};

} // namespace ajaduplex::asio
