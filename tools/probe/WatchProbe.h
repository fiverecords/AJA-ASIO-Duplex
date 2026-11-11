// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    WatchProbe.h
 * @brief   watch: the driver's routing watch on the card, without a host.
 *
 * The engine runs as the driver's does (N 128, G 32, the driver's input,
 * AES out 1-8 and the monitor routed to this audio system), with silence
 * on the outputs and nothing done with the inputs, and the routing is read
 * back once a second after a block, as in the driver (engine/RoutingWatch).
 * Every change is printed as it comes and listed in the report: what
 * changed, to what, and whether it was put back (--keep, the driver's Keep
 * my routing).
 *
 * Left alone for a few minutes with the card in standard task mode, it
 * answers Q9: whether AJA's services rewrite the audio routing by
 * themselves. Run while someone changes things in the AJA Control Panel,
 * or starts AJA's audio driver or another program, it shows what the
 * driver will report when that happens while it plays.
 */

#pragma once

#include "common/Clock.h"
#include "common/Report.h"
#include "device/CardAudio.h"
#include "engine/AudioEngine.h"

#include <functional>
#include <string>
#include <vector>

namespace ajaduplex::probe
{

struct WatchSettings
{
    double seconds = 60.0;
    bool analog = true;        ///< the input set up: analog, the driver's default (D2), or AES
    bool keep = false;         ///< put the routing back, as the driver's Keep my routing does
    unsigned frames = 128;     ///< N and G as the driver's defaults
    unsigned guard = 32;
};

struct WatchEvent
{
    double seconds = 0.0;      ///< since the start
    std::string line;          ///< as the driver logs it
};

struct WatchRun
{
    WatchSettings settings;
    std::string description;            ///< the card
    bool started = false;
    std::string error;                  ///< why it did not start
    bool interrupted = false;
    double seconds = 0.0;               ///< how long it ran
    bool watched = false;               ///< the routing could be read back
    device::CardRouting baseline;       ///< as configured: the driver's
    device::CardRouting last;           ///< at the last check
    unsigned checks = 0;
    unsigned changes = 0;               ///< parts of the driver's routing another program changed
    unsigned otherChanges = 0;          ///< changes to what it leaves as the card has it (the analog level)
    unsigned putBack = 0;               ///< parts put back (--keep)
    unsigned notOursAtEnd = 0;          ///< device::routing bits not the driver's at the last check
    std::vector<WatchEvent> events;
    engine::EngineStats stats;
};

/// Runs it. `stop` is asked between blocks (Ctrl+C); `meanwhile`, if set,
/// is called after every block with the seconds since the start (the
/// simulated run plays another program with it); `onEvent`, if set, gets
/// each line as it comes. On the caller's thread; the card is left
/// stopped, for the caller to restore.
WatchRun runWatch(device::CardAudio& card, Clock& clock, const WatchSettings& settings,
                  const std::function<bool()>& stop, const std::function<void(double)>& meanwhile,
                  const std::function<void(const WatchEvent&)>& onEvent);

/// The [watch] section, and [answers] with Q9.
void addWatchReport(const WatchRun& run, Report& report);

} // namespace ajaduplex::probe
