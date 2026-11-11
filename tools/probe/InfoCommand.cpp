// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "InfoCommand.h"

#include "CommandSupport.h"
#include "common/Report.h"
#include "common/TextFormat.h"
#include "device/CardInspector.h"

#include <cstdio>
#include <ctime>

namespace ajaduplex::probe
{

int runInfo(const Options& options)
{
    const std::tm now = text::localTime(std::time(nullptr));

    Report report;
    addProbeHeader(report, options, now, /*simulated*/ false);

    std::string error;
    if (!device::inspectCard(options.device, report, error))
    {
        std::fprintf(stderr, "duplex-probe: %s\n", error.c_str());
        return 1;
    }
    // The file adds the capabilities and the decoded registers.
    return finishReport(options, "info", now, report, /*csv*/ std::string(), /*interrupted*/ false);
}

} // namespace ajaduplex::probe
