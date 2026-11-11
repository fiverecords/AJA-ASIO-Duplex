// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "device/CardInspector.h"

#include "common/TextFormat.h"

#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2registerexpert.h"
#include "ntv2utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <sstream>
#include <thread>

namespace ajaduplex::device
{

namespace
{

using text::hex32;
using text::megabytes;
using text::yesNo;

/// Shown when an SDK call fails or does not apply to the device.
const std::string kNA = "n/a";

template <typename T>
std::string num(T value)
{
    return std::to_string(value);
}

std::string addressText(ULWord address)
{
    return hex32(address) + " (" + megabytes(address) + ")";
}

std::string audioSystemName(NTV2AudioSystem system)
{
    return ::NTV2AudioSystemToString(system, true);
}

std::string audioLevelName(NTV2AudioLevel level)
{
    switch (level)
    {
        case NTV2_AUDIOLEVEL_24dBu: return "+24 dBu";
        case NTV2_AUDIOLEVEL_18dBu: return "+18 dBu";
        case NTV2_AUDIOLEVEL_12dBu: return "+12 dBu";
        case NTV2_AUDIOLEVEL_15dBu: return "+15 dBu";
        default:                    return "unknown (" + num(int(level)) + ")";
    }
}

std::string autoCirculateStateName(NTV2AutoCirculateState state)
{
    switch (state)
    {
        case NTV2_AUTOCIRCULATE_DISABLED:          return "disabled";
        case NTV2_AUTOCIRCULATE_INIT:              return "init";
        case NTV2_AUTOCIRCULATE_STARTING:          return "starting";
        case NTV2_AUTOCIRCULATE_PAUSED:            return "paused";
        case NTV2_AUTOCIRCULATE_STOPPING:          return "stopping";
        case NTV2_AUTOCIRCULATE_RUNNING:           return "running";
        case NTV2_AUTOCIRCULATE_STARTING_AT_TIME:  return "starting at time";
        default:                                   return "unknown (" + num(int(state)) + ")";
    }
}

std::string mixerInputName(NTV2AudioMixerInput input)
{
    switch (input)
    {
        case NTV2_AudioMixerInputMain: return "main";
        case NTV2_AudioMixerInputAux1: return "aux 1";
        case NTV2_AudioMixerInputAux2: return "aux 2";
        default:                       return "input " + num(int(input));
    }
}

/// Number of audio systems worth listing. On a device with an audio mixer
/// (the KONA X has one) the SDK counts one more buffered audio system than
/// GetNumAudioSystems: the mixer's, with its own buffer at the top of memory.
UWord audioSystemCount(CNTV2Card& card)
{
    return std::max(card.features().GetNumAudioSystems(), card.features().GetNumBufferedAudioSystems());
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void addIdentity(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("device");
    const NTV2DeviceID id = card.GetDeviceID();

    s.add("model", card.GetModelName());
    s.add("device id", ::NTV2DeviceIDToString(id) + " (" + hex32(ULWord(id)) + ")");
    std::string serial;
    s.add("serial", card.GetSerialNumberString(serial) ? serial : kNA);
    s.add("index", num(card.GetIndexNumber()));

    UWord major = 0, minor = 0, point = 0, build = 0;
    s.add("driver", card.GetDriverVersionComponents(major, minor, point, build)
                        ? num(major) + "." + num(minor) + "." + num(point) + "." + num(build)
                        : kNA);

    UWord fwRevision = 0;
    s.add("firmware revision", card.GetRunningFirmwareRevision(fwRevision) ? num(fwRevision) : kNA);
    ULWord fwPackage = 0;
    s.add("firmware package", card.GetRunningFirmwarePackageRevision(fwPackage) ? num(fwPackage) : kNA);
    std::string fwDate, fwTime;
    s.add("firmware date", card.GetRunningFirmwareDate(fwDate, fwTime) ? fwDate + " " + fwTime : kNA);

    s.add("breakout hardware", ::NTV2BreakoutTypeToString(card.GetBreakoutHardware(), true));
    s.add("breakout board attached", yesNo(card.features().HasBreakoutBoard()));
}

void addCapabilities(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("capabilities", /*detail*/ true);
    DeviceCapabilities& f = card.features();

    s.add("audio systems", num(f.GetNumAudioSystems()));
    s.add("buffered audio systems", num(f.GetNumBufferedAudioSystems()));
    s.add("max audio channels", num(f.GetMaxAudioChannels()));
    s.add("DMA engines", num(f.GetNumDMAEngines()));
    s.add("memory", megabytes(f.GetActiveMemorySize()));
    s.add("frame stores", num(f.GetNumFrameStores()));
    s.add("video inputs / outputs", num(f.GetNumVideoInputs()) + " / " + num(f.GetNumVideoOutputs()));
    s.add("HDMI inputs / outputs", num(f.GetNumHDMIVideoInputs()) + " / " + num(f.GetNumHDMIVideoOutputs()));
    s.add("AES channels in / out",
          num(f.GetNumAESAudioInputChannels()) + " / " + num(f.GetNumAESAudioOutputChannels()));
    s.add("analog channels in / out",
          num(f.GetNumAnalogAudioInputChannels()) + " / " + num(f.GetNumAnalogAudioOutputChannels()));
    s.add("HDMI audio channels in / out",
          num(f.GetNumHDMIAudioInputChannels()) + " / " + num(f.GetNumHDMIAudioOutputChannels()));
    s.add("embedded audio channels in / out",
          num(f.GetNumEmbeddedAudioInputChannels()) + " / " + num(f.GetNumEmbeddedAudioOutputChannels()));
    s.add("stacked audio", yesNo(f.CanDoStackedAudio()));
    s.add("audio mixer", yesNo(f.CanDoAudioMixer()));
    s.add("audio delay", yesNo(f.CanDoAudioDelay()));
    s.add("96 kHz audio", yesNo(f.CanDoAudio96K()));
    s.add("breakout board support", yesNo(f.CanDoBreakoutBoard()));
}

void addClock(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("clock");

    NTV2ReferenceSource reference = NTV2_REFERENCE_INVALID;
    s.add("reference", card.GetReference(reference) ? ::NTV2ReferenceSourceToString(reference) : kNA);
    s.add("reference video format", ::NTV2VideoFormatToString(card.GetReferenceVideoFormat(), true));

    // Two reads of the 48 kHz counter 200 ms apart give its rate against the
    // PC clock: a first look at whether it follows the card's audio clock.
    using Clock = std::chrono::steady_clock;
    ULWord ticks0 = 0, ticks1 = 0;
    const Clock::time_point t0 = Clock::now();
    bool ok = card.GetRawAudioTimer(ticks0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ok = card.GetRawAudioTimer(ticks1) && ok;
    const Clock::time_point t1 = Clock::now();

    s.add("raw audio timer", ok ? num(ticks1) : kNA);
    if (ok)
    {
        const double seconds = std::chrono::duration<double>(t1 - t0).count();
        const ULWord delta = ticks1 - ticks0;   // unsigned: wraps correctly
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.0f ticks/s (measured over %.0f ms)",
                      seconds > 0.0 ? double(delta) / seconds : 0.0, seconds * 1000.0);
        s.add("raw audio timer rate", buf);
    }
}

void addSharing(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("sharing");

    NTV2TaskMode mode = NTV2_TASK_MODE_INVALID;
    s.add("task mode", card.GetTaskMode(mode) ? ::NTV2TaskModeToString(mode) : kNA);

    // The base-class call: the platform driver interfaces shadow it with a
    // deprecated inline of the same name.
    ULWord appCode = 0;
    int32_t pid = 0;
    if (card.CNTV2DriverInterface::GetStreamingApplication(appCode, pid))
    {
        std::string owner;
        if (appCode == 0 && pid == 0)
            owner = "nobody";
        else
        {
            owner = text::fourcc(appCode) + ", pid " + num(pid);
            if (appCode == ULWord(NTV2_FOURCC('A', 'j', 'a', 'A')))
                owner += " (AJA services)";
        }
        s.add("reserved by", owner);
    }
    else
    {
        s.add("reserved by", kNA);
    }
}

void addAudioSystem(CNTV2Card& card, NTV2AudioSystem system, Report& report)
{
    Report::Section& s = report.addSection("audio system " + num(int(system) + 1));

    bool flag = false;
    s.add("input running", card.IsAudioInputRunning(system, flag) ? yesNo(flag) : kNA);
    s.add("output running", card.IsAudioOutputRunning(system, flag) ? yesNo(flag) : kNA);
    s.add("capture enable", card.GetAudioCaptureEnable(system, flag) ? yesNo(flag) : kNA);

    ULWord channels = 0;
    s.add("channels", card.GetNumberAudioChannels(channels, system) ? num(channels) : kNA);
    NTV2AudioRate rate = NTV2_MAX_NUM_AudioRates;
    s.add("rate", card.GetAudioRate(rate, system) ? ::NTV2AudioRateToString(rate) : kNA);
    NTV2AudioBufferSize bufferSize = NTV2_MAX_NUM_AudioBufferSizes;
    s.add("buffer size", card.GetAudioBufferSize(bufferSize, system) ? ::NTV2AudioBufferSizeToString(bufferSize) : kNA);
    NTV2AudioLoopBack loopBack = NTV2_AUDIO_LOOPBACK_INVALID;
    s.add("loopback", card.GetAudioLoopBack(loopBack, system) ? ::NTV2AudioLoopBackToString(loopBack) : kNA);

    NTV2AudioSource source = NTV2_AUDIO_SOURCE_INVALID;
    NTV2EmbeddedAudioInput embedded = NTV2_EMBEDDED_AUDIO_INPUT_INVALID;
    if (card.GetAudioSystemInputSource(system, source, embedded))
    {
        std::string sourceText = ::NTV2AudioSourceToString(source);
        if (NTV2_AUDIO_SOURCE_IS_EMBEDDED(source))
            sourceText += ", " + ::NTV2EmbeddedAudioInputToString(embedded);
        s.add("input source", sourceText);
    }
    else
    {
        s.add("input source", kNA);
    }

    NTV2EmbeddedAudioClock clock = NTV2_EMBEDDED_AUDIO_CLOCK_INVALID;
    s.add("embedded audio clock", card.GetEmbeddedAudioClock(clock, system) ? ::NTV2EmbeddedAudioClockToString(clock) : kNA);
    s.add("output erase mode", card.GetAudioOutputEraseMode(system, flag) ? yesNo(flag) : kNA);

    if (card.features().CanDoAudioDelay())
    {
        ULWord delay = 0;
        s.add("input delay", card.GetAudioInputDelay(system, delay) ? num(delay) : kNA);
        s.add("output delay", card.GetAudioOutputDelay(system, delay) ? num(delay) : kNA);
    }

    ULWord value = 0;
    s.add("capture offset", card.GetAudioReadOffset(value, system) ? addressText(value) : kNA);
    s.add("wrap address", card.GetAudioWrapAddress(value, system) ? hex32(value) + " (" + num(value) + " bytes)" : kNA);
    s.add("playback buffer at", card.GetAudioMemoryOffset(0, value, system, false) ? addressText(value) : kNA);
    s.add("capture buffer at", card.GetAudioMemoryOffset(0, value, system, true) ? addressText(value) : kNA);

    // Two reads 20 ms apart say whether each head is moving.
    ULWord in0 = 0, out0 = 0, in1 = 0, out1 = 0;
    bool inOk = card.ReadAudioLastIn(in0, system);
    bool outOk = card.ReadAudioLastOut(out0, system);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    inOk = card.ReadAudioLastIn(in1, system) && inOk;
    outOk = card.ReadAudioLastOut(out1, system) && outOk;
    s.add("last in", inOk ? hex32(in1) + (in1 != in0 ? " (moving)" : " (still)") : kNA);
    s.add("last out", outOk ? hex32(out1) + (out1 != out0 ? " (moving)" : " (still)") : kNA);
}

void addAudioOutputs(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("audio outputs");
    DeviceCapabilities& f = card.features();

    if (f.GetNumAESAudioOutputChannels() > 0)
    {
        const struct { NTV2Audio4ChannelSelect quad; const char* label; } aesQuads[] = {
            {NTV2_AudioChannel1_4, "AES 1-4"},
            {NTV2_AudioChannel5_8, "AES 5-8"},
        };
        for (const auto& q : aesQuads)
        {
            NTV2AudioSystem system = NTV2_AUDIOSYSTEM_INVALID;
            NTV2Audio4ChannelSelect channels = NTV2_AudioChannel1_4;
            s.add(q.label, card.GetAESOutputSource(q.quad, system, channels)
                               ? audioSystemName(system) + " ch " + ::NTV2AudioChannelQuadToString(channels, true)
                               : kNA);
        }
    }

    NTV2AudioChannelPair pair = NTV2_AudioChannel1_2;
    NTV2AudioSystem monitorSystem = NTV2_AUDIOSYSTEM_INVALID;
    s.add("monitor", card.GetAudioOutputMonitorSource(pair, monitorSystem)
                         ? audioSystemName(monitorSystem) + " ch " + ::NTV2AudioChannelPairToString(pair, true)
                         : kNA);

    for (UWord i = 0; i < f.GetNumVideoOutputs(); ++i)
    {
        const NTV2Channel sdi = NTV2Channel(i);
        NTV2AudioSystem system = NTV2_AUDIOSYSTEM_INVALID;
        bool enabled = false;
        const bool haveSystem = card.GetSDIOutputAudioSystem(sdi, system);
        const bool haveEnabled = card.GetSDIOutputAudioEnabled(sdi, enabled);
        std::string line = haveSystem ? audioSystemName(system) : kNA;
        line += haveEnabled ? (enabled ? ", embedder on" : ", embedder off") : ", embedder n/a";
        s.add("SDI out " + num(i + 1), line);
    }

    if (f.GetNumHDMIVideoOutputs() > 0)
    {
        NTV2Audio8ChannelSelect octet = NTV2_AudioChannel1_8;
        NTV2AudioSystem system = NTV2_AUDIOSYSTEM_INVALID;
        s.add("HDMI out 8 ch", card.GetHDMIOutAudioSource8Channel(octet, system)
                                   ? audioSystemName(system) + " ch " + ::NTV2AudioChannelOctetToString(octet, true)
                                   : kNA);
        s.add("HDMI out 2 ch", card.GetHDMIOutAudioSource2Channel(pair, system)
                                   ? audioSystemName(system) + " ch " + ::NTV2AudioChannelPairToString(pair, true)
                                   : kNA);
    }
}

void addXpand(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("xpand");
    if (!card.features().CanDoBreakoutBoard())
    {
        s.add("breakout board", "not supported by this device");
        return;
    }

    // kRegBOBAudioControl holds the Xpand's analog/AES input select and the
    // analog reference level. The SDK writes 0 or 1 into the 4-bit select
    // field (EnableBOBAnalogAudioIn); the whole field is shown here.
    ULWord control = 0;
    if (card.ReadRegister(kRegBOBAudioControl, control))
    {
        s.add("audio control register", hex32(control));
        s.add("analog input select (bits 8-11)",
              num((control & ULWord(kRegMaskBOBAnalogInputSelect)) >> ULWord(kRegShiftBOBAnalogInputSelect)));
        s.add("analog level field (bits 4-7)",
              num((control & ULWord(kRegMaskBOBAnalogLevelControl)) >> ULWord(kRegShiftBOBAnalogLevelControl)));
    }
    else
    {
        s.add("audio control register", kNA);
    }

    NTV2AudioLevel level = NTV2_AUDIOLEVEL_24dBu;
    s.add("analog level", card.GetAudioAnalogLevel(level) ? audioLevelName(level) : kNA);

    ULWord status = 0;
    s.add("status register", card.ReadRegister(kRegBOBStatus, status) ? hex32(status) : kNA);
}

void addAudioMixer(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("audio mixer");
    if (!card.features().CanDoAudioMixer())
    {
        s.add("audio mixer", "not supported by this device");
        return;
    }

    const NTV2AudioMixerInput inputs[] = {NTV2_AudioMixerInputMain, NTV2_AudioMixerInputAux1, NTV2_AudioMixerInputAux2};
    for (const NTV2AudioMixerInput input : inputs)
    {
        NTV2AudioSystem system = NTV2_AUDIOSYSTEM_INVALID;
        s.add(mixerInputName(input) + " source",
              card.GetAudioMixerInputAudioSystem(input, system) ? audioSystemName(system) : kNA);
        NTV2AudioChannelsMuted16 mutes;
        s.add(mixerInputName(input) + " muted",
              card.GetAudioMixerInputChannelsMute(input, mutes) ? text::channelSet(ULWord(mutes.to_ulong()), 16) : kNA);
    }

    NTV2AudioChannelPair pair = NTV2_AudioChannel1_2;
    s.add("main channel select", card.GetAudioMixerInputChannelSelect(NTV2_AudioMixerInputMain, pair)
                                     ? ::NTV2AudioChannelPairToString(pair, true)
                                     : kNA);
    ULWord gain = 0;
    s.add("output gain", card.GetAudioMixerOutputGain(gain) ? hex32(gain) : kNA);
    NTV2AudioChannelsMuted16 outputMutes;
    s.add("output muted", card.GetAudioMixerOutputChannelsMute(outputMutes)
                              ? text::channelSet(ULWord(outputMutes.to_ulong()), 16)
                              : kNA);
}

void addAutoCirculate(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("autocirculate");

    // Lowest byte of any audio system's buffers: the video frames of a
    // running channel should end below it (stacked audio sits at the top).
    ULWord lowestAudio = std::numeric_limits<ULWord>::max();
    for (UWord i = 0; i < audioSystemCount(card); ++i)
    {
        ULWord address = 0;
        if (card.GetAudioMemoryOffset(0, address, NTV2AudioSystem(i), false))
            lowestAudio = std::min(lowestAudio, address);
    }

    bool anyRunning = false;
    for (UWord i = 0; i < card.features().GetNumFrameStores(); ++i)
    {
        const NTV2Channel channel = NTV2Channel(i);
        AUTOCIRCULATE_STATUS status;
        if (!card.AutoCirculateGetStatus(channel, status))
        {
            s.add("channel " + num(i + 1), kNA);
            continue;
        }
        if (status.IsStopped())
            continue;
        anyRunning = true;

        std::ostringstream line;
        line << autoCirculateStateName(status.GetState()) << ", " << (status.IsInput() ? "capture" : "playout")
             << ", frames " << status.GetStartFrame() << "-" << status.GetEndFrame()
             << ", active " << status.GetActiveFrame()
             << ", level " << status.GetBufferLevel()
             << ", processed " << status.GetProcessedFrameCount()
             << ", dropped " << status.GetDroppedFrameCount()
             << ", audio " << (status.WithAudio() ? audioSystemName(status.GetAudioSystem()) : std::string("none"));

        uint64_t address = 0, length = 0;
        if (card.GetDeviceFrameInfo(status.GetEndFrame(), channel, address, length))
        {
            const uint64_t top = address + length;   // one past the last byte
            line << "\nframes end at " << hex32(ULWord(top - 1));
            if (lowestAudio != std::numeric_limits<ULWord>::max())
            {
                if (top <= lowestAudio)
                    line << ", " << megabytes(lowestAudio - top) << " below the audio buffers";
                else
                    line << ", OVERLAPS the audio buffers by " << megabytes(top - lowestAudio);
            }
        }
        s.add("channel " + num(i + 1), line.str());
    }
    if (!anyRunning)
        s.add("channels", "none running");
}

void addSdramMap(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("sdram");

    // Stopped audio systems' buffers are kept tagged ("Aud1", "Aud2") so a
    // video frame over them shows up as a conflict even while they are idle.
    SDRAMAuditor auditor;
    if (!auditor.AssessDevice(card, /*inIgnoreStoppedAudioBuffers*/ false))
    {
        s.add("audit", "failed");
        return;
    }

    ULWordSequence freeRegions, usedRegions, badRegions;
    auditor.GetRegions(freeRegions, usedRegions, badRegions);
    std::string conflicts;
    for (const ULWord region : badRegions)
        conflicts += (conflicts.empty() ? "" : "\n") + text::describeBlockRegion(region);
    s.add("conflicts", conflicts.empty() ? std::string("none") : conflicts);

    std::ostringstream blocks;
    auditor.DumpBlocks(blocks);
    s.block("8 MB blocks (SDRAMAuditor):\n" + blocks.str());
}

/// True for per-audio-system registers ("kRegAud3Control") of systems the
/// device does not have; the SDK's register list covers eight.
bool isRegisterOfMissingAudioSystem(const std::string& name, UWord systemCount)
{
    const std::string prefix = "kRegAud";
    if (name.compare(0, prefix.size(), prefix) != 0 || name.size() <= prefix.size())
        return false;
    const char digit = name[prefix.size()];
    return digit >= '1' && digit <= '8' && UWord(digit - '0') > systemCount;
}

void addRegisters(CNTV2Card& card, Report& report)
{
    Report::Section& s = report.addSection("registers", /*detail*/ true);
    const NTV2DeviceID id = card.GetDeviceID();

    const NTV2RegNumSet onDevice = CNTV2RegisterExpert::GetRegistersForDevice(id);
    NTV2RegNumSet wanted;
    for (const std::string& registerClass : {kRegClass_Audio, kRegClass_AES})
    {
        for (const ULWord reg : CNTV2RegisterExpert::GetRegistersForClass(registerClass))
        {
            if (onDevice.count(reg))
                wanted.insert(reg);
        }
    }
    if (card.features().CanDoBreakoutBoard())
    {
        for (ULWord reg = ULWord(kRegBOBStatus); reg <= ULWord(kRegBOBAudioControl); ++reg)
            wanted.insert(reg);
    }

    const UWord systems = audioSystemCount(card);
    for (const ULWord reg : wanted)
    {
        const std::string name = CNTV2RegisterExpert::GetDisplayName(reg);
        if (CNTV2RegisterExpert::IsWriteOnly(reg) || isRegisterOfMissingAudioSystem(name, systems))
            continue;

        ULWord value = 0;
        std::string entry = name + " (" + num(reg) + ")";
        if (!card.ReadRegister(reg, value))
        {
            s.block(entry + " = read failed");
            continue;
        }
        entry += " = " + hex32(value);
        std::istringstream decoded(CNTV2RegisterExpert::GetDisplayValue(reg, value, id));
        for (std::string line; std::getline(decoded, line);)
        {
            if (!line.empty())
                entry += "\n  " + line;
        }
        s.block(entry);
    }
}

} // namespace

bool inspectCard(const std::string& selector, Report& report, std::string& error)
{
    CNTV2Card card;
    if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(selector, card) || !card.IsOpen())
    {
        error = "no AJA device matches '" + selector + "' (try: duplex-probe list)";
        return false;
    }

    addIdentity(card, report);
    addClock(card, report);
    addSharing(card, report);
    for (UWord i = 0; i < audioSystemCount(card); ++i)
        addAudioSystem(card, NTV2AudioSystem(i), report);
    addAudioOutputs(card, report);
    addXpand(card, report);
    addAudioMixer(card, report);
    addAutoCirculate(card, report);
    addSdramMap(card, report);
    addCapabilities(card, report);
    addRegisters(card, report);
    return true;
}

} // namespace ajaduplex::device
