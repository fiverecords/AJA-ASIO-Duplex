// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/ChannelMap.h"

namespace ajaduplex::asio
{

ChannelMap ChannelMap::forSettings(const DriverSettings& settings)
{
    ChannelMap map;
    switch (settings.input)
    {
        case InputChoice::Analog:
            map.inputs.push_back({"Analog L", 0});
            map.inputs.push_back({"Analog R", 1});
            break;
        case InputChoice::Aes:
            for (unsigned c = 0; c < 8; ++c)
                map.inputs.push_back({"AES " + std::to_string(c + 1), c});
            break;
        case InputChoice::Sdi:
            for (unsigned c = 0; c < 16; ++c)
                map.inputs.push_back(
                    {"SDI " + std::to_string(settings.sdiInput) + " ch " + std::to_string(c + 1), c});
            break;
        case InputChoice::Hdmi:
            for (unsigned c = 0; c < 8; ++c)
                map.inputs.push_back({"HDMI ch " + std::to_string(c + 1), c});
            break;
    }
    // Outputs 1-8 go to AES 1-8, 1-2 to the analog out too, and to every
    // SDI or HDMI output that plays: those carry the same channel numbers,
    // so the names say "+ SDI" and "+ HDMI" (31 characters at most, as ASIO
    // allows). Outputs 9-16 only exist with an SDI output, the only one that
    // carries them.
    const bool sdi = settings.sdiOutputs != 0;
    const std::string also = std::string(sdi ? " + SDI" : "") + (settings.hdmiOutput ? " + HDMI" : "");
    map.outputs.push_back({"AES 1 + Analog L" + also, 0});
    map.outputs.push_back({"AES 2 + Analog R" + also, 1});
    for (unsigned c = 2; c < 8; ++c)
        map.outputs.push_back({"AES " + std::to_string(c + 1) + also, c});
    for (unsigned c = 8; sdi && c < 16; ++c)
        map.outputs.push_back({"SDI " + std::to_string(c + 1), c});
    return map;
}

} // namespace ajaduplex::asio
