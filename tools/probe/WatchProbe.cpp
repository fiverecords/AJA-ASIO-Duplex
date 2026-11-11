// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "WatchProbe.h"

#include "engine/RoutingWatch.h"

#include <cstdio>

namespace ajaduplex::probe
{

namespace
{

constexpr std::int64_t kCheckNs = 1000000000;   // as the driver: once a second

unsigned bitCount(unsigned bits)
{
    unsigned n = 0;
    for (; bits != 0; bits &= bits - 1)
        ++n;
    return n;
}

/// Silence out, nothing in, and the routing watch after the blocks.
class WatchClient final : public engine::EngineClient
{
public:
    WatchClient(engine::RoutingWatch& watch, WatchRun& run, const std::function<void(double)>& meanwhile,
                const std::function<void(const WatchEvent&)>& onEvent)
        : watch_(watch), run_(run), meanwhile_(meanwhile), onEvent_(onEvent)
    {
    }

    void begin(std::int64_t nowNs)
    {
        startNs_ = nowNs;
        lastCheckNs_ = nowNs;
    }

    engine::BlockBuffers buffers(unsigned) override { return engine::BlockBuffers{}; }
    void process(const engine::BlockInfo&, unsigned) override {}

    void afterBlock(const engine::BlockInfo& info) override
    {
        seconds_ = static_cast<double>(info.hostTimeNs - startNs_) / 1e9;
        if (meanwhile_)
            meanwhile_(seconds_);
        if (info.hostTimeNs - lastCheckNs_ < kCheckNs)
            return;
        lastCheckNs_ = info.hostTimeNs;
        const engine::RoutingWatch::Check c = watch_.check(run_.settings.keep);
        if (!c.read)
            return;
        ++run_.checks;
        run_.changes += engine::reportCheck(watch_, c, "--keep", &WatchClient::emit, this);
        run_.otherChanges += bitCount(c.changed & device::routingParts(c.now) & ~watch_.owned() &
                                      ~device::routing::kTaskMode);
        const unsigned putBack = c.putBack & ~c.putBackFailed;
        run_.putBack += bitCount(putBack);
        run_.notOursAtEnd = c.notOurs & ~putBack;
    }

private:
    static void emit(void* context, const char* line)
    {
        auto* self = static_cast<WatchClient*>(context);
        WatchEvent event;
        event.seconds = self->seconds_;
        event.line = line;
        self->run_.events.push_back(event);
        if (self->onEvent_)
            self->onEvent_(event);
    }

    engine::RoutingWatch& watch_;
    WatchRun& run_;
    const std::function<void(double)>& meanwhile_;
    const std::function<void(const WatchEvent&)>& onEvent_;
    std::int64_t startNs_ = 0;
    std::int64_t lastCheckNs_ = 0;
    double seconds_ = 0.0;
};

std::string seconds(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f s", value);
    return text;
}

} // namespace

WatchRun runWatch(device::CardAudio& card, Clock& clock, const WatchSettings& settings,
                  const std::function<bool()>& stop, const std::function<void(double)>& meanwhile,
                  const std::function<void(const WatchEvent&)>& onEvent)
{
    WatchRun run;
    run.settings = settings;
    run.description = card.description();

    // The driver's set-up (DriverCore::start).
    engine::EngineConfig config;
    config.blockFrames = settings.frames;
    config.guardFrames = settings.guard;
    config.audio.channels = 16;
    config.audio.source = settings.analog ? device::InputSource::Analog : device::InputSource::Aes;
    config.audio.keepXpandSelect = false;
    config.audio.routeAesOutputs = true;
    config.audio.routeMonitor = true;
    config.audio.monitorPair = 0;

    engine::AudioEngine engine(card, clock);
    engine::RoutingWatch watch(card);
    WatchClient client(watch, run, meanwhile, onEvent);
    if (!engine.start(config, client, run.error))
        return run;
    run.started = true;
    // The analog level is left as the card has it: reported, not kept.
    run.watched = watch.takeBaseline(device::routing::kRouting & ~device::routing::kAnalogLevel);
    run.baseline = watch.baseline();
    const std::int64_t startNs = clock.nowNs();
    client.begin(startNs);
    const std::int64_t endNs = startNs + static_cast<std::int64_t>(settings.seconds * 1e9);
    while (clock.nowNs() < endNs)
    {
        if (stop && stop())
        {
            run.interrupted = true;
            break;
        }
        if (!engine.runOnce())
            break;
    }
    run.seconds = static_cast<double>(clock.nowNs() - startNs) / 1e9;
    run.stats = engine.stats();
    run.last = watch.last();
    engine.stop();
    return run;
}

void addWatchReport(const WatchRun& run, Report& report)
{
    Report::Section& s = report.addSection("watch");
    s.add("card", run.description);
    s.add("input", std::string(run.settings.analog ? "analog" : "AES") + ", as the driver sets it up");
    s.add("keep", run.settings.keep ? "on: what another program changed was put back, as Keep my routing does"
                                    : "off: changes are only reported (--keep puts them back)");
    if (!run.started)
    {
        s.add("result", "did not start: " + run.error);
        return;
    }
    s.add("ran", seconds(run.seconds) + ", N " + std::to_string(run.settings.frames) + ", G " +
                     std::to_string(run.settings.guard) + ", silence on the outputs" +
                     (run.interrupted ? " (interrupted)" : ""));
    if (!run.watched)
    {
        s.add("routing", "the card could not read it back");
        return;
    }
    s.add("routing at start", engine::describeRouting(run.baseline));
    s.add("checks", std::to_string(run.checks) + ", once a second");
    s.add("changes", std::to_string(run.changes) + " by other programs, " + std::to_string(run.putBack) +
                         " parts put back" +
                         (run.otherChanges == 0 ? std::string()
                                                : ", and " + std::to_string(run.otherChanges) +
                                                      " to the analog level, which it leaves as the card has it"));
    if (run.notOursAtEnd != 0)
        s.add("not the driver's at the end", engine::routingPartNames(run.notOursAtEnd));
    if (device::routingDifferences(run.baseline, run.last) != 0)
        s.add("routing at the end", engine::describeRouting(run.last));
    if (!run.events.empty())
    {
        std::string lines;
        for (const WatchEvent& e : run.events)
        {
            char at[24];
            std::snprintf(at, sizeof(at), "%8.1f s  ", e.seconds);
            lines += at + e.line + "\n";
        }
        s.block(lines);
    }
    s.add("engine", std::to_string(run.stats.blocks) + " blocks, " + std::to_string(run.stats.lateOutputs) +
                        " late outputs, " + std::to_string(run.stats.outputMoves) +
                        " output moves (out of step with the input, placed anew), " +
                        std::to_string(run.stats.inputOverruns) + " overruns, " + std::to_string(run.stats.stalls) +
                        " stalls");

    Report::Section& a = report.addSection("answers");
    const device::TaskMode mode = run.baseline.haveTaskMode ? run.baseline.taskMode : device::TaskMode::Unknown;
    const unsigned changed = run.changes + run.otherChanges;
    std::string q9;
    if (mode != device::TaskMode::Standard)
        q9 = std::string("not answered: the card was in ") + device::toString(mode) +
             " task mode; run it again in standard mode (AJA Control Panel), with nothing else open";
    else if (changed == 0 && run.seconds >= 60.0)
        q9 = "nothing rewrote the routing in " + seconds(run.seconds) +
             " of standard task mode: if nobody touched the card meanwhile, AJA's services leave the audio "
             "routing alone, and the driver needs no OEM mode for it";
    else if (changed == 0)
        q9 = "nothing changed, but " + seconds(run.seconds) + " is short: run it for five minutes (--seconds 300)";
    else
        q9 = "the routing changed " + std::to_string(changed) + (changed == 1 ? " time" : " times") +
             " in standard task mode: if nobody touched the card meanwhile, AJA's services rewrote it, and the "
             "driver will need OEM mode while it plays (or Keep my routing)";
    a.add("Q9", q9);
}

} // namespace ajaduplex::probe
