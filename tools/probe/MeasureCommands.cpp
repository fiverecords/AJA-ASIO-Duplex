// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "MeasureCommands.h"

#include "CommandSupport.h"
#include "CostProbe.h"
#include "HeadsProbe.h"
#include "LoopProbe.h"
#include "RoutingProbe.h"
#include "WatchProbe.h"
#include "common/TextFormat.h"
#include "platform/Realtime.h"
#include "sim/FakeCardAudio.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>

namespace ajaduplex::probe
{

namespace
{

bool stopRequested()
{
    return platform::interruptRequested();
}

/// A line a minute while a long loop run goes on. It prints from a thread
/// of its own, so the engine's thread never waits on the console.
class ProgressPrinter
{
public:
    explicit ProgressPrinter(const LoopProgress& progress) : progress_(progress), thread_([this] { run(); }) {}
    ~ProgressPrinter()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    ProgressPrinter(const ProgressPrinter&) = delete;
    ProgressPrinter& operator=(const ProgressPrinter&) = delete;

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (unsigned minutes = 1;; ++minutes)
        {
            if (wake_.wait_for(lock, std::chrono::minutes(1), [this] { return done_; }))
                return;
            const std::uint64_t problems = progress_.problems.load(std::memory_order_relaxed);
            if (!progress_.allLocked.load(std::memory_order_relaxed) && problems == 0)
                std::printf("  %3u min: a looped input has not heard its output yet (cables?)\n", minutes);
            else if (problems == 0)
                std::printf("  %3u min: clean so far\n", minutes);
            else
                std::printf("  %3u min: %llu problems so far (Ctrl+C ends it; the report says which)\n", minutes,
                            static_cast<unsigned long long>(problems));
            std::fflush(stdout);
        }
    }

    const LoopProgress& progress_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool done_ = false;
    std::thread thread_;   // last: starts once the rest is ready
};

engine::HeadMode toHeadMode(HeadChoice choice)
{
    switch (choice)
    {
        case HeadChoice::Conservative: return engine::HeadMode::Conservative;
        case HeadChoice::LastByte:     return engine::HeadMode::LastByte;
        case HeadChoice::NextByte:     return engine::HeadMode::NextByte;
    }
    return engine::HeadMode::Conservative;
}

} // namespace

int runHeadsCommand(const Options& options)
{
    platform::installInterruptHandler();
    const std::tm now = text::localTime(std::time(nullptr));

    CardSession session;
    std::string error;
    if (!openCardSession(options, session, error))
    {
        std::fprintf(stderr, "duplex-probe: %s\n", error.c_str());
        return 1;
    }
    if (session.simulated)
        std::printf("heads on %s\n\n", session.card->description().c_str());
    else
        std::printf("heads on %s\nAbout %.0f s. Ctrl+C stops it and puts the audio system back.\n\n",
                    session.card->description().c_str(), options.seconds + 12.0);
    std::fflush(stdout);

    HeadsSettings settings;
    settings.sampleSeconds = options.seconds;
    HeadsRun run;
    const bool ok = runHeads(*session.card, *session.clock, settings, stopRequested, run, error);
    session.card->restore();
    if (!ok)
    {
        std::fprintf(stderr, "duplex-probe: heads: %s (the audio system has been put back)\n", error.c_str());
        return 1;
    }

    Report report;
    addProbeHeader(report, options, now, session.simulated);
    addHeadsReport(run, report);
    return finishReport(options, "heads", now, report, headsCsv(run), run.interrupted);
}

int runCostCommand(const Options& options)
{
    platform::installInterruptHandler();
    const std::tm now = text::localTime(std::time(nullptr));

    CardSession session;
    std::string error;
    if (!openCardSession(options, session, error))
    {
        std::fprintf(stderr, "duplex-probe: %s\n", error.c_str());
        return 1;
    }
    std::printf("cost on %s\nAbout %s s. Ctrl+C stops it%s.\n\n", session.card->description().c_str(),
                session.simulated ? "8" : "15", session.simulated ? "" : " and puts the audio system back");
    std::fflush(stdout);

    CostSettings settings;
    CostRun run;
    {
        // The whole measurement runs the way the engine's thread will.
        platform::ProAudioScope priority;
        const bool ok = runCardCost(*session.card, *session.clock, settings, stopRequested, run, error);
        session.card->restore();
        if (!ok)
        {
            std::fprintf(stderr, "duplex-probe: cost: %s (the audio system has been put back)\n", error.c_str());
            return 1;
        }
        // The wake-up timer is this PC's, simulated card or not.
        platform::SystemClock realClock;
        run.timerDescription = std::string(realClock.timer().kind()) + ", " + priority.description() +
                               (session.simulated ? " (real: this PC)" : "");
        runWakeCost(realClock, settings, stopRequested, run);
    }

    Report report;
    addProbeHeader(report, options, now, session.simulated);
    addCostReport(run, report);
    return finishReport(options, "cost", now, report, costCsv(run), run.interrupted);
}

int runLoopCommand(const Options& options)
{
    platform::installInterruptHandler();
    const std::tm now = text::localTime(std::time(nullptr));

    CardSession session;
    std::string error;
    const bool analog = options.path == PathChoice::Analog;
    if (!openCardSession(options, session, error, analog ? SimulatedSetup::AnalogLoop : SimulatedSetup::AesLoop))
    {
        std::fprintf(stderr, "duplex-probe: %s\n", error.c_str());
        return 1;
    }

    const std::vector<std::pair<unsigned, unsigned>> grid = sweepGrid(options.frames, options.guard);
    const double total = static_cast<double>(grid.size()) * (options.seconds + 0.5);
    std::printf("loop on %s\n", session.card->description().c_str());
    if (!session.simulated)
    {
        if (analog)
            std::printf("%zu run(s) of %.0f s, about %.0f min. XLR out L/R must be cabled to XLR in L/R, and\n"
                        "nothing else on those outputs: they play -20 dBFS tones. Ctrl+C stops it and puts\n"
                        "the audio system back.\n",
                        grid.size(), options.seconds, total / 60.0 < 1.0 ? 1.0 : total / 60.0);
        else if (options.path == PathChoice::Aes)
            std::printf("%zu run(s) of %.0f s, about %.0f min. AES out to AES in must be cabled; the looped\n"
                        "outputs play -40 dBFS tones, and the XLR outputs play AES 1-2's too. Ctrl+C stops\n"
                        "it and puts the audio system back.\n",
                        grid.size(), options.seconds, total / 60.0 < 1.0 ? 1.0 : total / 60.0);
        else
            std::printf("%zu run(s) of %.0f s, about %.0f min. AES out to AES in must be cabled; the looped\n"
                        "outputs carry a quiet test signal. Ctrl+C stops it and puts the audio system back.\n",
                        grid.size(), options.seconds, total / 60.0 < 1.0 ? 1.0 : total / 60.0);
    }
    std::printf("\n");
    std::fflush(stdout);

    LoopRun run;
    run.description = session.card->description();
    {
        // Every run on the thread priority the driver's engine will have.
        platform::ProAudioScope priority;
        for (const auto& [frames, guard] : grid)
        {
            LoopSettings settings;
            settings.frames = frames;
            settings.guard = guard;
            settings.seconds = options.seconds;
            settings.channels = options.channels;
            settings.headMode = toHeadMode(options.head);
            settings.oemTasks = options.oemTasks;
            settings.path = analog                                   ? LoopPath::Analog
                            : options.path == PathChoice::AesWords ? LoopPath::AesWords
                                                                   : LoopPath::Aes;

            // Runs of two minutes or more on the card say how they go.
            const bool withProgress = !session.simulated && options.seconds >= 120.0;
            std::printf("N %4u, G %3u: ", frames, guard);
            if (withProgress)
                std::printf("running, a line a minute\n");
            std::fflush(stdout);
            LoopProgress progress;
            LoopResult result;
            {
                std::unique_ptr<ProgressPrinter> printer;
                if (withProgress)
                    printer = std::make_unique<ProgressPrinter>(progress);
                result = runLoop(*session.card, *session.clock, settings, stopRequested, &progress);
            }
            if (withProgress)
                std::printf("N %4u, G %3u: ", frames, guard);
            std::printf("%s\n", loopSummaryLine(result).c_str());
            std::fflush(stdout);

            const bool started = result.started;
            run.interrupted = run.interrupted || result.interrupted;
            run.results.push_back(std::move(result));
            if (!started || run.interrupted)
                break;   // the next runs would not start either, or were not wanted
        }
    }
    session.card->restore();
    std::printf("\n");

    Report report;
    addProbeHeader(report, options, now, session.simulated);
    addLoopReport(run, report);
    return finishReport(options, "loop", now, report, loopCsv(run), run.interrupted);
}

int runRoutingCommand(const Options& options)
{
    platform::installInterruptHandler();
    const std::tm now = text::localTime(std::time(nullptr));

    CardSession session;
    std::string error;
    if (!openCardSession(options, session, error, SimulatedSetup::AnalogLoop))
    {
        std::fprintf(stderr, "duplex-probe: %s\n", error.c_str());
        return 1;
    }
    std::printf("routing on %s\n", session.card->description().c_str());
    if (!session.simulated)
        std::printf("AES out 1-8 looped to AES in 1-8 and XLR out L/R to XLR in L/R; nothing else on the XLR\n"
                    "outputs: every output plays a -20 dBFS tone. About %s. Ctrl+C stops it and puts the\n"
                    "audio system back.\n",
                    options.scanSelect ? "8 s" : "2 s");
    std::fflush(stdout);

    const RoutingRun run = runRouting(*session.card, *session.clock, options.scanSelect, stopRequested,
                                      options.oemTasks);
    session.card->restore();

    Report report;
    addProbeHeader(report, options, now, session.simulated);
    addRoutingReport(run, report);
    return finishReport(options, "routing", now, report, routingCsv(run), run.interrupted);
}

int runWatchCommand(const Options& options)
{
    platform::installInterruptHandler();
    const std::tm now = text::localTime(std::time(nullptr));

    CardSession session;
    std::string error;
    if (!openCardSession(options, session, error))
    {
        std::fprintf(stderr, "duplex-probe: %s\n", error.c_str());
        return 1;
    }
    std::printf("watch on %s\n", session.card->description().c_str());

    // On the simulated card nobody else is there: another program is
    // played, taking AES out 1-4 at 40%% of the run and setting the Xpand's
    // select to AES at 70%%, so the run shows what a change looks like.
    std::function<void(double)> meanwhile;
    auto* fake = dynamic_cast<sim::FakeCardAudio*>(session.card.get());
    if (session.simulated && fake != nullptr)
    {
        std::printf("Simulated: another program takes AES out 1-4 at %.0f s and sets the Xpand's select to AES at "
                    "%.0f s.\n",
                    0.4 * options.seconds, 0.7 * options.seconds);
        const double total = options.seconds;
        meanwhile = [fake, total, step = 0](double seconds) mutable {
            device::CardRouting theirs = fake->routing();
            if (step == 0 && seconds >= 0.4 * total)
            {
                theirs.aes[0] = device::OutputSource{1, 1};
                fake->anotherProgramSets(theirs, device::routing::kAesGroup);
                step = 1;
            }
            else if (step == 1 && seconds >= 0.7 * total)
            {
                theirs.xpandSelect = 0;
                fake->anotherProgramSets(theirs, device::routing::kXpandSelect);
                step = 2;
            }
        };
    }
    else
    {
        std::printf("About %.0f s. The audio system plays silence while its routing is read back once a second, as\n"
                    "the driver does. Leave the card alone to see whether AJA's services change anything by\n"
                    "themselves (Q9), or change things in the AJA Control Panel to see what the driver will\n"
                    "report. Ctrl+C stops it and puts the audio system back.\n",
                    options.seconds);
    }
    std::printf("\n");
    std::fflush(stdout);

    WatchSettings settings;
    settings.seconds = options.seconds;
    settings.analog = options.analogInput;
    settings.keep = options.keep;
    WatchRun run;
    {
        // The engine as the driver's runs it, at its priority. The rare
        // line a change brings is printed from this thread: no host listens.
        platform::ProAudioScope priority;
        run = runWatch(*session.card, *session.clock, settings, stopRequested, meanwhile,
                       [](const WatchEvent& event) {
                           std::printf("%8.1f s  %s\n", event.seconds, event.line.c_str());
                           std::fflush(stdout);
                       });
    }
    session.card->restore();
    if (!run.started)
    {
        std::fprintf(stderr, "duplex-probe: watch: %s (the audio system has been put back)\n", run.error.c_str());
        return 1;
    }
    if (run.events.empty())
        std::printf("%8.1f s  no change\n", run.seconds);
    std::printf("\n");

    Report report;
    addProbeHeader(report, options, now, session.simulated);
    addWatchReport(run, report);
    return finishReport(options, "watch", now, report, std::string(), run.interrupted);
}

} // namespace ajaduplex::probe
