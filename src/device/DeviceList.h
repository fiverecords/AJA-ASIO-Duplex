// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    DeviceList.h
 * @brief   The AJA devices AJA's driver exposes, and the SDK version.
 *
 * Part of src/device, the only code that includes libajantv2. Callers see
 * plain strings and numbers.
 */

#pragma once

#include <string>
#include <vector>

namespace ajaduplex::device
{

struct DeviceSummary
{
    unsigned index = 0;      ///< Index as CNTV2DeviceScanner counts them.
    std::string model;       ///< e.g. "KONA X"
    std::string serial;      ///< Serial number, or "n/a".
    std::string deviceId;    ///< SDK name and hex value of the NTV2DeviceID.
};

/// Every device AJA's driver reports, in SDK index order. Opening a device
/// to read its name does not change its state.
std::vector<DeviceSummary> listDevices();

/// Version of the libajantv2 the program was built with.
std::string sdkVersion();

} // namespace ajaduplex::device
