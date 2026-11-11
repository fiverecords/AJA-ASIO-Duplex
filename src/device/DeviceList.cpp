// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "device/DeviceList.h"

#include "common/TextFormat.h"

#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"

namespace ajaduplex::device
{

std::vector<DeviceSummary> listDevices()
{
    // The SDK numbers devices from 0 without gaps; stop at the first index
    // that does not open. The cap only guards against a misbehaving driver.
    constexpr ULWord kMaxDevices = 16;

    std::vector<DeviceSummary> devices;
    for (ULWord index = 0; index < kMaxDevices; ++index)
    {
        CNTV2Card card;
        if (!CNTV2DeviceScanner::GetDeviceAtIndex(index, card))
            break;

        DeviceSummary summary;
        summary.index = index;
        summary.model = card.GetModelName();
        std::string serial;
        summary.serial = card.GetSerialNumberString(serial) ? serial : "n/a";
        const NTV2DeviceID id = card.GetDeviceID();
        summary.deviceId = ::NTV2DeviceIDToString(id) + " (" + text::hex32(ULWord(id)) + ")";
        devices.push_back(summary);
    }
    return devices;
}

std::string sdkVersion()
{
    return ::NTV2GetVersionString(true);
}

} // namespace ajaduplex::device
