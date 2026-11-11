// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "ListCommand.h"

#include "device/DeviceList.h"

#include <cstdio>

namespace ajaduplex::probe
{

int runList()
{
    const std::vector<device::DeviceSummary> devices = device::listDevices();
    if (devices.empty())
    {
        std::printf("No AJA devices found. Check that AJA's driver is installed and the card is seated.\n");
        return 1;
    }
    for (const device::DeviceSummary& d : devices)
        std::printf("%u  %s  serial %s  %s\n", d.index, d.model.c_str(), d.serial.c_str(), d.deviceId.c_str());
    return 0;
}

} // namespace ajaduplex::probe
