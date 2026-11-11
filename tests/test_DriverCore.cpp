// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// The ASIO driver's core on the simulated card, driven the way a host drives
// a driver: init, the queries, createBuffers with plain C callbacks, start,
// blocks, stop. The card's AES outputs are looped to its inputs, so what
// the host writes comes back to it.

#include "test_framework.h"

#include "DuplexVersion.h"
#include "asio/CardOpeners.h"
#include "asio/DriverCore.h"
#include "platform/Realtime.h"
#include "sim/FakeCardAudio.h"
#include "sim/VirtualClock.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ajaduplex;
using namespace ajaduplex::asio;
using sim::FakeCardAudio;
using sim::FakeCardModel;
using sim::VirtualClock;

namespace
{

constexpr std::int64_t kMs = 1000000;

/// Opens a simulated card, or fails with a reason.
class TestOpener final : public CardOpener
{
public:
    TestOpener(Clock& clock, FakeCardModel model, bool virtualTime = true)
        : clock_(clock), model_(std::move(model)), virtualTime_(virtualTime)
    {
    }

    std::unique_ptr<device::CardAudio> open(const DriverSettings& settings, std::string& error) override
    {
        opened = settings;
        if (!failWith.empty())
        {
            error = failWith;
            return nullptr;
        }
        if (virtualTime_)
        {
            auto card = std::make_unique<FakeCardAudio>(static_cast<VirtualClock&>(clock_), model_);
            card_ = card.get();
            return card;
        }
        auto card = std::make_unique<FakeCardAudio>(clock_, model_);
        card_ = card.get();
        return card;
    }

    std::string clockSourceName() override { return "Simulated reference"; }

    FakeCardAudio* card() { return card_; }

    std::string failWith;
    DriverSettings opened;

private:
    Clock& clock_;
    FakeCardModel model_;
    bool virtualTime_;
    FakeCardAudio* card_ = nullptr;
};

std::int32_t encode(std::int64_t frame)
{
    return static_cast<std::int32_t>(((std::uint32_t(frame) & 0xFFFFFu) << 12) | 0x100u);
}

/// A host, as far as the driver can tell: ASIO's callbacks are plain
/// functions, so they reach it through a global, as in real hosts.
struct TestHost
{
    DriverCore* core = nullptr;
    VirtualClock* clock = nullptr;
    std::vector<ASIOBufferInfo> infos;
    unsigned frames = 0;

    bool takesTimeInfo = true;
    bool takesMessages = true;

    std::vector<long> halves;
    std::vector<std::int64_t> positions;
    std::vector<std::int64_t> times;
    std::vector<std::int64_t> polledPositions;
    std::vector<std::int32_t> heard;       ///< input 0, block after block
    std::vector<long> messages;
    std::int64_t written = 0;              ///< frames written to output 0 so far

    std::uint64_t outputReadyAt = ~std::uint64_t(0);
    std::uint64_t sleepAt = ~std::uint64_t(0);
    std::int64_t sleepNs = 0;
    std::function<void(std::uint64_t)> everyBlock;   ///< called with the block's number, inside the callback

    void onBuffer(long index, const ASIOTime* time)
    {
        halves.push_back(index);
        if (time != nullptr)
        {
            positions.push_back(getHiLo(time->timeInfo.samplePosition));
            times.push_back(getHiLo(time->timeInfo.systemTime));
        }
        ASIOSamples position{};
        ASIOTimeStamp stamp{};
        if (core->getSamplePosition(&position, &stamp) == ASE_OK)
            polledPositions.push_back(getHiLo(position));

        for (const ASIOBufferInfo& b : infos)
        {
            auto* samples = static_cast<std::int32_t*>(b.buffers[index]);
            if (b.isInput && b.channelNum == 0)
                heard.insert(heard.end(), samples, samples + frames);
            if (!b.isInput && b.channelNum == 0)
            {
                for (unsigned f = 0; f < frames; ++f)
                    samples[f] = encode(written + f);
            }
        }
        written += frames;
        const std::uint64_t block = halves.size() - 1;
        if (block == outputReadyAt)
        {
            core->outputReady();
            for (const ASIOBufferInfo& b : infos)
            {
                if (!b.isInput)
                    std::memset(b.buffers[index], 0x7F, frames * sizeof(std::int32_t));   // too late to be sent
            }
        }
        if (block == sleepAt && clock != nullptr)
            clock->advance(sleepNs);
        if (everyBlock)
            everyBlock(block);
    }

    long onMessage(long selector, long value)
    {
        if (selector == kAsioSelectorSupported)
        {
            if (!takesMessages)
                return 0;
            return value == kAsioOverload || value == kAsioResyncRequest || value == kAsioResetRequest ||
                           value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo
                       ? 1
                       : 0;
        }
        if (selector == kAsioSupportsTimeInfo)
            return takesTimeInfo ? 1 : 0;
        if (selector == kAsioEngineVersion)
            return 2;
        messages.push_back(selector);
        return 1;
    }

    std::size_t count(long selector) const
    {
        std::size_t n = 0;
        for (const long m : messages)
            n += m == selector;
        return n;
    }

    /// Input 0's round trips: frames between writing a counter and hearing it.
    std::vector<std::int64_t> roundTrips() const
    {
        std::vector<std::int64_t> trips;
        for (std::size_t i = 0; i < heard.size(); ++i)
        {
            const std::uint32_t v = static_cast<std::uint32_t>(heard[i]);
            if (v == 0 || (v & 0xFFFu) != 0x100u)
                continue;
            trips.push_back((static_cast<std::int64_t>(i) - static_cast<std::int64_t>((v >> 12) & 0xFFFFF)) & 0xFFFFF);
        }
        return trips;
    }
};

TestHost* gHost = nullptr;

void bufferSwitch(long index, ASIOBool)
{
    gHost->onBuffer(index, nullptr);
}

ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool)
{
    gHost->onBuffer(index, time);
    return time;
}

void sampleRateDidChange(ASIOSampleRate) {}

long asioMessage(long selector, long value, void*, double*)
{
    return gHost->onMessage(selector, value);
}

ASIOCallbacks callbacks()
{
    ASIOCallbacks c{};
    c.bufferSwitch = bufferSwitch;
    c.sampleRateDidChange = sampleRateDidChange;
    c.asioMessage = asioMessage;
    c.bufferSwitchTimeInfo = bufferSwitchTimeInfo;
    return c;
}

/// Keeps what the driver logs.
class CaptureLog final : public LogTarget
{
public:
    void line(const char* text) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(text);
    }

    std::vector<std::string> lines()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    std::size_t count(const std::string& part)
    {
        std::size_t n = 0;
        for (const std::string& l : lines())
            n += l.find(part) != std::string::npos;
        return n;
    }

    std::string find(const std::string& part)
    {
        for (const std::string& l : lines())
        {
            if (l.find(part) != std::string::npos)
                return l;
        }
        return std::string();
    }

    /// Every line with `part`, in order.
    std::vector<std::string> all(const std::string& part)
    {
        std::vector<std::string> found;
        for (const std::string& l : lines())
        {
            if (l.find(part) != std::string::npos)
                found.push_back(l);
        }
        return found;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

DriverOptions unthreaded(LogTarget* log = nullptr)
{
    DriverOptions options;
    options.threaded = false;
    options.proAudioPriority = false;
    options.log = log;
    return options;
}

DriverSettings aesInput()
{
    DriverSettings s;
    s.input = InputChoice::Aes;
    return s;
}

/// Everything a test needs: clock, card, core, host, callbacks.
struct Rig
{
    VirtualClock clock;
    TestOpener opener;
    DriverCore core;
    TestHost host;
    ASIOCallbacks cb = callbacks();

    explicit Rig(FakeCardModel model = FakeCardModel::withAesLoop(), DriverOptions options = unthreaded())
        : opener(clock, std::move(model)), core(opener, clock, std::move(options))
    {
        host.core = &core;
        host.clock = &clock;
        gHost = &host;
    }

    /// createBuffers for every input and output. The host only takes the
    /// buffers on success, as a host would.
    ASIOError prepare(unsigned frames)
    {
        long inputs = 0, outputs = 0;
        core.getChannels(&inputs, &outputs);
        std::vector<ASIOBufferInfo> infos;
        for (long i = 0; i < inputs; ++i)
            infos.push_back(ASIOBufferInfo{ASIOTrue, i, {nullptr, nullptr}});
        for (long i = 0; i < outputs; ++i)
            infos.push_back(ASIOBufferInfo{ASIOFalse, i, {nullptr, nullptr}});
        const ASIOError result =
            core.createBuffers(infos.data(), static_cast<long>(infos.size()), static_cast<long>(frames), &cb);
        if (result == ASE_OK)
        {
            host.infos = infos;
            host.frames = frames;
        }
        return result;
    }
};

std::size_t changes(const std::vector<std::int64_t>& values)
{
    std::size_t n = 0;
    for (const std::int64_t v : values)
        n += v != values.front();
    return n;
}

} // namespace

TEST_CASE(DriverCore, EverythingIsNotPresentBeforeInit)
{
    Rig rig;
    long a = 0, b = 0, c = 0, d = 0;
    EXPECT_EQ(rig.core.getChannels(&a, &b), ASE_NotPresent);
    EXPECT_EQ(rig.core.getBufferSize(&a, &b, &c, &d), ASE_NotPresent);
    EXPECT_EQ(rig.core.getLatencies(&a, &b), ASE_NotPresent);
    EXPECT_EQ(rig.core.start(), ASE_NotPresent);
    EXPECT_EQ(rig.core.canSampleRate(48000.0), ASE_NotPresent);
    EXPECT_EQ(rig.prepare(128), ASE_NotPresent);
    EXPECT_FALSE(rig.core.initialized());
}

TEST_CASE(DriverCore, InitPassesTheOpenersReasonOn)
{
    Rig rig;
    rig.opener.failWith = "Audio system 2 of KONA X is in use by another program.";
    EXPECT_FALSE(rig.core.init(DriverSettings{}));
    EXPECT_EQ(rig.core.errorMessage(), std::string("Audio system 2 of KONA X is in use by another program."));
    rig.opener.failWith.clear();
    EXPECT_TRUE(rig.core.init(DriverSettings{}));
    EXPECT_TRUE(rig.core.errorMessage().empty());
}

TEST_CASE(DriverCore, AnalogIsTheDefaultInput)
{
    Rig rig;
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    long inputs = 0, outputs = 0;
    ASSERT_EQ(rig.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(inputs, 2L);
    EXPECT_EQ(outputs, 8L);
    ASIOChannelInfo info{};
    info.channel = 1;
    info.isInput = ASIOTrue;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("Analog R"));
    EXPECT_EQ(info.type, ASIOSTInt32LSB);
    EXPECT_EQ(info.isActive, ASIOFalse);
    info.channel = 0;
    info.isInput = ASIOFalse;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("AES 1 + Analog L"));
    info.channel = 7;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("AES 8"));
    info.channel = 8;
    EXPECT_EQ(rig.core.getChannelInfo(&info), ASE_InvalidParameter);
}

TEST_CASE(DriverCore, AesInputGivesEightChannels)
{
    Rig rig;
    ASSERT_TRUE(rig.core.init(aesInput()));
    long inputs = 0, outputs = 0;
    ASSERT_EQ(rig.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(inputs, 8L);
    ASIOChannelInfo info{};
    info.channel = 5;
    info.isInput = ASIOTrue;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("AES 6"));
}

TEST_CASE(DriverCore, SizesRatesAndClock)
{
    Rig rig;
    DriverSettings settings;
    settings.bufferFrames = 256;
    ASSERT_TRUE(rig.core.init(settings));
    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    ASSERT_EQ(rig.core.getBufferSize(&minSize, &maxSize, &preferred, &granularity), ASE_OK);
    EXPECT_EQ(minSize, 64L);
    EXPECT_EQ(maxSize, 2048L);
    EXPECT_EQ(preferred, 256L);
    EXPECT_EQ(granularity, -1L);

    EXPECT_EQ(rig.core.canSampleRate(48000.0), ASE_OK);
    EXPECT_EQ(rig.core.canSampleRate(44100.0), ASE_NoClock);
    EXPECT_EQ(rig.core.canSampleRate(96000.0), ASE_NoClock);
    EXPECT_EQ(rig.core.setSampleRate(48000.0), ASE_OK);
    EXPECT_EQ(rig.core.setSampleRate(44100.0), ASE_NoClock);
    ASIOSampleRate rate = 0.0;
    ASSERT_EQ(rig.core.getSampleRate(&rate), ASE_OK);
    EXPECT_TRUE(rate == 48000.0);

    ASIOClockSource clocks[4]{};
    long count = 4;
    ASSERT_EQ(rig.core.getClockSources(clocks, &count), ASE_OK);
    EXPECT_EQ(count, 1L);
    EXPECT_EQ(clocks[0].isCurrentSource, ASIOTrue);
    EXPECT_EQ(std::string(clocks[0].name), std::string("Card reference: Free Run"));   // the card's own name for it
    EXPECT_EQ(rig.core.setClockSource(0), ASE_OK);
    EXPECT_EQ(rig.core.setClockSource(1), ASE_InvalidParameter);

    EXPECT_EQ(rig.core.future(kAsioCanTimeInfo, nullptr), ASE_SUCCESS);
    EXPECT_EQ(rig.core.future(kAsioCanReportOverload, nullptr), ASE_SUCCESS);
    EXPECT_EQ(rig.core.future(kAsioCanTimeCode, nullptr), ASE_InvalidParameter);
    EXPECT_EQ(rig.core.future(kAsioEnableTimeCodeRead, nullptr), ASE_InvalidParameter);
    EXPECT_EQ(rig.core.driverVersion(),
              long((AJADUPLEX_VERSION_MAJOR << 16) | (AJADUPLEX_VERSION_MINOR << 8) | AJADUPLEX_VERSION_PATCH));
}

TEST_CASE(DriverCore, LatenciesAreNAndSPlusTheHardware)
{
    Rig rig;
    DriverSettings settings;
    settings.inputLatencyFrames = 30;
    settings.outputLatencyFrames = 42;
    ASSERT_TRUE(rig.core.init(settings));
    const long g = static_cast<long>(settings.guardFrames);   // the default
    long in = 0, out = 0;
    // Before createBuffers: the preferred size.
    ASSERT_EQ(rig.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(in, 128L + 30);
    EXPECT_EQ(out, 128L + g + 42);
    // After: the size the host chose.
    ASSERT_EQ(rig.prepare(512), ASE_OK);
    ASSERT_EQ(rig.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(in, 512L + 30);
    EXPECT_EQ(out, 512L + g + 42);

    Rig aggressive;
    settings.aggressive = true;
    ASSERT_TRUE(aggressive.core.init(settings));
    ASSERT_EQ(aggressive.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(out, 64L + g + 42);
}

TEST_CASE(DriverCore, CreateBuffersChecksWhatItIsGiven)
{
    Rig rig;
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    EXPECT_EQ(rig.prepare(100), ASE_InvalidMode);    // not a power of two
    EXPECT_CONTAINS(rig.core.errorMessage(), "power of two");
    EXPECT_EQ(rig.prepare(32), ASE_InvalidMode);     // under the minimum
    EXPECT_EQ(rig.prepare(4096), ASE_InvalidMode);   // over the maximum

    ASIOBufferInfo twice[2] = {{ASIOTrue, 0, {nullptr, nullptr}}, {ASIOTrue, 0, {nullptr, nullptr}}};
    EXPECT_EQ(rig.core.createBuffers(twice, 2, 128, &rig.cb), ASE_InvalidMode);
    ASIOBufferInfo missing[1] = {{ASIOTrue, 2, {nullptr, nullptr}}};   // analog has 2 inputs
    EXPECT_EQ(rig.core.createBuffers(missing, 1, 128, &rig.cb), ASE_InvalidMode);
    ASIOBufferInfo fine[1] = {{ASIOFalse, 7, {nullptr, nullptr}}};
    EXPECT_EQ(rig.core.createBuffers(fine, 1, 128, nullptr), ASE_InvalidParameter);
    EXPECT_FALSE(rig.core.prepared());

    EXPECT_EQ(rig.core.createBuffers(fine, 1, 128, &rig.cb), ASE_OK);
    EXPECT_TRUE(fine[0].buffers[0] != nullptr && fine[0].buffers[1] != nullptr);
    EXPECT_EQ(static_cast<std::int32_t*>(fine[0].buffers[1]) - static_cast<std::int32_t*>(fine[0].buffers[0]),
              std::ptrdiff_t(128));
    ASIOChannelInfo info{};
    info.channel = 7;
    info.isInput = ASIOFalse;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(info.isActive, ASIOTrue);
    EXPECT_TRUE(rig.core.hostSupport().timeInfo);
    EXPECT_TRUE(rig.core.hostSupport().overload);
}

TEST_CASE(DriverCore, TheHostHearsItsOwnOutputBack)
{
    Rig rig;
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_TRUE(rig.core.running());
    ASSERT_TRUE(rig.core.runBlocks(400));
    ASSERT_EQ(rig.core.stop(), ASE_OK);
    EXPECT_FALSE(rig.core.running());

    // Halves alternate from 0; positions are N apart from 0, in the time
    // info and from getSamplePosition alike; times move forward.
    ASSERT_EQ(rig.host.halves.size(), std::size_t(400));
    for (std::size_t i = 0; i < rig.host.halves.size(); ++i)
        EXPECT_EQ(rig.host.halves[i], long(i % 2));
    ASSERT_EQ(rig.host.positions.size(), std::size_t(400));
    for (std::size_t i = 0; i < rig.host.positions.size(); ++i)
    {
        EXPECT_EQ(rig.host.positions[i], std::int64_t(i) * 128);
        EXPECT_EQ(rig.host.polledPositions[i], rig.host.positions[i]);
    }
    for (std::size_t i = 1; i < rig.host.times.size(); ++i)
        EXPECT_TRUE(rig.host.times[i] > rig.host.times[i - 1]);

    // AES out 1 comes back on AES in 1, every frame, at one round trip:
    // N + S and what the simulated card and cable add.
    const std::int64_t s = 128 + std::int64_t(aesInput().guardFrames);
    const std::vector<std::int64_t> trips = rig.host.roundTrips();
    ASSERT_TRUE(trips.size() > 40000);
    EXPECT_EQ(changes(trips), std::size_t(0));
    EXPECT_TRUE(trips.front() > 128 + s && trips.front() < 128 + s + 100);
    EXPECT_EQ(rig.core.stats().lateOutputs, std::uint64_t(0));
    EXPECT_TRUE(rig.host.messages.empty());
}

TEST_CASE(DriverCore, HostWithoutTimeInfoGetsBufferSwitch)
{
    Rig rig;
    rig.host.takesTimeInfo = false;
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(256), ASE_OK);
    EXPECT_FALSE(rig.core.hostSupport().timeInfo);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(50));
    rig.core.stop();
    EXPECT_EQ(rig.host.halves.size(), std::size_t(50));
    EXPECT_TRUE(rig.host.positions.empty());   // no ASIOTime came
    ASSERT_EQ(rig.host.polledPositions.size(), std::size_t(50));
    EXPECT_EQ(rig.host.polledPositions[49], std::int64_t(49) * 256);
}

TEST_CASE(DriverCore, OutputReadyInsideTheCallbackSendsAtOnce)
{
    Rig rig;
    rig.host.outputReadyAt = 20;
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    EXPECT_EQ(rig.core.outputReady(), ASE_OK);   // outside a callback: nothing to send, still OK
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(200));
    rig.core.stop();
    // Block 20 went out before the host overwrote it.
    for (const std::int32_t sample : rig.host.heard)
        EXPECT_NE(sample, std::int32_t(0x7F7F7F7F));
    EXPECT_EQ(changes(rig.host.roundTrips()), std::size_t(0));
}

TEST_CASE(DriverCore, LateOutputIsAnOverloadNotAResync)
{
    Rig rig;
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(100));
    rig.opener.card()->delayNextDmas(2, 6 * kMs);
    ASSERT_TRUE(rig.core.runBlocks(300));
    rig.core.stop();
    const engine::EngineStats s = rig.core.stats();
    EXPECT_TRUE(s.lateOutputs > 0);
    EXPECT_EQ(rig.host.count(kAsioOverload), std::size_t(s.lateOutputs));
    EXPECT_EQ(rig.host.count(kAsioResyncRequest), std::size_t(0));
    EXPECT_EQ(changes(rig.host.roundTrips()), std::size_t(0));   // the latency the host was told holds
}

TEST_CASE(DriverCore, HungHostGetsOverloadAndResync)
{
    Rig rig;
    rig.host.sleepAt = 50;
    rig.host.sleepNs = 1000 * kMs;
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(300));
    rig.core.stop();
    EXPECT_EQ(rig.core.stats().inputOverruns, std::uint64_t(1));
    EXPECT_EQ(rig.host.count(kAsioResyncRequest), std::size_t(1));
    EXPECT_TRUE(rig.host.count(kAsioOverload) >= 1);
    // The sample position jumped by what was skipped, and stays N apart after.
    ASSERT_TRUE(rig.host.positions.size() > 52);
    EXPECT_TRUE(rig.host.positions[51] - rig.host.positions[50] > 40000);
    EXPECT_EQ(rig.host.positions[52] - rig.host.positions[51], std::int64_t(128));
}

TEST_CASE(DriverCore, HostThatTakesNoMessagesIsLeftAlone)
{
    Rig rig;
    rig.host.takesMessages = false;
    rig.host.sleepAt = 50;
    rig.host.sleepNs = 1000 * kMs;
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(200));
    rig.core.stop();
    EXPECT_TRUE(rig.host.messages.empty());
    EXPECT_EQ(rig.core.messagesSent().resync, std::uint64_t(1));   // due, but not sent
}

TEST_CASE(DriverCore, StopStartAndDispose)
{
    Rig rig;
    ASSERT_TRUE(rig.core.init(aesInput()));
    EXPECT_EQ(rig.core.disposeBuffers(), ASE_InvalidMode);   // nothing prepared
    EXPECT_EQ(rig.core.start(), ASE_InvalidMode);            // nor here
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_EQ(rig.core.start(), ASE_OK);                     // already running
    EXPECT_EQ(rig.prepare(256), ASE_InvalidMode);            // not while running
    ASSERT_TRUE(rig.core.runBlocks(30));
    ASSERT_EQ(rig.core.stop(), ASE_OK);
    EXPECT_EQ(rig.core.stop(), ASE_OK);                      // already stopped
    ASIOSamples position{};
    ASIOTimeStamp stamp{};
    EXPECT_EQ(rig.core.getSamplePosition(&position, &stamp), ASE_SPNotAdvancing);

    // Started again, the sample position starts again from 0.
    rig.host.positions.clear();
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(10));
    ASSERT_EQ(rig.host.positions.size(), std::size_t(10));
    EXPECT_EQ(rig.host.positions.front(), std::int64_t(0));

    // Disposing stops; buffers of another size after.
    ASSERT_EQ(rig.core.disposeBuffers(), ASE_OK);
    EXPECT_FALSE(rig.core.running());
    EXPECT_FALSE(rig.core.prepared());
    ASSERT_EQ(rig.prepare(512), ASE_OK);
    rig.host.positions.clear();
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(3));
    ASSERT_EQ(rig.host.positions.size(), std::size_t(3));
    EXPECT_EQ(rig.host.positions[2], std::int64_t(1024));
}

TEST_CASE(DriverCore, AnalogLoopThroughTheDriver)
{
    // XLR out L/R looped to XLR in L/R: output 1 ("AES 1 + Analog L") comes
    // back on "Analog L" through the simulated converters.
    Rig rig(FakeCardModel::withAnalogLoop());
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(200));
    rig.core.stop();
    // The counter does not survive converters, but its energy does.
    double energy = 0.0;
    for (std::size_t i = rig.host.heard.size() / 2; i < rig.host.heard.size(); ++i)
        energy += std::fabs(static_cast<double>(rig.host.heard[i]));
    EXPECT_TRUE(energy > 1e9);
    ASSERT_TRUE(rig.opener.card() != nullptr);
    EXPECT_TRUE(rig.opener.card()->analogSelected());
    EXPECT_TRUE(rig.opener.card()->aesOutputsRouted());
    EXPECT_TRUE(rig.opener.card()->monitorRouted());
}

TEST_CASE(DriverCore, RunsOnItsOwnThreadInRealTime)
{
    // The simulated card on the PC's clock, the engine on its thread, the
    // host only waiting: about 375 blocks a second at N = 128.
    platform::SystemClock clock;
    TestOpener opener(clock, FakeCardModel::withAesLoop(), /*virtualTime*/ false);
    DriverOptions options;
    options.proAudioPriority = false;
    DriverCore core(opener, clock, options);
    TestHost host;
    host.core = &core;
    gHost = &host;
    ASIOCallbacks cb = callbacks();
    ASSERT_TRUE(core.init(aesInput()));
    long inputs = 0, outputs = 0;
    core.getChannels(&inputs, &outputs);
    host.infos = {ASIOBufferInfo{ASIOTrue, 0, {nullptr, nullptr}}, ASIOBufferInfo{ASIOFalse, 0, {nullptr, nullptr}}};
    host.frames = 128;
    ASSERT_EQ(core.createBuffers(host.infos.data(), 2, 128, &cb), ASE_OK);
    ASSERT_EQ(core.start(), ASE_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ASSERT_EQ(core.stop(), ASE_OK);
    const std::size_t blocks = host.halves.size();
    EXPECT_TRUE(blocks > 100 && blocks < 200);
    const std::vector<std::int64_t> trips = host.roundTrips();
    EXPECT_TRUE(!trips.empty());
    // Under a loaded test machine a late block is possible; it keeps the
    // round trip all the same.
    EXPECT_EQ(changes(trips), std::size_t(0));
    EXPECT_FALSE(core.running());
    gHost = nullptr;
}

TEST_CASE(DriverCore, WithoutTheXpandInitSaysSo)
{
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.hasBreakoutBoard = false;
    CaptureLog log;
    Rig rig(model, unthreaded(&log));
    EXPECT_FALSE(rig.core.init(DriverSettings{}));
    EXPECT_CONTAINS(rig.core.errorMessage(), "Xpand");
    EXPECT_FALSE(rig.core.initialized());
    EXPECT_EQ(log.count("init failed: The Xpand"), std::size_t(1));
}

TEST_CASE(DriverCore, LogsItsLife)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    EXPECT_CONTAINS(log.find("init: "), "AJA Duplex ASIO");
    EXPECT_CONTAINS(log.find("init: "), "Input = 1 (AES), SdiInput = 1, BufferSize = 128");
    EXPECT_EQ(log.count("card: "), std::size_t(1));   // the simulated card describes itself in one line
    EXPECT_CONTAINS(log.find("ready: "), "8 inputs, 8 outputs, clock Card reference: Free Run");

    ASSERT_EQ(rig.prepare(128), ASE_OK);
    EXPECT_CONTAINS(log.find("createBuffers: "), "N = 128, 8 inputs and 8 outputs active");
    EXPECT_CONTAINS(log.find("createBuffers: "), "time info");
    ASSERT_EQ(rig.core.start(), ASE_OK);
    // The defaults: N = 128 and G = 256, 8 ms for each block; and on the way
    // out, the 208 frames of the card and its converters with the analog
    // input, which no PC has to measure before the driver reports them.
    EXPECT_CONTAINS(log.find("start: "), "N = 128, G = 256, S = 384; latencies reported: in 128, out 592");
    ASSERT_TRUE(rig.core.runBlocks(100));
    rig.opener.card()->delayNextDmas(2, 6 * kMs);
    ASSERT_TRUE(rig.core.runBlocks(100));
    rig.core.stop();
    EXPECT_EQ(log.count("late output at "), std::size_t(rig.core.stats().lateOutputs));
    // Each says where the time went: here two DMAs held the engine up 12
    // ms, so the block after was found late, past its 8 ms. It loses what
    // the card passed and G after it: with G over N, all of it.
    EXPECT_CONTAINS(log.find("late output at "), "ms late; a block has 8.00 ms from complete to written, this one "
                                                 "was found after ");
    EXPECT_CONTAINS(log.find("late output at "), "and the host ");
    EXPECT_CONTAINS(log.find("stop: "), "200 blocks in");
    EXPECT_CONTAINS(log.find("stop: "), "1 late outputs (128 frames dropped)");
    EXPECT_EQ(log.count("smallest output margin"), std::size_t(1));
}

TEST_CASE(DriverCore, ADmaErrorSaysWhichDma)
{
    // The first session with the card logged a DMA error every 1.36 s and
    // no more than that: the line now says which DMA, what the system said
    // of it, and how second tries went.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(10));
    rig.opener.card()->failNextDmas(1, 87);   // the next block's input; once
    ASSERT_TRUE(rig.core.runBlocks(10));
    rig.opener.card()->failNextDmas(2);       // the next block's input, and its first second try
    ASSERT_TRUE(rig.core.runBlocks(10));
    rig.opener.card()->failPlaybackWritesAt(0, 1784);   // what wraps at the ring's end, wherever it comes from
    ASSERT_TRUE(rig.core.runBlocks(600));
    rig.core.stop();

    const std::vector<std::string> saved = log.all("DMA error, recovered at ");
    ASSERT_EQ(saved.size(), std::size_t(2));
    EXPECT_CONTAINS(saved[0], "(1 so far): input, the whole block: 8192 bytes at ring offset ");
    EXPECT_CONTAINS(saved[0], "; system error 87; again from the same buffer: went through; nothing lost");
    EXPECT_CONTAINS(saved[1], "(2 so far): input, the whole block: ");
    EXPECT_CONTAINS(saved[1], "; system error 0 (none: refused before the card's driver); again from the same "
                              "buffer: failed, system error 0; from a spare buffer: went through; nothing lost");

    const std::vector<std::string> lost = log.all("DMA error at ");
    ASSERT_EQ(lost.size(), std::size_t(1));
    EXPECT_CONTAINS(lost[0], "(1 so far): output, the part of the block past the ring's end, at the ring's start: ");
    EXPECT_CONTAINS(lost[0], " bytes at ring offset 0; system error 1784; again from the same buffer: failed, "
                             "system error 1784; from a spare buffer: failed, system error 1784");
    EXPECT_CONTAINS(log.find("stop: "), "1 DMA errors, 2 recovered");
    // The input buffer the spare replaced went through at stop: no bad buffer.
    EXPECT_CONTAINS(log.find("stop: a buffer retired"), "(input, 8192 bytes at ring offset ");
    EXPECT_CONTAINS(log.find("stop: a buffer retired"), "went through at stop, with the card stopped");
}

TEST_CASE(DriverCore, ABufferThatFailsStartsCheckIsLogged)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_EQ(log.count("buffers: "), std::size_t(0));   // all passed: nothing said
    rig.core.stop();

    rig.opener.card()->failDmasThroughBufferAt(device::Ring::Playback, 0x1FE000u, 31, 1);   // after the zeroing
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_CONTAINS(log.find("buffers: "), "1 of 10 buffers failed a 512-byte DMA before the run (a write to the "
                                           "card, system error 31): 1 replaced by a spare that passed; 3 spares left");
    ASSERT_TRUE(rig.core.runBlocks(100));
    rig.core.stop();
    EXPECT_CONTAINS(log.find("stop: "), "0 DMA errors");
}

TEST_CASE(DriverCore, ABurstOfEventsIsNotLoggedLineByLine)
{
    // Wake-ups 2 ms late again and again at N = 64, G = 8: hundreds of late
    // outputs; the log names the first ten and the stop line counts them.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    rig.clock.setSleepLateness(sim::SleepLateness{20000, 0.05, 2000000, 5});
    DriverSettings settings = aesInput();
    settings.guardFrames = 8;
    ASSERT_TRUE(rig.core.init(settings));
    ASSERT_EQ(rig.prepare(64), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(5000));
    rig.core.stop();
    const std::uint64_t late = rig.core.stats().lateOutputs;
    EXPECT_TRUE(late > 50);
    const std::size_t logged = log.count("late output at ");
    EXPECT_TRUE(logged >= 10 && logged <= 10 + late / 1000);
    EXPECT_CONTAINS(log.find("stop: "), std::to_string(late) + " late outputs");
}

TEST_CASE(DriverCore, LatenciesChangeAtOnceTheRestAsksForAReset)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings settings = aesInput();
    ASSERT_TRUE(rig.core.init(settings));
    // Before createBuffers there is no host to tell.
    DriverSettings changed = settings;
    changed.input = InputChoice::Analog;
    EXPECT_TRUE(rig.core.settingsChanged(changed) == DriverCore::SettingsApplied::NextOpen);
    EXPECT_TRUE(rig.core.settingsChanged(settings) == DriverCore::SettingsApplied::Nothing);

    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(20));

    // Latencies: in use now, the host told.
    changed = settings;
    changed.inputLatencyFrames = 30;
    changed.outputLatencyFrames = 42;
    EXPECT_TRUE(rig.core.settingsChanged(changed) == DriverCore::SettingsApplied::Now);
    long in = 0, out = 0;
    ASSERT_EQ(rig.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(in, 128L + 30);
    EXPECT_EQ(out, 128L + long(settings.guardFrames) + 42);
    EXPECT_EQ(rig.host.count(kAsioLatenciesChanged), std::size_t(1));
    EXPECT_TRUE(rig.core.running());   // nothing stopped for that

    // The guard: S changes, so the host is asked to reset.
    changed.guardFrames = 48;
    EXPECT_TRUE(rig.core.settingsChanged(changed) == DriverCore::SettingsApplied::WhenTheHostResets);
    EXPECT_EQ(rig.host.count(kAsioResetRequest), std::size_t(1));
    EXPECT_CONTAINS(log.find("asking the host to reset"), "settings changed (Guard)");
    rig.core.stop();
}

TEST_CASE(DriverCore, TheBufferSizeOfferedChangesAtOnce)
{
    // A host that asks for the sizes again after a reset request, without
    // opening the driver again, must get the new one (Resolume got the old
    // one, so it lagged a change behind).
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings settings = aesInput();
    ASSERT_TRUE(rig.core.init(settings));
    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;

    // Before createBuffers: the next sizes asked for.
    DriverSettings changed = settings;
    changed.bufferFrames = 64;
    EXPECT_TRUE(rig.core.settingsChanged(changed) == DriverCore::SettingsApplied::NextOpen);
    ASSERT_EQ(rig.core.getBufferSize(&minSize, &maxSize, &preferred, &granularity), ASE_OK);
    EXPECT_EQ(preferred, 64L);
    long in = 0, out = 0;
    ASSERT_EQ(rig.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(in, 64L);   // the latencies the host is told go with it

    // Running: offered at once, while what runs keeps its size until the
    // host resets.
    ASSERT_EQ(rig.prepare(64), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(10));
    changed.bufferFrames = 256;
    EXPECT_TRUE(rig.core.settingsChanged(changed) == DriverCore::SettingsApplied::WhenTheHostResets);
    EXPECT_EQ(rig.host.count(kAsioResetRequest), std::size_t(1));
    ASSERT_EQ(rig.core.getBufferSize(&minSize, &maxSize, &preferred, &granularity), ASE_OK);
    EXPECT_EQ(preferred, 256L);
    ASSERT_EQ(rig.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(in, 64L);   // still running at 64
    EXPECT_CONTAINS(log.find("offered first: 256"), "buffer size offered first: 256 frames from now on");
    EXPECT_TRUE(rig.core.settingsChanged(changed) == DriverCore::SettingsApplied::Nothing);
    rig.core.stop();

    // The host takes it without opening the driver again.
    ASSERT_EQ(rig.core.disposeBuffers(), ASE_OK);
    ASSERT_EQ(rig.prepare(256), ASE_OK);
    ASSERT_EQ(rig.core.getLatencies(&in, &out), ASE_OK);
    EXPECT_EQ(in, 256L);
}

TEST_CASE(SettingsChange, SaysWhatChanged)
{
    DriverSettings a;
    DriverSettings b = a;
    EXPECT_FALSE(compareSettings(a, b).any);
    b.outputLatencyFrames = 12;
    SettingsChange c = compareSettings(a, b);
    EXPECT_TRUE(c.any && c.latencies && !c.needsReset && !c.keepRouting);
    EXPECT_EQ(c.what, std::string("OutputLatency"));
    b.bufferFrames = 256;
    b.input = InputChoice::Aes;
    c = compareSettings(a, b);
    EXPECT_TRUE(c.needsReset);
    EXPECT_EQ(c.what, std::string("Input, BufferSize, OutputLatency"));

    DriverSettings keep = a;
    keep.keepRouting = true;
    c = compareSettings(a, keep);
    EXPECT_TRUE(c.any && c.keepRouting && !c.needsReset && !c.latencies);
    EXPECT_EQ(c.what, std::string("KeepRouting"));

    // Which SDI input only counts when the input is SDI.
    DriverSettings sdi = a;
    sdi.sdiInput = 2;
    EXPECT_FALSE(compareSettings(a, sdi).any);
    DriverSettings sdi1 = a;
    sdi1.input = InputChoice::Sdi;
    sdi.input = InputChoice::Sdi;
    c = compareSettings(sdi1, sdi);
    EXPECT_TRUE(c.needsReset);
    EXPECT_EQ(c.what, std::string("SdiInput"));

    DriverSettings oem = a;
    oem.oemTasks = !a.oemTasks;
    c = compareSettings(a, oem);
    EXPECT_TRUE(c.needsReset);
    EXPECT_EQ(c.what, std::string("OemTaskMode"));

    // The SDI outputs change the outputs' number and names.
    DriverSettings outputs = a;
    outputs.sdiOutputs = 2;
    outputs.hdmiOutput = true;
    c = compareSettings(a, outputs);
    EXPECT_TRUE(c.needsReset);
    EXPECT_EQ(c.what, std::string("SdiOutputs, HdmiOutput"));

    DriverSettings level = a;
    level.analogLevelDbu = 18;
    c = compareSettings(a, level);
    EXPECT_TRUE(c.any && c.analogLevel && !c.needsReset && !c.latencies && !c.keepRouting);
    EXPECT_EQ(c.what, std::string("AnalogLevel"));
}

// ---------------------------------------------------------------------------
// The routing watch
// ---------------------------------------------------------------------------

namespace
{

/// Another program sets `parts` of the card's routing, changed by `change`.
template <typename Change>
bool anotherProgram(FakeCardAudio& card, unsigned parts, Change change)
{
    device::CardRouting theirs = card.routing();
    change(theirs);
    return card.anotherProgramSets(theirs, parts);
}

bool silentAtTheEnd(const std::vector<std::int32_t>& heard, std::size_t samples)
{
    if (heard.size() < samples)
        return false;
    for (std::size_t i = heard.size() - samples; i < heard.size(); ++i)
    {
        if (heard[i] != 0)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE(DriverCore, ReadsItsRoutingBackAtStart)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    EXPECT_FALSE(rig.core.routingStatus().watched);   // not before it runs
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_CONTAINS(log.find("routing: input source"),
                    "routing: input source: analog; Xpand input select: 1 (analog); Xpand analog level: +24 dBu; "
                    "AES out 1-4: audio system 2, channels 1-4; AES out 5-8: audio system 2, channels 5-8; monitor: "
                    "audio system 2, channels 1-2; task mode: OEM; read back once a second, Keep my routing "
                    "off");   // OEM task mode by default since 0.1.17 (Q9)
    ASSERT_TRUE(rig.core.runBlocks(800));   // two checks, nothing changed
    const DriverCore::RoutingStatus s = rig.core.routingStatus();
    EXPECT_TRUE(s.watched);
    EXPECT_FALSE(s.keep);
    EXPECT_EQ(s.changes, std::uint64_t(0));
    EXPECT_EQ(s.notOurs, 0u);
    EXPECT_TRUE(s.taskMode == device::TaskMode::Oem);
    EXPECT_EQ(s.analogLevelDbu, 24);
    rig.core.stop();
    EXPECT_EQ(log.count("routing: "), std::size_t(1));   // the start line only
    EXPECT_EQ(log.count("stop: routing"), std::size_t(0));
}

TEST_CASE(DriverCore, AnotherProgramTakesTheAesOutputs)
{
    // AES out 1-4 go to audio system 1 some seconds into the run (past the
    // first two looks, which after a start from standard mode put back
    // whatever AJA's services may still have written): the next check says
    // so, once, and the host hears nothing on AES in 1 from then.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(900));
    FakeCardAudio& card = *rig.opener.card();
    ASSERT_TRUE(anotherProgram(card, device::routing::kAesGroup, [](device::CardRouting& r) { r.aes[0] = {1, 1}; }));
    ASSERT_TRUE(rig.core.runBlocks(800));
    EXPECT_EQ(log.count("routing: another program changed AES out 1-4"), std::size_t(1));
    EXPECT_CONTAINS(log.find("routing: another program changed AES out 1-4"),
                    "to audio system 1, channels 1-4; the driver's is audio system 2, channels 1-4; left as it is "
                    "(Keep my routing is off)");
    const DriverCore::RoutingStatus s = rig.core.routingStatus();
    EXPECT_EQ(s.changes, std::uint64_t(1));
    EXPECT_EQ(s.putBack, std::uint64_t(0));
    EXPECT_EQ(s.notOurs, device::routing::kAesGroup);
    EXPECT_TRUE(silentAtTheEnd(rig.host.heard, 1000));
    rig.core.stop();
    EXPECT_CONTAINS(log.find("stop: routing"),
                    "changes by other programs 1, parts put back 0; not the driver's at the end: AES out 1-4");
}

TEST_CASE(DriverCore, KeepMyRoutingPutsItBack)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings settings = aesInput();
    settings.keepRouting = true;
    ASSERT_TRUE(rig.core.init(settings));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_CONTAINS(log.find("routing: input source"), "Keep my routing on");
    ASSERT_TRUE(rig.core.runBlocks(100));
    FakeCardAudio& card = *rig.opener.card();
    ASSERT_TRUE(anotherProgram(card, device::routing::kAesGroup, [](device::CardRouting& r) { r.aes[0] = {1, 1}; }));
    ASSERT_TRUE(rig.core.runBlocks(800));
    EXPECT_TRUE(card.aesOutputsRouted());
    EXPECT_CONTAINS(log.find("routing: another program changed AES out 1-4"), "; put back");
    const DriverCore::RoutingStatus s = rig.core.routingStatus();
    EXPECT_TRUE(s.keep);
    EXPECT_EQ(s.changes, std::uint64_t(1));
    EXPECT_EQ(s.putBack, std::uint64_t(1));
    EXPECT_EQ(s.notOurs, 0u);
    // The host hears itself again, with the round trip it had.
    const std::vector<std::int64_t> trips = rig.host.roundTrips();
    ASSERT_TRUE(!trips.empty());
    EXPECT_EQ(changes(trips), std::size_t(0));
    EXPECT_FALSE(silentAtTheEnd(rig.host.heard, 1000));
    rig.core.stop();
}

TEST_CASE(DriverCore, KeepMyRoutingAppliesAtOnce)
{
    // The analog input; another program records AES on audio system 1, and
    // the select goes to AES with it. Turned on, Keep my routing puts it
    // back at the next check, with no reset.
    CaptureLog log;
    Rig rig(FakeCardModel::withAnalogLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(900));   // past the first two looks after a start from standard mode
    FakeCardAudio& card = *rig.opener.card();
    ASSERT_TRUE(anotherProgram(card, device::routing::kXpandSelect, [](device::CardRouting& r) { r.xpandSelect = 0; }));
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_CONTAINS(log.find("routing: another program changed Xpand input select"),
                    "to 0 (AES); the driver's is 1 (analog): the analog inputs are off; left as it is");
    EXPECT_FALSE(card.analogSelected());

    DriverSettings keep;
    keep.keepRouting = true;
    EXPECT_TRUE(rig.core.settingsChanged(keep) == DriverCore::SettingsApplied::Now);
    EXPECT_EQ(rig.host.count(kAsioResetRequest), std::size_t(0));
    EXPECT_EQ(rig.host.count(kAsioLatenciesChanged), std::size_t(0));
    EXPECT_CONTAINS(log.find("Keep my routing is now"), "on");
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_TRUE(card.analogSelected());
    EXPECT_CONTAINS(log.find("routing: Xpand input select put back"), "1 (analog)");
    EXPECT_EQ(rig.core.routingStatus().putBack, std::uint64_t(1));
    EXPECT_EQ(rig.core.routingStatus().notOurs, 0u);
    rig.core.stop();
}

TEST_CASE(DriverCore, TheTaskModeIsReportedNotChanged)
{
    // With OEM task mode turned off.
    CaptureLog log;
    DriverSettings settings = aesInput();
    settings.keepRouting = true;
    settings.oemTasks = false;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(settings));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(100));
    rig.opener.card()->anotherProgramSetsTaskMode(device::TaskMode::Oem);
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_CONTAINS(log.find("routing: the card's task mode"), "is now OEM (it was standard)");
    const DriverCore::RoutingStatus s = rig.core.routingStatus();
    EXPECT_TRUE(s.taskMode == device::TaskMode::Oem);
    EXPECT_EQ(s.changes, std::uint64_t(0));
    EXPECT_EQ(s.putBack, std::uint64_t(0));
    EXPECT_TRUE(rig.opener.card()->routing().taskMode == device::TaskMode::Oem);
    rig.core.stop();
}

TEST_CASE(DriverCore, AFightOverTheRoutingIsNotLoggedLineByLine)
{
    // Another program sets AES out 1-4 again after every block while Keep my
    // routing puts them back once a second: two minutes, over a hundred
    // changes, the first 20 in the log and then one line in a hundred.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings settings = aesInput();
    settings.keepRouting = true;
    ASSERT_TRUE(rig.core.init(settings));
    ASSERT_EQ(rig.prepare(2048), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    FakeCardAudio& card = *rig.opener.card();
    rig.host.everyBlock = [&card](std::uint64_t) {
        anotherProgram(card, device::routing::kAesGroup, [](device::CardRouting& r) { r.aes[0] = {1, 1}; });
    };
    ASSERT_TRUE(rig.core.runBlocks(2813));   // 120 s at 2048 frames a block
    rig.host.everyBlock = nullptr;
    rig.core.stop();
    const DriverCore::RoutingStatus s = rig.core.routingStatus();
    // A check at the first block a second after the last: every 24 blocks
    // of 42.7 ms, 117 of them.
    EXPECT_TRUE(s.changes >= 115 && s.changes <= 120);
    EXPECT_EQ(s.putBack, s.changes);
    const std::size_t logged = log.count("routing: another program changed AES out 1-4");
    EXPECT_TRUE(logged >= 20 && logged <= 21);
    EXPECT_CONTAINS(log.find("stop: routing"), "changes by other programs " + std::to_string(s.changes));
}

TEST_CASE(DriverCore, TheLevelChosenIsSetAtStart)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAnalogLoop(), unthreaded(&log));
    DriverSettings settings;
    settings.analogLevelDbu = 18;
    ASSERT_TRUE(rig.core.init(settings));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_EQ(rig.opener.card()->routing().analogLevelDbu, 18);
    EXPECT_CONTAINS(log.find("routing: input source"), "Xpand analog level: +18 dBu");
    EXPECT_EQ(rig.core.routingStatus().analogLevelDbu, 18);
    // The analog loop still comes back: the level is the same both ways.
    ASSERT_TRUE(rig.core.runBlocks(200));
    double energy = 0.0;
    for (std::size_t i = rig.host.heard.size() / 2; i < rig.host.heard.size(); ++i)
        energy += std::fabs(static_cast<double>(rig.host.heard[i]));
    EXPECT_TRUE(energy > 1e9);
    rig.core.stop();
}

TEST_CASE(DriverCore, TheLevelChangesAtOnceWhileRunning)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAnalogLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(900));   // past the first two looks after a start from standard mode
    FakeCardAudio& card = *rig.opener.card();
    EXPECT_EQ(card.routing().analogLevelDbu, 24);   // as the card had it

    // Chosen in the panel: set after the next block, no reset, nothing told.
    DriverSettings level;
    level.analogLevelDbu = 15;
    EXPECT_TRUE(rig.core.settingsChanged(level) == DriverCore::SettingsApplied::Now);
    EXPECT_EQ(card.routing().analogLevelDbu, 24);
    ASSERT_TRUE(rig.core.runBlocks(1));
    EXPECT_EQ(card.routing().analogLevelDbu, 15);
    EXPECT_EQ(log.count("analog level set to +15 dBu"), std::size_t(1));
    EXPECT_EQ(rig.host.count(kAsioResetRequest), std::size_t(0));
    EXPECT_EQ(rig.core.routingStatus().analogLevelDbu, 15);

    // Now the driver's: another program's change is one of the driver's parts.
    ASSERT_TRUE(anotherProgram(card, device::routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 18; }));
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_CONTAINS(log.find("routing: another program changed Xpand analog level"),
                    "to +18 dBu; the driver's is +15 dBu; left as it is");
    EXPECT_EQ(rig.core.routingStatus().changes, std::uint64_t(1));

    // Back to "as the card has it": no longer the driver's, only reported.
    level.analogLevelDbu = 0;
    EXPECT_TRUE(rig.core.settingsChanged(level) == DriverCore::SettingsApplied::Now);
    ASSERT_TRUE(rig.core.runBlocks(1));
    EXPECT_EQ(log.count("analog level: left as the card has it from now on"), std::size_t(1));
    ASSERT_TRUE(anotherProgram(card, device::routing::kAnalogLevel, [](device::CardRouting& r) { r.analogLevelDbu = 12; }));
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_CONTAINS(log.find("routing: Xpand analog level is now"), "+12 dBu (it was +18 dBu)");
    EXPECT_EQ(rig.core.routingStatus().changes, std::uint64_t(1));
    EXPECT_EQ(card.routing().analogLevelDbu, 12);
    rig.core.stop();
}

TEST_CASE(DriverCore, ALevelChosenWhileStoppedWaitsForTheStart)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAnalogLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(DriverSettings{}));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    DriverSettings level;
    level.analogLevelDbu = 12;
    EXPECT_TRUE(rig.core.settingsChanged(level) == DriverCore::SettingsApplied::Now);
    EXPECT_EQ(log.count("analog level: +12 dBu from the next start"), std::size_t(1));
    EXPECT_EQ(rig.opener.card()->routing().analogLevelDbu, 24);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_EQ(rig.opener.card()->routing().analogLevelDbu, 12);
    rig.core.stop();
}

// ---------------------------------------------------------------------------
// The embedded inputs
// ---------------------------------------------------------------------------

namespace
{

DriverSettings sdiInput(unsigned n)
{
    DriverSettings s;
    s.input = InputChoice::Sdi;
    s.sdiInput = n;
    return s;
}

/// The mean absolute value of input `channel` over the second half of what
/// the host heard (TestHost keeps channel 0 only, so the rig records them all).
double level(const std::vector<std::vector<std::int32_t>>& heard, std::size_t channel)
{
    const std::vector<std::int32_t>& samples = heard[channel];
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = samples.size() / 2; i < samples.size(); ++i, ++n)
        sum += std::fabs(static_cast<double>(samples[i]));
    return n == 0 ? 0.0 : sum / static_cast<double>(n);
}

/// Every input of the rig, block after block. With `play`, every output
/// plays the value it gives for its channel.
std::vector<std::vector<std::int32_t>> runAndRecord(Rig& rig, unsigned blocks,
                                                    std::function<std::int32_t(long)> play = nullptr)
{
    long inputs = 0, outputs = 0;
    rig.core.getChannels(&inputs, &outputs);
    std::vector<std::vector<std::int32_t>> heard(static_cast<std::size_t>(inputs));
    rig.host.everyBlock = [&rig, &heard, play](std::uint64_t block) {
        const long half = static_cast<long>(block & 1u);
        for (const ASIOBufferInfo& b : rig.host.infos)
        {
            if (!b.isInput)
            {
                auto* samples = static_cast<std::int32_t*>(b.buffers[half]);
                for (unsigned f = 0; play && f < rig.host.frames; ++f)
                    samples[f] = play(b.channelNum);
                continue;
            }
            const auto* samples = static_cast<const std::int32_t*>(b.buffers[half]);
            auto& channel = heard[static_cast<std::size_t>(b.channelNum)];
            channel.insert(channel.end(), samples, samples + rig.host.frames);
        }
    };
    rig.core.runBlocks(blocks);
    rig.host.everyBlock = nullptr;
    return heard;
}

} // namespace

TEST_CASE(DriverCore, AnSdiInputGivesSixteenChannelsOnTheVideoInputsClock)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(sdiInput(2)));
    long inputs = 0, outputs = 0;
    ASSERT_EQ(rig.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(inputs, 16L);
    EXPECT_EQ(outputs, 8L);
    ASIOChannelInfo info{};
    info.isInput = ASIOTrue;
    info.channel = 0;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("SDI 2 ch 1"));
    info.channel = 15;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("SDI 2 ch 16"));
    ASIOClockSource clocks[2]{};
    long count = 2;
    ASSERT_EQ(rig.core.getClockSources(clocks, &count), ASE_OK);
    EXPECT_EQ(std::string(clocks[0].name), std::string("Video input: SDI 2"));
    EXPECT_CONTAINS(log.find("ready: "), "16 inputs, 8 outputs, clock Video input: SDI 2");

    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    FakeCardAudio& card = *rig.opener.card();
    EXPECT_TRUE(card.routing().source == device::InputSource::Embedded);
    EXPECT_EQ(card.routing().embeddedInput, 1u);   // SDI 2
    EXPECT_EQ(card.xpandSelect(), 0u);              // left as it was, for the Xpand's own users
    EXPECT_CONTAINS(log.find("routing: input source"), "input source: SDI 2 embedded");

    // Each embedded channel carries a tone of its own.
    const std::vector<std::vector<std::int32_t>> heard = runAndRecord(rig, 200);
    for (std::size_t c = 0; c < 16; ++c)
        EXPECT_TRUE(level(heard, c) > 1e7);   // -30 dBFS is about 4e7 on average
    EXPECT_TRUE(heard[0] != heard[15]);
    rig.core.stop();
}

TEST_CASE(DriverCore, TheHdmiInputGivesEightChannels)
{
    Rig rig;
    DriverSettings s;
    s.input = InputChoice::Hdmi;
    ASSERT_TRUE(rig.core.init(s));
    long inputs = 0, outputs = 0;
    ASSERT_EQ(rig.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(inputs, 8L);
    ASIOChannelInfo info{};
    info.isInput = ASIOTrue;
    info.channel = 7;
    ASSERT_EQ(rig.core.getChannelInfo(&info), ASE_OK);
    EXPECT_EQ(std::string(info.name), std::string("HDMI ch 8"));
    ASSERT_EQ(rig.prepare(256), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_TRUE(rig.opener.card()->routing().source == device::InputSource::Hdmi);
    const std::vector<std::vector<std::int32_t>> heard = runAndRecord(rig, 100);
    EXPECT_TRUE(level(heard, 7) > 1e7);
    rig.core.stop();
}

TEST_CASE(DriverCore, AnInputTheCardDoesNotHaveIsRefused)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    EXPECT_FALSE(rig.core.init(sdiInput(3)));
    EXPECT_EQ(rig.core.errorMessage(), std::string("The card has 2 SDI inputs; the settings ask for SDI 3."));
    EXPECT_FALSE(rig.core.initialized());

    FakeCardModel noHdmi = FakeCardModel::withAesLoop();
    noHdmi.hdmiInputs = 0;
    Rig bare(noHdmi);
    DriverSettings s;
    s.input = InputChoice::Hdmi;
    EXPECT_FALSE(bare.core.init(s));
    EXPECT_CONTAINS(bare.core.errorMessage(), "no HDMI input");
}

TEST_CASE(DriverCore, WithoutAVideoSignalTheEmbeddedInputIsSilent)
{
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.videoSignal = false;
    Rig rig(model);
    ASSERT_TRUE(rig.core.init(sdiInput(1)));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    const std::vector<std::vector<std::int32_t>> heard = runAndRecord(rig, 100);
    EXPECT_TRUE(level(heard, 0) == 0.0);
    rig.core.stop();
}

TEST_CASE(DriverCore, AnEmbeddedInputWhoseClockStopsGetsSilenceThenAResync)
{
    // If the audio clock stops with the video signal (Q7), the host keeps
    // getting blocks, of silence, and a resync when it comes back.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(sdiInput(1)));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(50));
    rig.opener.card()->stallAudioClock(rig.clock.nowNs(), 300 * kMs);
    ASSERT_TRUE(rig.core.runBlocks(300));
    EXPECT_EQ(log.count("input stalled at "), std::size_t(1));
    EXPECT_EQ(log.count("input resumed at "), std::size_t(1));
    EXPECT_EQ(rig.host.count(kAsioResyncRequest), std::size_t(1));
    EXPECT_TRUE(rig.core.stats().silentBlocks > 50);
    rig.core.stop();
}

TEST_CASE(DriverCore, WithAnEmbeddedInputTheXpandsSelectIsOnlyReported)
{
    // Its inputs are not the driver's then: another program may switch them,
    // and Keep my routing leaves that alone.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings s = sdiInput(1);
    s.keepRouting = true;
    ASSERT_TRUE(rig.core.init(s));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(100));
    FakeCardAudio& card = *rig.opener.card();
    ASSERT_TRUE(anotherProgram(card, device::routing::kXpandSelect, [](device::CardRouting& r) { r.xpandSelect = 1; }));
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_CONTAINS(log.find("routing: Xpand input select is now"), "1 (analog) (it was 0 (AES))");
    EXPECT_EQ(card.xpandSelect(), 1u);
    EXPECT_EQ(rig.core.routingStatus().changes, std::uint64_t(0));
    rig.core.stop();
}

// ---------------------------------------------------------------------------
// OEM task mode
// ---------------------------------------------------------------------------

TEST_CASE(DriverCore, OemTaskModeIsSetAndReasserted)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings s = aesInput();
    s.oemTasks = true;
    ASSERT_TRUE(rig.core.init(s));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    FakeCardAudio& card = *rig.opener.card();
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    EXPECT_CONTAINS(log.find("task mode: OEM while the driver plays"), "(it was standard)");
    EXPECT_CONTAINS(log.find("routing: input source"), "task mode: OEM");
    ASSERT_TRUE(rig.core.runBlocks(100));
    // The AJA Control Panel puts it back to standard: reasserted, Keep my
    // routing or not.
    card.anotherProgramSetsTaskMode(device::TaskMode::Standard);
    ASSERT_TRUE(rig.core.runBlocks(400));
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    EXPECT_CONTAINS(log.find("routing: another program changed task mode"),
                    "to standard; the driver's is OEM; put back");
    rig.core.stop();
}

TEST_CASE(DriverCore, AfterAMomentInStandardModeTheOutputsComeBack)
{
    // The defaults: OEM task mode on, Keep my routing off. A program that
    // had the card lets it go and puts it back in standard mode (AJA Input
    // does when its last instance closes, which a change of deck can be);
    // AJA's services move AES out 1-8 and the monitor to their mixer at
    // once (Q9). The driver takes OEM mode back at its next look, and its
    // outputs with it: the host hears itself again without a restart.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(100));
    FakeCardAudio& card = *rig.opener.card();
    ASSERT_TRUE(card.aesOutputsRouted());

    card.anotherProgramSetsTaskMode(device::TaskMode::Standard);
    ASSERT_TRUE(anotherProgram(card,
                               device::routing::kAesGroup | (device::routing::kAesGroup << 1) |
                                   device::routing::kMonitor,
                               [](device::CardRouting& r) {
                                   r.aes[0] = {4, 1};
                                   r.aes[1] = {4, 5};
                                   r.monitor = {4, 1};
                               }));
    ASSERT_TRUE(!card.aesOutputsRouted());
    ASSERT_TRUE(rig.core.runBlocks(800));   // two seconds: a look in between
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    EXPECT_TRUE(card.aesOutputsRouted());
    EXPECT_TRUE(card.monitorRouted());
    EXPECT_CONTAINS(log.find("routing: another program changed task mode"),
                    "to standard; the driver's is OEM; put back");
    EXPECT_CONTAINS(log.find("routing: another program changed AES out 1-4"),
                    "to audio system 4, channels 1-4; the driver's is audio system 2, channels 1-4; put back");
    EXPECT_CONTAINS(log.find("routing: another program changed monitor"), "; put back");
    const DriverCore::RoutingStatus s = rig.core.routingStatus();
    EXPECT_FALSE(s.keep);
    EXPECT_EQ(s.notOurs, 0u);
    EXPECT_FALSE(silentAtTheEnd(rig.host.heard, 1000));
    rig.core.stop();
}

TEST_CASE(DriverCore, WhatTheServicesWriteRightAfterTheStartGoesBack)
{
    // The defaults again, the card in standard mode before the driver (no
    // Resolume holding it: vMix, duplex-asiotest). The driver takes it to
    // OEM mode at its first start; AJA's services, in the middle of a pass
    // just then, write AES out 1-8 and the monitor once more after the
    // driver did. Its first look puts them back: a second without sound at
    // most, and the log says what happened, instead of silence until the
    // host starts the audio again.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_CONTAINS(log.find("task mode: OEM while the driver plays"), "(it was standard)");
    FakeCardAudio& card = *rig.opener.card();
    ASSERT_TRUE(card.aesOutputsRouted());
    ASSERT_TRUE(rig.core.runBlocks(4));   // some 10 ms later
    const unsigned outputs =
        device::routing::kAesGroup | (device::routing::kAesGroup << 1) | device::routing::kMonitor;
    ASSERT_TRUE(anotherProgram(card, outputs, [](device::CardRouting& r) {
        r.aes[0] = {4, 1};
        r.aes[1] = {4, 5};
        r.monitor = {4, 1};
    }));
    ASSERT_TRUE(!card.aesOutputsRouted());
    ASSERT_TRUE(rig.core.runBlocks(800));   // two seconds: the first look in between
    EXPECT_TRUE(card.aesOutputsRouted());
    EXPECT_TRUE(card.monitorRouted());
    EXPECT_CONTAINS(log.find("routing: another program changed AES out 1-4"),
                    "to audio system 4, channels 1-4; the driver's is audio system 2, channels 1-4; put back");
    EXPECT_EQ(rig.core.routingStatus().notOurs, 0u);
    EXPECT_FALSE(silentAtTheEnd(rig.host.heard, 1000));

    // A second start finds the driver's own OEM mode: the services have
    // been off all along, and a change then is another program's.
    rig.core.stop();
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(4));
    ASSERT_TRUE(anotherProgram(card, device::routing::kAesGroup, [](device::CardRouting& r) { r.aes[0] = {4, 1}; }));
    ASSERT_TRUE(rig.core.runBlocks(800));
    EXPECT_TRUE(!card.aesOutputsRouted());
    EXPECT_EQ(rig.core.routingStatus().notOurs, device::routing::kAesGroup);
    rig.core.stop();
}

TEST_CASE(DriverCore, OtherProgramsAreToldWhileTheTaskModeIsTheDrivers)
{
    // What the Windows driver turns into a named mark (win/InstanceLock.h,
    // TaskModeMark) for another program on the card, AJA Input, to leave
    // the task mode to the driver: told at the first start with OEM task
    // mode on, and again when the card is let go.
    std::vector<std::pair<bool, unsigned>> told;
    DriverOptions options = unthreaded();
    options.taskModeHeld = [&told](bool held, unsigned card) { told.emplace_back(held, card); };
    {
        Rig rig(FakeCardModel::withAesLoop(), options);
        DriverSettings s = aesInput();
        s.card = 3;
        ASSERT_TRUE(rig.core.init(s));
        EXPECT_EQ(told.size(), std::size_t(0));   // nothing changed on the card yet
        ASSERT_EQ(rig.prepare(128), ASE_OK);
        ASSERT_EQ(rig.core.start(), ASE_OK);
        ASSERT_EQ(told.size(), std::size_t(1));
        EXPECT_TRUE(told[0].first);
        EXPECT_EQ(told[0].second, 3u);
        ASSERT_TRUE(rig.core.runBlocks(10));
        rig.core.stop();
        ASSERT_EQ(rig.core.start(), ASE_OK);   // a second start: said once
        rig.core.stop();
        EXPECT_EQ(told.size(), std::size_t(1));
        // The host opens the driver again: the old card is let go.
        ASSERT_TRUE(rig.core.init(aesInput()));
        ASSERT_EQ(told.size(), std::size_t(2));
        EXPECT_FALSE(told[1].first);
        EXPECT_EQ(told[1].second, 3u);
    }
    EXPECT_EQ(told.size(), std::size_t(2));   // opened again but never started: nothing more to say

    // With OEM task mode off the driver only reads the task mode.
    told.clear();
    {
        Rig rig(FakeCardModel::withAesLoop(), options);
        DriverSettings s = aesInput();
        s.oemTasks = false;
        ASSERT_TRUE(rig.core.init(s));
        ASSERT_EQ(rig.prepare(128), ASE_OK);
        ASSERT_EQ(rig.core.start(), ASE_OK);
        rig.core.stop();
    }
    EXPECT_EQ(told.size(), std::size_t(0));

    // A driver the host lets go lets the card go as a new init does.
    {
        Rig rig(FakeCardModel::withAesLoop(), options);
        ASSERT_TRUE(rig.core.init(aesInput()));
        ASSERT_EQ(rig.prepare(128), ASE_OK);
        ASSERT_EQ(rig.core.start(), ASE_OK);
        rig.core.stop();
        EXPECT_EQ(told.size(), std::size_t(1));
    }
    ASSERT_EQ(told.size(), std::size_t(2));
    EXPECT_FALSE(told[1].first);
}

TEST_CASE(DriverCore, ReleaseLogsWhatItLeft)
{
    // OEM mode set by the driver, the card reserved by another program at
    // the end: the task mode stays, and the log says why.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings s = aesInput();
    s.oemTasks = true;
    ASSERT_TRUE(rig.core.init(s));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    ASSERT_TRUE(rig.core.runBlocks(10));
    rig.core.stop();
    rig.opener.card()->anotherProgramHoldsTheCard(true);
    ASSERT_TRUE(rig.core.init(aesInput()));   // the host opens it again: the old card is let go
    EXPECT_CONTAINS(log.find("release: "), "the task mode left as it is: AJIN, process 4242, holds the card");
}

TEST_CASE(DriverCore, AnOemModeThatWasAnotherProgramsIsNotPutBack)
{
    // AJA Input is up first: it reserved the card and set OEM mode, and
    // will put back what it found, standard, when it closes. The driver
    // starts, finds OEM and holds it. AJA Input closes while the driver
    // plays (standard again, the card let go): the driver takes OEM mode
    // back. When the driver lets the card go nobody holds it, and putting
    // back the OEM mode it found would leave the card in it for good, with
    // AJA's services off: it goes to standard, and the log says so.
    CaptureLog log;
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.taskMode = device::TaskMode::Oem;
    model.heldAtStart = true;
    Rig rig(model, unthreaded(&log));
    ASSERT_TRUE(rig.core.init(aesInput()));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_CONTAINS(log.find("task mode: OEM while the driver plays"), "(it was OEM)");
    ASSERT_TRUE(rig.core.runBlocks(100));
    FakeCardAudio& card = *rig.opener.card();
    card.anotherProgramHoldsTheCard(false);
    card.anotherProgramSetsTaskMode(device::TaskMode::Standard);
    ASSERT_TRUE(rig.core.runBlocks(800));
    EXPECT_TRUE(card.routing().taskMode == device::TaskMode::Oem);
    rig.core.stop();
    ASSERT_TRUE(rig.core.init(aesInput()));   // the host opens it again: the old card is let go
    EXPECT_CONTAINS(log.find("release: "), "the task mode put to standard, not back to OEM: AJIN, process 4242, "
                                           "which held the card at the start, holds it no longer");
}

TEST_CASE(DriverCore, WithoutOemModeTheTaskModeIsNotTouched)
{
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings settings = aesInput();
    settings.oemTasks = false;
    ASSERT_TRUE(rig.core.init(settings));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    EXPECT_TRUE(rig.opener.card()->routing().taskMode == device::TaskMode::Standard);
    EXPECT_EQ(log.count("task mode: OEM"), std::size_t(0));
    rig.core.stop();
}

// ---------------------------------------------------------------------------
// The SDI and HDMI outputs
// ---------------------------------------------------------------------------

namespace
{

std::string outputName(DriverCore& core, long channel)
{
    ASIOChannelInfo info{};
    info.isInput = ASIOFalse;
    info.channel = channel;
    return core.getChannelInfo(&info) == ASE_OK ? std::string(info.name) : std::string("?");
}

/// What output `channel` plays in the loop test: its number in the top byte.
std::int32_t channelMark(long channel)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(channel + 1) << 24 | 0x5500u);
}

} // namespace

TEST_CASE(DriverCore, AnSdiOutputAddsOutputsNineToSixteen)
{
    CaptureLog log;
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.sdiSending = 0x2;   // SDI 2 is an output
    Rig rig(model, unthreaded(&log));
    DriverSettings s = aesInput();
    s.sdiOutputs = 2;
    ASSERT_TRUE(rig.core.init(s));
    long inputs = 0, outputs = 0;
    ASSERT_EQ(rig.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(outputs, 16L);
    EXPECT_EQ(outputName(rig.core, 0), std::string("AES 1 + Analog L + SDI"));
    EXPECT_EQ(outputName(rig.core, 7), std::string("AES 8 + SDI"));
    EXPECT_EQ(outputName(rig.core, 8), std::string("SDI 9"));
    EXPECT_EQ(outputName(rig.core, 15), std::string("SDI 16"));
    EXPECT_TRUE(rig.core.sdiNote().empty());
    EXPECT_CONTAINS(log.find("ready: "), "8 inputs, 16 outputs");

    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    FakeCardAudio& card = *rig.opener.card();
    EXPECT_TRUE(card.routing().sdiOut[1] == (device::SdiOutputAudio{2, true}));
    EXPECT_TRUE(card.routing().sdiOut[0] == (device::SdiOutputAudio{1, true}));   // not asked for: as it was
    EXPECT_CONTAINS(log.find("outputs: "), "AES 1-8, analog out, SDI out 2; the audio goes into whatever video");
    EXPECT_CONTAINS(log.find("routing: input source"), "SDI out 2: audio system 2; task mode");
    // The AES loop still brings output 1 back.
    rig.core.runBlocks(40);
    EXPECT_TRUE(!rig.host.roundTrips().empty());
    rig.core.stop();
    rig.core.disposeBuffers();
}

TEST_CASE(DriverCore, TheHdmiOutputPlaysOutputsOneToEight)
{
    FakeCardModel model = FakeCardModel::withAesLoop();
    Rig rig(model);
    DriverSettings s = aesInput();
    s.hdmiOutput = true;
    s.sdiOutputs = 1;
    ASSERT_TRUE(rig.core.init(s));
    long inputs = 0, outputs = 0;
    ASSERT_EQ(rig.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(outputs, 16L);
    EXPECT_EQ(outputName(rig.core, 0), std::string("AES 1 + Analog L + SDI + HDMI"));   // 29 characters
    EXPECT_EQ(outputName(rig.core, 2), std::string("AES 3 + SDI + HDMI"));
    EXPECT_EQ(outputName(rig.core, 9), std::string("SDI 10"));

    DriverSettings hdmiOnly = aesInput();
    hdmiOnly.hdmiOutput = true;
    Rig only;
    ASSERT_TRUE(only.core.init(hdmiOnly));
    ASSERT_EQ(only.core.getChannels(&inputs, &outputs), ASE_OK);
    EXPECT_EQ(outputs, 8L);   // HDMI carries 1-8
    EXPECT_EQ(outputName(only.core, 1), std::string("AES 2 + Analog R + HDMI"));
    ASSERT_EQ(only.prepare(128), ASE_OK);
    ASSERT_EQ(only.core.start(), ASE_OK);
    EXPECT_TRUE(only.opener.card()->routing().hdmiOut == (device::HdmiOutputAudio{2, 8, 1}));
    only.core.stop();
}

TEST_CASE(DriverCore, AnSdiLoopBringsEverySixteenOutputsBack)
{
    // SDI out 2 cabled to SDI in 1, the way phase 0 can try it on the KONA X.
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.sdiSending = 0x2;
    model.sdiLoop.looped = true;
    model.sdiLoop.output = 1;
    model.sdiLoop.input = 0;
    Rig rig(model);
    DriverSettings s = sdiInput(1);
    s.sdiOutputs = 2;
    ASSERT_TRUE(rig.core.init(s));
    EXPECT_TRUE(rig.core.sdiNote().empty());
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    const std::vector<std::vector<std::int32_t>> heard = runAndRecord(rig, 100, channelMark);
    rig.core.stop();
    ASSERT_EQ(heard.size(), std::size_t(16));
    for (std::size_t c = 0; c < 16; ++c)
    {
        // Each input hears its own output, and nothing else but silence.
        std::size_t own = 0, other = 0;
        for (const std::int32_t v : heard[c])
        {
            if (v == channelMark(static_cast<long>(c)))
                ++own;
            else if (v != 0)
                ++other;
        }
        EXPECT_TRUE(own > heard[c].size() / 2);
        EXPECT_EQ(other, std::size_t(0));
    }
}

TEST_CASE(DriverCore, WithTheSdiOutputOnAnotherAudioSystemTheLoopIsSilent)
{
    FakeCardModel model = FakeCardModel::withAesLoop();
    model.sdiSending = 0x2;
    model.sdiLoop.looped = true;
    Rig rig(model);
    DriverSettings s = sdiInput(1);
    s.sdiOutputs = 2;
    ASSERT_TRUE(rig.core.init(s));
    ASSERT_EQ(rig.prepare(128), ASE_OK);
    ASSERT_EQ(rig.core.start(), ASE_OK);
    // Another program points SDI out 2 back at audio system 1.
    ASSERT_TRUE(anotherProgram(*rig.opener.card(), device::routing::kSdiOut << 1,
                               [](device::CardRouting& r) { r.sdiOut[1] = {1, true}; }));
    const std::vector<std::vector<std::int32_t>> heard = runAndRecord(rig, 100, channelMark);
    rig.core.stop();
    EXPECT_TRUE(level(heard, 0) == 0.0);
    EXPECT_TRUE(level(heard, 15) == 0.0);
}

TEST_CASE(DriverCore, OutputsTheCardDoesNotHaveAreRefused)
{
    Rig rig;
    DriverSettings s = aesInput();
    s.sdiOutputs = 4;   // SDI out 3
    EXPECT_FALSE(rig.core.init(s));
    EXPECT_EQ(rig.core.errorMessage(), std::string("The card has 2 SDI outputs; the settings ask for SDI out 3."));

    FakeCardModel noHdmi = FakeCardModel::withAesLoop();
    noHdmi.hdmiOutputs = 0;
    Rig bare(noHdmi);
    DriverSettings h = aesInput();
    h.hdmiOutput = true;
    EXPECT_FALSE(bare.core.init(h));
    EXPECT_EQ(bare.core.errorMessage(), std::string("The card has no HDMI output to play to."));
}

TEST_CASE(DriverCore, TheSdiConnectorsWayIsSaid)
{
    // Both SDI connectors are inputs: SDI out 2 sends nothing.
    CaptureLog log;
    Rig rig(FakeCardModel::withAesLoop(), unthreaded(&log));
    DriverSettings s = aesInput();
    s.sdiOutputs = 2;
    ASSERT_TRUE(rig.core.init(s));
    EXPECT_EQ(rig.core.sdiNote(), std::string("SDI 2 is set as an input, so the driver's audio does not go out on "
                                              "it: set it as an output in the AJA Control Panel."));
    EXPECT_CONTAINS(log.find("sdi: "), "SDI 2 is set as an input");

    // SDI 1 sends: the input on it hears nothing.
    FakeCardModel sending = FakeCardModel::withAesLoop();
    sending.sdiSending = 0x1;
    Rig out(sending);
    ASSERT_TRUE(out.core.init(sdiInput(1)));
    EXPECT_EQ(out.core.sdiNote(), std::string("SDI 1 is set as an output, so the driver's input hears nothing: set "
                                              "it as an input in the AJA Control Panel."));
    ASSERT_EQ(out.prepare(128), ASE_OK);
    ASSERT_EQ(out.core.start(), ASE_OK);
    const std::vector<std::vector<std::int32_t>> heard = runAndRecord(out, 100);
    out.core.stop();
    EXPECT_TRUE(level(heard, 0) == 0.0);   // no tones: no signal

    // One connector cannot be both.
    Rig both;
    DriverSettings same = sdiInput(1);
    same.sdiOutputs = 1;
    ASSERT_TRUE(both.core.init(same));
    EXPECT_EQ(both.core.sdiNote(), std::string("SDI 1 cannot be the driver's input and one of its outputs: the "
                                               "connector is one or the other (it is set as an input now)."));

    // Cards whose connectors go one way have nothing to say.
    FakeCardModel oneWay = FakeCardModel::withAesLoop();
    oneWay.bidirectionalSdi = false;
    Rig fixed(oneWay);
    ASSERT_TRUE(fixed.core.init(same));
    EXPECT_TRUE(fixed.core.sdiNote().empty());
}

TEST_CASE(DriverCore, TheSimulatedCardLoopsAnSdiOutputToTheSdiInput)
{
    VirtualClock clock;
    SimulatedOpener opener(clock);
    DriverSettings s = sdiInput(1);
    s.sdiOutputs = 2;
    std::string error;
    std::unique_ptr<device::CardAudio> card = opener.open(s, error);
    ASSERT_TRUE(card != nullptr);
    auto* fake = static_cast<FakeCardAudio*>(card.get());
    EXPECT_TRUE(fake->model().sdiLoop.looped);
    EXPECT_EQ(fake->model().sdiLoop.output, 1u);
    EXPECT_EQ(fake->model().sdiLoop.input, 0u);
    EXPECT_EQ(fake->model().sdiSending, 2u);
    std::uint32_t in = 0, out = 0;
    ASSERT_TRUE(opener.hardwareLatencies(s, in, out));
    EXPECT_EQ(in, fake->model().sdiLoop.delayFrames);
}
