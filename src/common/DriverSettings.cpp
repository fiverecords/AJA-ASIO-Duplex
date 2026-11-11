// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "common/DriverSettings.h"

namespace ajaduplex
{

namespace
{

// The value names, as they appear in the registry.
constexpr const char* kCard = "Card";
constexpr const char* kAudioSystem = "AudioSystem";
constexpr const char* kInput = "Input";
constexpr const char* kSdiInput = "SdiInput";
constexpr const char* kBufferSize = "BufferSize";
constexpr const char* kGuard = "Guard";
constexpr const char* kAggressive = "Aggressive";
constexpr const char* kInputLatency = "InputLatency";
constexpr const char* kOutputLatency = "OutputLatency";
constexpr const char* kKeepRouting = "KeepRouting";
constexpr const char* kAnalogLevel = "AnalogLevel";
constexpr const char* kOemTasks = "OemTaskMode";   // "OemTasks" up to 0.1.16, off by default then
constexpr const char* kSdiOutputs = "SdiOutputs";
constexpr const char* kHdmiOutput = "HdmiOutput";

constexpr std::uint32_t kMaxCard = 15;
constexpr std::uint32_t kMaxAudioSystem = 8;

/// Reads `name` into `value` if it is there and `valid` accepts it; says
/// whether it did.
template <typename Valid>
bool readIf(SettingsStore& store, const char* name, std::uint32_t& value, Valid valid)
{
    std::uint32_t read = 0;
    if (!store.readNumber(name, read) || !valid(read))
        return false;
    value = read;
    return true;
}

} // namespace

const char* toString(InputChoice input)
{
    switch (input)
    {
        case InputChoice::Analog: return "analog";
        case InputChoice::Aes:    return "AES";
        case InputChoice::Sdi:    return "SDI embedded";
        case InputChoice::Hdmi:   return "HDMI embedded";
    }
    return "?";
}

std::string describeInput(const DriverSettings& s)
{
    if (s.input == InputChoice::Sdi)
        return "SDI " + std::to_string(s.sdiInput) + " embedded";
    return toString(s.input);
}

std::string describeOutputs(const DriverSettings& s)
{
    std::string text = "AES 1-8, analog out";
    for (unsigned n = 0; n < 8; ++n)
    {
        if ((s.sdiOutputs & (1u << n)) != 0)
            text += ", SDI out " + std::to_string(n + 1);
    }
    if (s.hdmiOutput)
        text += ", HDMI out";
    return text;
}

bool isAnalogLevel(std::uint32_t dbu, bool orNone)
{
    return dbu == 24 || dbu == 18 || dbu == 15 || dbu == 12 || (orNone && dbu == 0);
}

bool isValidBufferSize(std::uint32_t frames)
{
    return frames >= DriverSettings::kMinBufferFrames && frames <= DriverSettings::kMaxBufferFrames &&
           (frames & (frames - 1)) == 0;
}

bool MemorySettingsStore::readNumber(const char* name, std::uint32_t& value)
{
    const auto found = values.find(name);
    if (found == values.end())
        return false;
    value = found->second;
    return true;
}

bool MemorySettingsStore::writeNumber(const char* name, std::uint32_t value)
{
    values[name] = value;
    return true;
}

DriverSettings loadDriverSettings(SettingsStore& store)
{
    DriverSettings s;
    readIf(store, kCard, s.card, [](std::uint32_t v) { return v <= kMaxCard; });
    readIf(store, kAudioSystem, s.audioSystem, [](std::uint32_t v) { return v <= kMaxAudioSystem; });
    std::uint32_t input = static_cast<std::uint32_t>(s.input);
    readIf(store, kInput, input, [](std::uint32_t v) { return v <= 3; });
    s.input = static_cast<InputChoice>(input);
    readIf(store, kSdiInput, s.sdiInput, [](std::uint32_t v) { return v >= 1 && v <= 8; });
    readIf(store, kBufferSize, s.bufferFrames, isValidBufferSize);
    readIf(store, kGuard, s.guardFrames, [](std::uint32_t v) { return v <= DriverSettings::kMaxGuardFrames; });
    std::uint32_t aggressive = s.aggressive ? 1 : 0;
    readIf(store, kAggressive, aggressive, [](std::uint32_t v) { return v <= 1; });
    s.aggressive = aggressive != 0;
    readIf(store, kInputLatency, s.inputLatencyFrames,
           [](std::uint32_t v) { return v <= DriverSettings::kMaxLatencyFrames; });
    // Not saved (or not a latency): the card's own figure for the input,
    // which was read above.
    if (!readIf(store, kOutputLatency, s.outputLatencyFrames,
                [](std::uint32_t v) { return v <= DriverSettings::kMaxLatencyFrames; }))
        s.outputLatencyFrames = DriverSettings::cardLatencyFrames(s.input);
    std::uint32_t keep = s.keepRouting ? 1 : 0;
    readIf(store, kKeepRouting, keep, [](std::uint32_t v) { return v <= 1; });
    s.keepRouting = keep != 0;
    readIf(store, kAnalogLevel, s.analogLevelDbu, [](std::uint32_t v) { return isAnalogLevel(v, true); });
    std::uint32_t oem = s.oemTasks ? 1 : 0;
    readIf(store, kOemTasks, oem, [](std::uint32_t v) { return v <= 1; });
    s.oemTasks = oem != 0;
    readIf(store, kSdiOutputs, s.sdiOutputs, [](std::uint32_t v) { return v <= 0xFFu; });
    std::uint32_t hdmi = s.hdmiOutput ? 1 : 0;
    readIf(store, kHdmiOutput, hdmi, [](std::uint32_t v) { return v <= 1; });
    s.hdmiOutput = hdmi != 0;
    return s;
}

bool saveDriverSettings(SettingsStore& store, const DriverSettings& s)
{
    bool ok = store.writeNumber(kCard, s.card);
    ok = store.writeNumber(kAudioSystem, s.audioSystem) && ok;
    ok = store.writeNumber(kInput, static_cast<std::uint32_t>(s.input)) && ok;
    ok = store.writeNumber(kSdiInput, s.sdiInput) && ok;
    ok = store.writeNumber(kBufferSize, s.bufferFrames) && ok;
    ok = store.writeNumber(kGuard, s.guardFrames) && ok;
    ok = store.writeNumber(kAggressive, s.aggressive ? 1u : 0u) && ok;
    ok = store.writeNumber(kInputLatency, s.inputLatencyFrames) && ok;
    ok = store.writeNumber(kOutputLatency, s.outputLatencyFrames) && ok;
    ok = store.writeNumber(kKeepRouting, s.keepRouting ? 1u : 0u) && ok;
    ok = store.writeNumber(kAnalogLevel, s.analogLevelDbu) && ok;
    ok = store.writeNumber(kOemTasks, s.oemTasks ? 1u : 0u) && ok;
    ok = store.writeNumber(kSdiOutputs, s.sdiOutputs) && ok;
    ok = store.writeNumber(kHdmiOutput, s.hdmiOutput ? 1u : 0u) && ok;
    return ok;
}

std::string describe(const DriverSettings& s)
{
    std::string text;
    auto line = [&text](const char* name, const std::string& value) {
        text += std::string(name) + " = " + value + "\n";
    };
    line(kCard, std::to_string(s.card));
    line(kAudioSystem, s.audioSystem == 0 ? std::string("0 (the card's last)") : std::to_string(s.audioSystem));
    line(kInput, std::to_string(static_cast<std::uint32_t>(s.input)) + " (" + toString(s.input) + ")");
    line(kSdiInput, std::to_string(s.sdiInput));
    line(kBufferSize, std::to_string(s.bufferFrames));
    line(kGuard, std::to_string(s.guardFrames));
    line(kAggressive, s.aggressive ? "1" : "0");
    line(kInputLatency, std::to_string(s.inputLatencyFrames));
    line(kOutputLatency, std::to_string(s.outputLatencyFrames));
    line(kKeepRouting, s.keepRouting ? "1" : "0");
    line(kAnalogLevel, s.analogLevelDbu == 0 ? std::string("0 (as the card has it)")
                                             : std::to_string(s.analogLevelDbu) + " (+" +
                                                   std::to_string(s.analogLevelDbu) + " dBu)");
    line(kOemTasks, s.oemTasks ? "1" : "0");
    std::string outputs;
    for (unsigned n = 0; n < 8; ++n)
    {
        if ((s.sdiOutputs & (1u << n)) != 0)
            outputs += (outputs.empty() ? "SDI out " : ", ") + std::to_string(n + 1);
    }
    line(kSdiOutputs, std::to_string(s.sdiOutputs) + " (" + (outputs.empty() ? std::string("none") : outputs) + ")");
    line(kHdmiOutput, s.hdmiOutput ? "1" : "0");
    return text;
}

} // namespace ajaduplex
