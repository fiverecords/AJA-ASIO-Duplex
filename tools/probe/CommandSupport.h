// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CommandSupport.h
 * @brief   What the probe's commands share: opening the card (real or
 *          simulated), the report header, and writing the output files.
 */

#pragma once

#include "ProbeOptions.h"
#include "common/Clock.h"
#include "common/Report.h"
#include "device/CardAudio.h"

#include <ctime>
#include <memory>
#include <string>

namespace ajaduplex::probe
{

/// A card to measure and the clock to measure it with: the real card with
/// the system clock, or the simulated card with its virtual clock.
struct CardSession
{
    std::unique_ptr<Clock> clock;
    std::unique_ptr<device::CardAudio> card;
    bool simulated = false;
};

/// What the simulated card has plugged in.
enum class SimulatedSetup
{
    NoCables,   ///< heads, cost
    AesLoop,    ///< loop: AES out 1-8 cabled to AES in 1-8, a PC whose timer is sometimes late
    AnalogLoop, ///< loop --path analog: the same, plus XLR out L/R cabled to XLR in L/R
};

/// Opens what the options ask for (--simulate, --device, --audio-system).
bool openCardSession(const Options& options, CardSession& session, std::string& error,
                     SimulatedSetup setup = SimulatedSetup::NoCables);

/// The [probe] section every report starts with.
void addProbeHeader(Report& report, const Options& options, const std::tm& when, bool simulated);

/// Prints the summary, writes <command>-<label>-<time>.txt (and .csv when
/// `csv` is not empty) and says where. Returns the exit code: 0, or 1 when a
/// file could not be written or the run was interrupted.
int finishReport(const Options& options, const std::string& command, const std::tm& when, const Report& report,
                 const std::string& csv, bool interrupted);

} // namespace ajaduplex::probe
