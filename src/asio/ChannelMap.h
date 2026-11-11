// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    ChannelMap.h
 * @brief   The ASIO channels the driver offers, their names and card channels.
 *
 * Inputs follow the input chosen in the settings: analog gives "Analog L"
 * and "Analog R" (the Xpand puts them on channels 1-2 of the audio
 * system), AES gives "AES 1" to "AES 8", an SDI input "SDI 1 ch 1" to "SDI
 * 1 ch 16", the HDMI input "HDMI ch 1" to "HDMI ch 8". Outputs are AES 1-8,
 * channels 1-8 of the audio system. The Xpand's analog output follows either AES 1-2 or
 * the monitor (Q4, not answered yet); the driver points the monitor at
 * channels 1-2 too, so the analog output plays outputs 1 and 2 either way,
 * and their names say so. An SDI output, when one plays, carries all 16
 * channels: outputs 1-8 say "+ SDI", and outputs 9-16, "SDI 9" to "SDI
 * 16", exist only then. The HDMI output carries channels 1-8: "+ HDMI".
 */

#pragma once

#include "common/DriverSettings.h"

#include <string>
#include <vector>

namespace ajaduplex::asio
{

struct ChannelEntry
{
    std::string name;
    unsigned cardChannel = 0;   ///< 0-based channel of the audio system
};

struct ChannelMap
{
    std::vector<ChannelEntry> inputs;
    std::vector<ChannelEntry> outputs;

    static ChannelMap forSettings(const DriverSettings& settings);
};

} // namespace ajaduplex::asio
