// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CardInspector.h
 * @brief   Read-only snapshot of an AJA card's audio state, for duplex-probe info.
 *
 * Part of src/device, the only code that includes libajantv2.
 */

#pragma once

#include "common/Report.h"

#include <string>

namespace ajaduplex::device
{

/// Opens the device named by `selector` and appends its state to `report`,
/// one section per topic: identity, clock, sharing, each audio system, audio
/// outputs, Xpand, audio mixer, AutoCirculate, the SDRAM map, and (as detail
/// sections) the capabilities and the audio registers decoded by the SDK.
///
/// `selector` is what CNTV2DeviceScanner::GetFirstDeviceFromArgument accepts:
/// an index ("0"), a serial number or a model name.
///
/// Read-only: it reads registers and driver state, and never writes a
/// register, reserves the device or changes its task mode, so it can run
/// while other programs use the card.
///
/// Returns false, with `error` set, when no device matches the selector.
bool inspectCard(const std::string& selector, Report& report, std::string& error);

} // namespace ajaduplex::device
