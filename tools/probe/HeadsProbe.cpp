// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "HeadsProbe.h"

#include "common/AlignedBuffer.h"
#include "common/TextFormat.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace ajaduplex::probe
{

namespace
{

using device::CardAudio;
using device::CardEvent;
using device::Ring;
using device::RingLayout;

constexpr double kNominalRate = 48000.0;
constexpr std::uint32_t kFillChunk = 64 * 1024;
constexpr std::size_t kMaxArrivals = 100000;

std::int64_t secondsToNs(double seconds)
{
    return static_cast<std::int64_t>(seconds * 1e9);
}

std::int64_t lapNs(const RingLayout& layout)
{
    return secondsToNs(static_cast<double>(layout.ringFrames()) / kNominalRate);
}

bool fillRing(CardAudio& card, Ring ring, std::uint32_t bytes, std::uint32_t word, std::string& error)
{
    AlignedBuffer fill(kFillChunk);
    std::uint32_t* words = fill.as<std::uint32_t>();
    std::fill(words, words + kFillChunk / 4, word);
    for (std::uint32_t offset = 0; offset < bytes; offset += kFillChunk)
    {
        const std::uint32_t n = std::min(kFillChunk, bytes - offset);
        if (!card.dmaWrite(ring, offset, fill.data(), n))
        {
            error = std::string("DMA write to the ") + (ring == Ring::Playback ? "playback" : "capture") +
                    " ring failed at offset " + text::hex32(offset);
            return false;
        }
    }
    return true;
}

struct WordCount
{
    unsigned zero = 0;
    unsigned marked = 0;   ///< equal to the word looked for
    unsigned other = 0;
};

WordCount countWords(const std::uint32_t* words, std::size_t n, std::uint32_t marker)
{
    WordCount c;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (words[i] == 0)
            ++c.zero;
        else if (words[i] == marker)
            ++c.marked;
        else
            ++c.other;
    }
    return c;
}

std::string percent(std::size_t part, std::size_t whole)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0);
    return buf;
}

std::string fixed(double value, int decimals)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

/// With its sign, and a value that rounds to zero (-0.0 included) as +0.
std::string signedFixed(double value, int decimals)
{
    std::string text = fixed(value, decimals);
    if (text[0] == '-' && text.find_first_not_of("-0.") == std::string::npos)
        text.erase(0, 1);
    return text[0] == '-' ? text : "+" + text;
}

template <typename Map>
std::vector<std::pair<std::uint32_t, std::size_t>> mostCommon(const Map& counts, std::size_t keep)
{
    std::vector<std::pair<std::uint32_t, std::size_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (v.size() > keep)
        v.resize(keep);
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Procedure
// ---------------------------------------------------------------------------

bool runHeads(CardAudio& card, Clock& clock, const HeadsSettings& settings,
              const std::function<bool()>& stopRequested, HeadsRun& run, std::string& error)
{
    auto stop = [&]() {
        if (!run.interrupted && stopRequested && stopRequested())
            run.interrupted = true;
        return run.interrupted;
    };

    // 1. Set-up
    run.description = card.description();
    const device::AudioConfig config;   // 16 channels, AES, capture enable, erase, Xpand select kept
    if (!card.configure(config, error))
        return false;
    run.layout = card.layout();
    const RingLayout& layout = run.layout;
    const std::uint32_t ringBytes = layout.ringBytes;
    const std::int64_t lap = lapNs(layout);

    if (!fillRing(card, Ring::Playback, ringBytes, kPlaybackMarker, error) ||
        !fillRing(card, Ring::Capture, ringBytes, kSentinel, error))
        return false;
    if (!card.startOutput())
    {
        error = "StartAudioOutput failed";
        return false;
    }
    if (!card.startInput())
    {
        error = "StartAudioInput failed";
        return false;
    }
    const std::int64_t started = clock.nowNs();

    // 2. Last byte or next, during the first lap of the capture ring
    {
        constexpr std::uint32_t kWindow = 1024;
        AlignedBuffer window(kWindow);
        const std::uint32_t* words = window.as<std::uint32_t>();
        const std::int64_t trialsEnd = started + lap * 3 / 4;
        std::uint32_t previous = 0;
        while (run.trials.size() < settings.sentinelTrials && clock.nowNs() < trialsEnd && !stop())
        {
            SentinelTrial trial;
            if (!card.readInputHead(trial.headBefore))
            {
                run.notes.push_back("ReadAudioLastIn failed during the sentinel trials");
                break;
            }
            if (trial.headBefore == 0)
            {
                if (clock.nowNs() - started > 200000000)
                {
                    run.notes.push_back("the input head stayed at 0 for 200 ms: no sentinel trials");
                    break;
                }
                clock.sleepForNs(100000);
                continue;
            }
            if (trial.headBefore < previous)
                break;   // wrapped: the sentinel has been overwritten
            previous = trial.headBefore;

            const std::uint32_t head = trial.headBefore & ~3u;
            const std::uint32_t start = head >= kWindow / 2 ? (head - kWindow / 2) & ~63u : 0u;
            const std::uint32_t length = std::min(kWindow, ringBytes - start);
            const std::int64_t before = clock.nowNs();
            if (!card.dmaRead(Ring::Capture, start, window.data(), length))
            {
                run.notes.push_back("DMA read failed during the sentinel trials");
                break;
            }
            trial.dmaNs = clock.nowNs() - before;
            card.readInputHead(trial.headAfter);

            auto state = [&](std::uint32_t at) {
                if (at < start || at + 4 > start + length)
                    return -1;
                return words[(at - start) / 4] == kSentinel ? 0 : 1;
            };
            trial.wordAtHead = state(head);
            trial.wordAfterHead = state(head + 4);
            run.trials.push_back(trial);
        }
    }

    // 3. Erase mode, half a lap in
    if (!stop())
    {
        clock.sleepUntilNs(started + lap / 2);
        constexpr std::uint32_t kSpan = 4096;
        std::uint32_t head = 0;
        if (card.readOutputHead(head) && head > kSpan + 64 && std::uint64_t(head) + ringBytes / 4 + kSpan <= ringBytes)
        {
            AlignedBuffer buffer(kSpan);
            const std::uint32_t behind = (head - 64 - kSpan) & ~63u;
            const std::uint32_t ahead = (head + ringBytes / 4) & ~63u;
            EraseCheck& e = run.erase;
            e.head = head;
            if (card.dmaRead(Ring::Playback, behind, buffer.data(), kSpan))
            {
                const WordCount c = countWords(buffer.as<std::uint32_t>(), kSpan / 4, kPlaybackMarker);
                e.behindZero = c.zero;
                e.behindMarker = c.marked;
                e.behindOther = c.other;
            }
            if (card.dmaRead(Ring::Playback, ahead, buffer.data(), kSpan))
            {
                const WordCount c = countWords(buffer.as<std::uint32_t>(), kSpan / 4, kPlaybackMarker);
                e.aheadZero = c.zero;
                e.aheadMarker = c.marked;
                e.aheadOther = c.other;
            }
            e.done = true;
        }
        else
        {
            run.notes.push_back("erase check skipped: the output head was at " + text::hex32(head) +
                                " half a lap in");
        }
    }

    // 4. Sampling
    if (!stop())
    {
        run.samples.reserve(std::min<std::size_t>(settings.maxSamples, std::size_t(1) << 20));
        run.readCost.reserve(std::min<std::size_t>(settings.maxCostValues, std::size_t(1) << 20));
        run.sampleStartNs = clock.nowNs();
        const std::int64_t end = run.sampleStartNs + secondsToNs(settings.sampleSeconds);
        HeadSample last;
        bool haveLast = false;
        bool full = false;
        unsigned failures = 0;
        std::int64_t now = run.sampleStartNs;
        while (now < end && !stop())
        {
            HeadSample sample;
            sample.tNs = now;
            const bool ok = card.readInputHead(sample.input) && card.readOutputHead(sample.output) &&
                            card.readSampleCounter(sample.counter);
            const std::int64_t after = clock.nowNs();
            if (!ok)
            {
                if (++failures == 100)
                {
                    run.notes.push_back("register reads kept failing; sampling stopped early");
                    break;
                }
                now = after;
                continue;
            }
            if (run.readCost.count() < settings.maxCostValues)
                run.readCost.add((after - now) / 3);
            // A sample is kept when a head moved; the counter value rides along.
            if (!haveLast || sample.input != last.input || sample.output != last.output)
            {
                if (run.samples.size() < settings.maxSamples)
                    run.samples.push_back(sample);
                else if (!full)
                {
                    full = true;
                    run.notes.push_back("sample buffer full after " + std::to_string(settings.maxSamples) +
                                        " changes; the rest of the sampling only measured read cost");
                }
                last = sample;
                haveLast = true;
            }
            now = after;
        }
        run.sampleEndNs = clock.nowNs();
    }

    // 5. Events, with audio running
    for (const CardEvent event : {CardEvent::Audio, CardEvent::AudioInWrap, CardEvent::AudioOutWrap})
    {
        if (stop())
            break;
        EventCheck check;
        check.event = event;
        check.subscribed = card.subscribe(event);
        if (check.subscribed)
        {
            check.haveCounts = card.interruptCount(event, check.countBefore);
            const std::int64_t begin = clock.nowNs();
            const std::int64_t end = begin + secondsToNs(settings.eventSeconds);
            while (clock.nowNs() < end && !stop())
            {
                if (card.waitForEvent(event, 100))
                {
                    ++check.arrived;
                    if (check.arrivalsNs.size() < kMaxArrivals)
                        check.arrivalsNs.push_back(clock.nowNs() - begin);
                }
                else
                {
                    ++check.timeouts;
                }
            }
            check.durationNs = clock.nowNs() - begin;
            if (check.haveCounts)
                check.haveCounts = card.interruptCount(event, check.countAfter);
            card.unsubscribe(event);
        }
        run.events.push_back(check);
    }

    // 6. Capture enable off
    if (!stop())
    {
        constexpr std::uint32_t kSpan = 64 * 1024;
        CaptureEnableCheck& c = run.captureEnable;
        std::string fillError;
        if (card.stopInput() && card.setCaptureEnable(false) &&
            fillRing(card, Ring::Capture, std::min(ringBytes, 4 * kSpan), kSentinel, fillError) && card.startInput())
        {
            std::uint32_t head0 = 0, head1 = 0;
            card.readInputHead(head0);
            clock.sleepForNs(200000000);
            card.readInputHead(head1);
            c.headMoved = head1 != head0;
            AlignedBuffer buffer(kSpan);
            if (card.dmaRead(Ring::Capture, 0, buffer.data(), kSpan))
            {
                const WordCount words = countWords(buffer.as<std::uint32_t>(), kSpan / 4, kSentinel);
                c.written = words.zero + words.other;
                c.untouched = words.marked;
                c.done = true;
            }
        }
        if (!c.done)
            run.notes.push_back("capture enable check skipped: " + (fillError.empty() ? std::string("the card refused a step") : fillError));
        card.stopInput();
        card.setCaptureEnable(true);
    }

    card.stopInput();
    card.stopOutput();
    return true;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

HeadMotion analyzeHead(const std::vector<HeadSample>& samples, bool input, const RingLayout& layout)
{
    HeadMotion m;
    if (samples.empty() || layout.ringBytes == 0 || layout.frameBytes == 0)
        return m;

    for (const HeadSample& s : samples)
        m.maxValue = std::max(m.maxValue, input ? s.input : s.output);

    // Values should stay below the wrap address. If the card reports more,
    // count steps modulo the next power of two instead, and say so.
    std::uint64_t modulus = layout.ringBytes;
    if (m.maxValue >= layout.ringBytes)
    {
        modulus = 1;
        while (modulus <= m.maxValue)
            modulus <<= 1;
    }

    std::map<std::uint32_t, std::size_t> steps, phases;
    std::uint32_t previous = input ? samples.front().input : samples.front().output;
    std::int64_t previousT = samples.front().tNs;
    std::int64_t firstChangeT = 0, lastChangeT = 0;
    std::uint64_t advancedSinceFirst = 0;

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const std::uint32_t value = input ? samples[i].input : samples[i].output;
        if (value == previous)
            continue;
        const std::uint64_t delta = (std::uint64_t(value) + modulus - previous) % modulus;
        const std::int64_t t = samples[i].tNs;
        if (delta > modulus / 2)
        {
            ++m.backwardSteps;
        }
        else
        {
            if (m.changes == 0)
                firstChangeT = t;   // the advance is counted from the first change on
            else
            {
                advancedSinceFirst += delta;
                m.intervalsNs.add(t - previousT);
            }
            ++m.changes;
            ++steps[static_cast<std::uint32_t>(delta)];
            ++phases[value % layout.frameBytes];
            lastChangeT = t;
            m.advancedBytes += delta;
        }
        previous = value;
        previousT = t;
    }

    m.steps = mostCommon(steps, 6);
    m.phases = mostCommon(phases, 4);
    m.elapsedSeconds = static_cast<double>(lastChangeT - firstChangeT) / 1e9;
    if (m.changes > 1 && m.elapsedSeconds > 0.0)
    {
        m.framesPerSecond = static_cast<double>(advancedSinceFirst) / layout.frameBytes / m.elapsedSeconds;
        m.ppm = (m.framesPerSecond / kNominalRate - 1.0) * 1e6;
    }
    return m;
}

CounterMotion analyzeCounter(const std::vector<HeadSample>& samples, const RingLayout& layout)
{
    CounterMotion c;
    if (samples.size() < 2 || layout.frameBytes == 0 || layout.ringBytes == 0)
        return c;

    // Counter rate over the whole sampling span.
    std::uint64_t ticks = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        ticks += static_cast<std::uint32_t>(samples[i].counter - samples[i - 1].counter);
    const double seconds = static_cast<double>(samples.back().tNs - samples.front().tNs) / 1e9;
    if (seconds <= 0.0 || ticks == 0)
        return c;
    c.ticksPerSecond = static_cast<double>(ticks) / seconds;
    c.ppm = (c.ticksPerSecond / kNominalRate - 1.0) * 1e6;

    // Input frames against counter ticks between the first and the last
    // input change, so both are measured over exactly the same samples.
    std::size_t first = samples.size(), last = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        if (samples[i].input != samples[i - 1].input)
        {
            if (first == samples.size())
                first = i;
            last = i;
        }
    }
    if (first < last)
    {
        std::uint64_t bytes = 0, spanTicks = 0;
        for (std::size_t i = first + 1; i <= last; ++i)
        {
            bytes += (std::uint64_t(samples[i].input) + layout.ringBytes - samples[i - 1].input) % layout.ringBytes;
            spanTicks += static_cast<std::uint32_t>(samples[i].counter - samples[i - 1].counter);
        }
        if (spanTicks > 0)
            c.inputFramesPerTick = static_cast<double>(bytes) / layout.frameBytes / static_cast<double>(spanTicks);
    }
    c.valid = true;
    return c;
}

SentinelSummary summarizeSentinel(const std::vector<SentinelTrial>& trials)
{
    SentinelSummary s;
    s.trials = trials.size();
    for (const SentinelTrial& t : trials)
    {
        s.dmaNs.add(t.dmaNs);
        if (t.wordAtHead < 0)
            ++s.outsideWindow;
        else if (t.wordAtHead == 0)
            ++s.atHeadSentinel;
        else
        {
            ++s.atHeadWritten;
            if (t.wordAfterHead == 0)
                ++s.afterHeadSentinel;
        }
    }

    const std::string of = " of " + std::to_string(s.trials) + " trials";
    if (s.trials == 0)
        s.conclusion = "no trials: the input head did not move during the first lap";
    else if (s.atHeadSentinel > 0)
        s.conclusion = "LastIn points at the NEXT byte to write: the word at the head was still the sentinel in " +
                       std::to_string(s.atHeadSentinel) + of;
    else if (s.afterHeadSentinel > 0)
        s.conclusion = "LastIn points at the LAST byte written: the word at the head was always written, and the "
                       "word after it was still the sentinel in " + std::to_string(s.afterHeadSentinel) + of;
    else
        s.conclusion = "inconclusive: the card had always written past the head before the DMA finished (median DMA " +
                       formatNs(s.dmaNs.percentile(50)) + ")";
    return s;
}

std::string eraseConclusion(const EraseCheck& e)
{
    if (!e.done)
        return "not checked";
    const unsigned behind = e.behindZero + e.behindMarker + e.behindOther;
    if (behind > 0 && e.behindZero == behind && e.aheadMarker > 0 && e.aheadZero == 0)
        return "erase mode works: what was played is zero, what was not played yet is untouched";
    if (e.behindMarker > 0)
        return "erase mode did NOT zero what was played: " + std::to_string(e.behindMarker) + " of " +
               std::to_string(behind) + " words behind the head still hold the marker";
    if (e.aheadZero > 0)
        return "zeroes ahead of the head as well: something cleared more than what was played";
    return "unclear: see the counts";
}

std::string captureEnableConclusion(const CaptureEnableCheck& c)
{
    if (!c.done)
        return "not checked";
    if (c.written == 0)
        return std::string("with capture enable off the input head ") + (c.headMoved ? "moves" : "does not move") +
               " and nothing reaches memory: start() must call SetAudioCaptureEnable(true)";
    if (c.headMoved)
        return "capture reaches memory without capture enable: SetAudioCaptureEnable(true) is harmless but not required";
    return "memory was written although the head did not move: unexpected, see the counts";
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

namespace
{

void addHeadSection(Report& report, const char* title, const HeadMotion& m, const RingLayout& layout,
                    std::int64_t pollNs)
{
    Report::Section& s = report.addSection(title);
    s.add("changes", std::to_string(m.changes));
    if (m.changes == 0)
    {
        s.add("motion", "none: the head did not move while sampling");
        return;
    }

    std::string steps;
    for (const auto& [bytes, count] : m.steps)
    {
        if (!steps.empty())
            steps += "\n";
        steps += std::to_string(bytes) + " bytes";
        if (bytes % layout.frameBytes == 0)
            steps += " (" + std::to_string(bytes / layout.frameBytes) + " frame" + (bytes == layout.frameBytes ? "" : "s") + ")";
        else
            steps += " (not a whole frame)";
        steps += ": " + std::to_string(count) + " times, " + percent(count, m.changes);
    }
    s.add("steps", steps);

    std::string phases;
    for (const auto& [phase, count] : m.phases)
    {
        if (!phases.empty())
            phases += ", ";
        phases += std::to_string(phase) + ": " + percent(count, m.changes);
    }
    s.add("value mod " + std::to_string(layout.frameBytes), phases);

    if (m.framesPerSecond > 0.0)
    {
        // A change is seen up to one loop of three reads late, at both ends.
        std::string rate = fixed(m.framesPerSecond, 2) + " frames/s, " + signedFixed(m.ppm, 1) + " ppm";
        if (pollNs > 0 && m.elapsedSeconds > 0.0)
            rate += " (+/- " + fixed(2.0 * static_cast<double>(pollNs) / (m.elapsedSeconds * 1e9) * 1e6, 1) + ")";
        rate += " against 48 kHz on this PC's clock, over " + fixed(m.elapsedSeconds, 2) + " s";
        s.add("rate", rate);
    }
    s.add("time between changes", summarizeNs(m.intervalsNs));
    if (m.backwardSteps > 0)
        s.add("backward steps", std::to_string(m.backwardSteps));
    if (m.maxValue >= layout.ringBytes)
        s.add("beyond the wrap", "values up to " + text::hex32(m.maxValue) + ", wrap address " + text::hex32(layout.ringBytes));
}

std::string eventLine(const EventCheck& e)
{
    if (!e.subscribed)
        return "subscribe failed: not available";
    const double seconds = static_cast<double>(e.durationNs) / 1e9;
    std::string line = std::to_string(e.arrived) + " in " + fixed(seconds, 2) + " s";
    if (seconds > 0.0)
        line += " (" + fixed(e.arrived / seconds, 2) + "/s)";
    if (e.arrivalsNs.size() > 1)
    {
        Distribution intervals;
        for (std::size_t i = 1; i < e.arrivalsNs.size(); ++i)
            intervals.add(e.arrivalsNs[i] - e.arrivalsNs[i - 1]);
        line += "\nintervals: " + summarizeNs(intervals);
    }
    if (e.haveCounts)
        line += "\ndriver interrupt count: +" + std::to_string(e.countAfter - e.countBefore);
    return line;
}

} // namespace

void addHeadsReport(const HeadsRun& run, Report& report)
{
    const RingLayout& layout = run.layout;

    Report::Section& overview = report.addSection("heads");
    overview.add("device", run.description);
    overview.add("ring", std::to_string(layout.ringBytes) + " bytes, " + std::to_string(layout.ringFrames()) +
                             " frames of " + std::to_string(layout.frameBytes) + " bytes, one lap " +
                             formatNs(lapNs(layout)));
    overview.add("capture offset", text::hex32(layout.captureOffset));
    overview.add("sampling", formatNs(run.sampleEndNs - run.sampleStartNs) + ", " +
                                 std::to_string(run.samples.size()) + " changes kept");
    overview.add("register read", summarizeNs(run.readCost));
    if (run.samples.size() > kHeadsCsvRows)
        overview.add("data file", "the first " + std::to_string(kHeadsCsvRows) + " changes (the summary uses all)");
    if (run.interrupted)
        overview.add("interrupted", "yes (Ctrl+C): the results are partial");

    const HeadMotion inputMotion = analyzeHead(run.samples, true, layout);
    const HeadMotion outputMotion = analyzeHead(run.samples, false, layout);
    const std::int64_t pollNs = 3 * run.readCost.percentile(99);   // one loop of three reads
    addHeadSection(report, "input head (LastIn)", inputMotion, layout, pollNs);
    addHeadSection(report, "output head (LastOut)", outputMotion, layout, pollNs);

    Report::Section& counter = report.addSection("48 kHz counter");
    const CounterMotion c = analyzeCounter(run.samples, layout);
    if (c.valid)
    {
        counter.add("rate", fixed(c.ticksPerSecond, 2) + " ticks/s, " + signedFixed(c.ppm, 1) + " ppm on this PC's clock");
        if (c.inputFramesPerTick > 0.0)
            counter.add("input frames per tick", fixed(c.inputFramesPerTick, 6) + " (" +
                                                     signedFixed((c.inputFramesPerTick - 1.0) * 1e6, 1) + " ppm)");
    }
    else
    {
        counter.add("rate", "n/a");
    }

    const SentinelSummary sentinel = summarizeSentinel(run.trials);
    Report::Section& last = report.addSection("last byte or next");
    last.add("trials", std::to_string(sentinel.trials));
    last.add("word at head", std::to_string(sentinel.atHeadWritten) + " written, " +
                                 std::to_string(sentinel.atHeadSentinel) + " still sentinel, " +
                                 std::to_string(sentinel.outsideWindow) + " outside the window");
    last.add("word after head", std::to_string(sentinel.afterHeadSentinel) + " still sentinel");
    last.add("DMA of 1 KB", summarizeNs(sentinel.dmaNs));
    last.add("conclusion", sentinel.conclusion);

    Report::Section& erase = report.addSection("erase mode");
    if (run.erase.done)
    {
        erase.add("output head", text::hex32(run.erase.head));
        erase.add("4 KB behind", std::to_string(run.erase.behindZero) + " zero, " + std::to_string(run.erase.behindMarker) +
                                     " marker, " + std::to_string(run.erase.behindOther) + " other");
        erase.add("4 KB a quarter ring ahead", std::to_string(run.erase.aheadZero) + " zero, " +
                                                   std::to_string(run.erase.aheadMarker) + " marker, " +
                                                   std::to_string(run.erase.aheadOther) + " other");
    }
    erase.add("conclusion", eraseConclusion(run.erase));

    Report::Section& events = report.addSection("events");
    if (run.events.empty())
        events.add("events", "not checked");
    for (const EventCheck& e : run.events)
        events.add(device::toString(e.event), eventLine(e));

    Report::Section& capture = report.addSection("capture enable off");
    if (run.captureEnable.done)
    {
        capture.add("head moved", text::yesNo(run.captureEnable.headMoved));
        capture.add("first 64 KB", std::to_string(run.captureEnable.written) + " words written, " +
                                       std::to_string(run.captureEnable.untouched) + " still sentinel");
    }
    capture.add("conclusion", captureEnableConclusion(run.captureEnable));

    if (!run.notes.empty())
    {
        Report::Section& notes = report.addSection("notes");
        std::string all;
        for (const std::string& note : run.notes)
            all += (all.empty() ? "" : "\n") + note;
        notes.block(all);
    }
}

std::string headsCsv(const HeadsRun& run, std::size_t maxRows)
{
    std::string csv = "t_ns,last_in,last_out,counter\n";
    const std::size_t rows = std::min(maxRows, run.samples.size());
    csv.reserve(csv.size() + rows * 36);
    char line[96];
    for (std::size_t i = 0; i < rows; ++i)
    {
        const HeadSample& s = run.samples[i];
        std::snprintf(line, sizeof(line), "%lld,%u,%u,%u\n", static_cast<long long>(s.tNs - run.sampleStartNs),
                      static_cast<unsigned>(s.input), static_cast<unsigned>(s.output), static_cast<unsigned>(s.counter));
        csv += line;
    }
    return csv;
}

} // namespace ajaduplex::probe
