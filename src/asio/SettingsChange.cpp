// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/SettingsChange.h"

namespace ajaduplex::asio
{

SettingsChange compareSettings(const DriverSettings& before, const DriverSettings& after)
{
    SettingsChange change;
    enum class Kind
    {
        Reset,
        Latency,
        KeepRouting,
        AnalogLevel,
    };
    auto note = [&change](bool differs, const char* name, Kind kind) {
        if (!differs)
            return;
        change.any = true;
        change.what += (change.what.empty() ? "" : ", ") + std::string(name);
        switch (kind)
        {
            case Kind::Reset:       change.needsReset = true; break;
            case Kind::Latency:     change.latencies = true; break;
            case Kind::KeepRouting: change.keepRouting = true; break;
            case Kind::AnalogLevel: change.analogLevel = true; break;
        }
    };
    note(before.card != after.card, "Card", Kind::Reset);
    note(before.audioSystem != after.audioSystem, "AudioSystem", Kind::Reset);
    note(before.input != after.input, "Input", Kind::Reset);
    // Which SDI input only matters when the input is SDI.
    note(before.sdiInput != after.sdiInput && (before.input == InputChoice::Sdi || after.input == InputChoice::Sdi),
         "SdiInput", Kind::Reset);
    note(before.bufferFrames != after.bufferFrames, "BufferSize", Kind::Reset);
    note(before.guardFrames != after.guardFrames, "Guard", Kind::Reset);
    note(before.aggressive != after.aggressive, "Aggressive", Kind::Reset);
    note(before.oemTasks != after.oemTasks, "OemTaskMode", Kind::Reset);
    // The SDI outputs change how many outputs there are, and their names.
    note(before.sdiOutputs != after.sdiOutputs, "SdiOutputs", Kind::Reset);
    note(before.hdmiOutput != after.hdmiOutput, "HdmiOutput", Kind::Reset);
    note(before.inputLatencyFrames != after.inputLatencyFrames, "InputLatency", Kind::Latency);
    note(before.outputLatencyFrames != after.outputLatencyFrames, "OutputLatency", Kind::Latency);
    note(before.keepRouting != after.keepRouting, "KeepRouting", Kind::KeepRouting);
    note(before.analogLevelDbu != after.analogLevelDbu, "AnalogLevel", Kind::AnalogLevel);
    return change;
}

} // namespace ajaduplex::asio
