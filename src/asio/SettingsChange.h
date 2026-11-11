// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SettingsChange.h
 * @brief   What a change of settings means for a host that has the driver.
 *
 * The design's rule: a change to the source, the outputs, the routing or
 * the buffer means a reset (kAsioResetRequest: the host stops, lets the driver go and
 * opens it again, so the new settings are read); a change that only moves
 * the latencies is told with kAsioLatenciesChanged and applies at once.
 * The guard and aggressive mode change S, which the engine is placed by,
 * so they need the reset too. Keeping the routing or not applies at the
 * next check of the routing watch, and the Xpand's analog level after the
 * next block, with no reset and nothing to tell.
 */

#pragma once

#include "common/DriverSettings.h"

#include <string>

namespace ajaduplex::asio
{

struct SettingsChange
{
    bool any = false;
    bool needsReset = false;     ///< card, audio system, input, outputs, buffer size, guard, aggressive mode, OEM task mode
    bool latencies = false;      ///< input or output latency
    bool keepRouting = false;    ///< Keep my routing, on or off
    bool analogLevel = false;    ///< the Xpand's analog level
    std::string what;            ///< "Input, Guard", for the log
};

SettingsChange compareSettings(const DriverSettings& before, const DriverSettings& after);

} // namespace ajaduplex::asio
