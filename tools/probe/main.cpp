// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    main.cpp
 * @brief   duplex-probe: the phase 0 measurement tool.
 *
 * Each command answers some of the open questions of the design (see
 * docs/ROADMAP.md and docs/PROBE.md) and leaves a report file behind.
 */

#include "DuplexVersion.h"
#include "MeasureCommands.h"
#include "InfoCommand.h"
#include "ListCommand.h"
#include "ProbeOptions.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    using namespace ajaduplex::probe;

    try
    {
        const std::vector<std::string> args(argv + 1, argv + argc);
        const ParseResult parsed = parseArguments(args);
        if (!parsed.ok)
        {
            std::fprintf(stderr, "duplex-probe: %s\n\n%s", parsed.error.c_str(), usageText().c_str());
            return 2;
        }

        switch (parsed.options.command)
        {
            case Command::Help:
                std::fputs(usageText().c_str(), stdout);
                return 0;
            case Command::Version:
                std::printf("duplex-probe %s\n", AJADUPLEX_VERSION_STRING);
                return 0;
            case Command::List:
                return runList();
            case Command::Info:
                return runInfo(parsed.options);
            case Command::Heads:
                return runHeadsCommand(parsed.options);
            case Command::Cost:
                return runCostCommand(parsed.options);
            case Command::Loop:
                return runLoopCommand(parsed.options);
            case Command::Routing:
                return runRoutingCommand(parsed.options);
            case Command::Watch:
                return runWatchCommand(parsed.options);
        }
        return 2;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "duplex-probe: unexpected error: %s\n", e.what());
        return 3;
    }
}
