// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "CommandSupport.h"

#include "DuplexVersion.h"
#include "OutputFile.h"
#include "common/TextFormat.h"
#include "device/DeviceList.h"
#include "device/Ntv2CardAudio.h"
#include "platform/Realtime.h"
#include "sim/FakeCardAudio.h"
#include "sim/VirtualClock.h"

#include <cstdio>

#ifdef _WIN32
#include "asio/win/InstanceLock.h"
#endif

namespace ajaduplex::probe
{

namespace
{

#ifdef _WIN32
/// The card index a --device selector gives, if it gives one.
bool cardIndex(const std::string& selector, unsigned& index)
{
    if (selector.empty() || selector.size() > 2)
        return false;
    index = 0;
    for (const char c : selector)
    {
        if (c < '0' || c > '9')
            return false;
        index = index * 10 + unsigned(c - '0');
    }
    return true;
}
#endif

} // namespace

bool openCardSession(const Options& options, CardSession& session, std::string& error, SimulatedSetup setup)
{
    if (options.simulate)
    {
        auto clock = std::make_unique<sim::VirtualClock>();
        sim::FakeCardModel model;
        if (setup == SimulatedSetup::AesLoop || setup == SimulatedSetup::AnalogLoop)
        {
            model = setup == SimulatedSetup::AnalogLoop ? sim::FakeCardModel::withAnalogLoop()
                                                        : sim::FakeCardModel::withAesLoop();
            // As on a KONA X with audio system 2: AES out follows audio
            // system 1 until the loop routes it.
            model.aesOutputsOnThisSystem = false;
            // Wake-ups usually 0-40 us late, one in a thousand 2 ms late: the
            // smaller combinations of the sweep fail, as they may on a PC.
            clock->setSleepLateness(sim::SleepLateness{20000, 0.001, 2000000, 7});
        }
        model.jitterNs = 5000;   // so the simulated distributions are not single values
        session.card = std::make_unique<sim::FakeCardAudio>(*clock, model);
        session.clock = std::move(clock);
        session.simulated = true;
        return true;
    }
#ifdef _WIN32
    // The driver's lock, held until this program ends: a host cannot start
    // AJA Duplex ASIO on this card and audio system while it measures, nor
    // can this start while a host plays through it.
    static asio::win::InstanceLock lock;
    unsigned index = 0;
    if (cardIndex(options.device, index))
    {
        std::string holder;
        if (!lock.acquire(index, options.audioSystem, holder))
        {
            error = "AJA Duplex ASIO is open in " + holder + " on this card and audio system: close it first";
            return false;
        }
    }
#endif
    session.card = device::openNtv2CardAudio(options.device, options.audioSystem, error);
    if (!session.card)
        return false;
    session.clock = std::make_unique<platform::SystemClock>();
    session.simulated = false;
    return true;
}

void addProbeHeader(Report& report, const Options& options, const std::tm& when, bool simulated)
{
    Report::Section& header = report.addSection("probe");
    header.add("tool", std::string("duplex-probe ") + AJADUPLEX_VERSION_STRING);
    header.add("sdk", device::sdkVersion());
    header.add("system", platform::systemDescription());
    header.add("time", text::timestampForDisplay(when));
    header.add("label", options.label.empty() ? std::string("(none)") : options.label);
    if (simulated)
        header.add("card", "SIMULATED (--simulate): the card numbers below are made up");
    else
        header.add("device selector", options.device);
}

int finishReport(const Options& options, const std::string& command, const std::tm& when, const Report& report,
                 const std::string& csv, bool interrupted)
{
    std::fputs(report.render(/*includeDetail*/ false).c_str(), stdout);

    int exitCode = interrupted ? 1 : 0;
    std::string error;
    const std::string textPath = joinPath(options.outputDir, reportFileName(command, options.label, when));
    if (writeTextFile(textPath, report.render(/*includeDetail*/ true), error))
        std::printf("\nReport: %s\n", textPath.c_str());
    else
    {
        std::fprintf(stderr, "\nduplex-probe: the report file was not written: %s\n", error.c_str());
        exitCode = 1;
    }
    if (!csv.empty())
    {
        const std::string csvPath = joinPath(options.outputDir, reportFileName(command, options.label, when, "csv"));
        if (writeTextFile(csvPath, csv, error))
            std::printf("Data:   %s\n", csvPath.c_str());
        else
        {
            std::fprintf(stderr, "duplex-probe: the data file was not written: %s\n", error.c_str());
            exitCode = 1;
        }
    }
    if (interrupted)
        std::printf("Interrupted: the results are partial.\n");
    return exitCode;
}

} // namespace ajaduplex::probe
