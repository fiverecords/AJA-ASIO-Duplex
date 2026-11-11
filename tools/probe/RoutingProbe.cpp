// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "RoutingProbe.h"

#include "engine/RingMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ajaduplex::probe
{

namespace
{

constexpr double kPi = 3.141592653589793;
constexpr std::int64_t kRate = 48000;
constexpr double kFullScale = 2147483648.0;
constexpr std::size_t kCaptureFrames = 5 * RoutingSignal::kWindowFrames;   // 0.2 s
constexpr std::uint32_t kMarginFrames = 480;          // left out before the head
constexpr std::int64_t kSettleNs = 500000000;         // after a start
constexpr std::int64_t kSelectSettleNs = 350000000;   // after a select change
constexpr unsigned kInputs = 8;                       // AES in 1-8; the analog comes in on 1-2
constexpr double kExactResidualDbfs = -125.0;         // under it, the input is bit-exact (digital)
constexpr unsigned kMonitorOn1516 = 7;
constexpr unsigned kMonitorOn12 = 0;

const std::vector<double>& sineTable()
{
    static const std::vector<double> table = [] {
        std::vector<double> t(static_cast<std::size_t>(kRate));
        for (std::size_t m = 0; m < t.size(); ++m)
            t[m] = std::sin(2.0 * kPi * static_cast<double>(m) / static_cast<double>(kRate));
        return t;
    }();
    return table;
}

std::size_t phaseIndex(unsigned hz, std::int64_t frame)
{
    const std::int64_t m = (static_cast<std::int64_t>(hz) * frame) % kRate;
    return static_cast<std::size_t>(m < 0 ? m + kRate : m);
}

double decibels(double ratio)
{
    return ratio > 0.0 ? 20.0 * std::log10(ratio) : -200.0;
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

bool exact(const ToneLevels& levels)
{
    return levels.residualDbfs < kExactResidualDbfs;
}

bool hears(const ToneLevels& levels, unsigned output)
{
    return output < RoutingSignal::kChannels && levels.dbfs[output] >= ToneLevels::kPresentDbfs;
}

/// AES in `input` hears AES out `input`: word for word, or resampled on the
/// way, as the KONA X's AES inputs do (2026-11-10), its tone the loudest.
bool ownOutput(const ToneLevels& levels, unsigned input)
{
    if (!hears(levels, input))
        return false;
    const std::vector<unsigned> heard = levels.heard();
    return exact(levels) || (!heard.empty() && heard.front() == input);
}

// ---------------------------------------------------------------------------
// One set-up
// ---------------------------------------------------------------------------

bool fillPlayback(device::CardAudio& card, std::string& error)
{
    const device::RingLayout layout = card.layout();
    const unsigned channels = layout.frameBytes / 4;
    const std::uint32_t frames = layout.ringFrames();
    const std::uint32_t chunk = 1024;
    std::vector<std::int32_t> buffer(static_cast<std::size_t>(chunk) * channels);
    for (std::uint32_t first = 0; first < frames; first += chunk)
    {
        const std::uint32_t count = std::min(chunk, frames - first);
        for (std::uint32_t f = 0; f < count; ++f)
            for (unsigned c = 0; c < channels; ++c)
                buffer[static_cast<std::size_t>(f) * channels + c] =
                    c < RoutingSignal::kChannels ? RoutingSignal::sample(first + f, c) : 0;
        if (!card.dmaWrite(device::Ring::Playback, first * layout.frameBytes, buffer.data(), count * layout.frameBytes))
        {
            error = "DMA to the playback ring failed";
            return false;
        }
    }
    return true;
}

/// The last kCaptureFrames frames the card has written, measured.
bool measureInputs(device::CardAudio& card, RoutingCapture& capture)
{
    const device::RingLayout layout = card.layout();
    const engine::RingGeometry ring(layout.ringBytes, layout.frameBytes);
    const unsigned channels = layout.frameBytes / 4;
    std::uint32_t head = 0;
    if (!card.readInputHead(head))
    {
        capture.error = "reading the input head failed";
        return false;
    }
    const std::uint32_t end = ring.frameFloor(ring.dataEnd(head, engine::HeadMode::Conservative));
    const std::uint32_t bytes = static_cast<std::uint32_t>(kCaptureFrames) * layout.frameBytes;
    const std::uint32_t start = ring.retreat(end, bytes + kMarginFrames * layout.frameBytes);

    std::vector<std::int32_t> frames(kCaptureFrames * channels);
    engine::Segment segments[2];
    const int count = ring.split(start, bytes, segments);
    auto* host = reinterpret_cast<std::uint8_t*>(frames.data());
    for (int i = 0; i < count; ++i)
    {
        if (!card.dmaRead(device::Ring::Capture, segments[i].offset, host, segments[i].bytes))
        {
            capture.error = "DMA from the capture ring failed";
            return false;
        }
        host += segments[i].bytes;
    }
    capture.inputs.clear();
    for (unsigned input = 0; input < kInputs && input < channels; ++input)
        capture.inputs.push_back(measureTones(frames.data(), channels, input, kCaptureFrames));
    capture.done = true;
    return true;
}

bool stopBoth(device::CardAudio& card)
{
    const bool in = card.stopInput();
    const bool out = card.stopOutput();
    return in && out;
}

/// Configures, fills the ring, starts both sides and lets them settle.
bool startSetup(device::CardAudio& card, Clock& clock, device::InputSource source, int monitorPair, bool oemTasks,
                std::string& error)
{
    device::AudioConfig config;
    if (oemTasks)
        config.taskMode = device::TaskMode::Oem;   // AJA's services keep off the routing meanwhile
    config.channels = RoutingSignal::kChannels;
    config.source = source;
    config.keepXpandSelect = false;        // the input must be what we ask for
    config.routeAesOutputs = true;
    config.routeMonitor = monitorPair >= 0;
    config.monitorPair = monitorPair >= 0 ? static_cast<unsigned>(monitorPair) : 0;
    config.eraseMode = false;              // the ring loops: nothing may wipe it
    stopBoth(card);
    if (!card.configure(config, error) || !fillPlayback(card, error))
        return false;
    if (!card.startOutput() || !card.startInput())
    {
        error = "starting the audio system failed";
        return false;
    }
    clock.sleepForNs(kSettleNs);
    return true;
}

void runSetup(device::CardAudio& card, Clock& clock, RoutingCapture& capture, device::InputSource source,
              int monitorPair, bool oemTasks)
{
    std::string error;
    if (!startSetup(card, clock, source, monitorPair, oemTasks, error))
        capture.error = error;
    else
    {
        unsigned value = 0;
        if (card.readXpandSelect(value))
            capture.selectValue = value;
        measureInputs(card, capture);
    }
    stopBoth(card);
}

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

/// The analog output's left and right, as output channels, in `capture`:
/// AES out 1-2, or the monitor's pair, or both.
std::vector<unsigned> analogSources(AnalogOutSource source, unsigned monitorPair, unsigned side)
{
    std::vector<unsigned> outputs;
    if (source == AnalogOutSource::Aes12 || source == AnalogOutSource::Both)
        outputs.push_back(side);
    if (source == AnalogOutSource::Monitor || source == AnalogOutSource::Both)
        outputs.push_back(2 * monitorPair + side);
    return outputs;
}

unsigned monitorPairOf(const RoutingRun& run, const RoutingCapture& capture)
{
    return &capture == &run.analogMonitor12 ? kMonitorOn12 : kMonitorOn1516;
}

std::string describeInput(const RoutingRun& run, const RoutingCapture& capture, unsigned input)
{
    const ToneLevels& levels = capture.inputs[input];
    const std::vector<unsigned> heard = levels.heard();
    std::string text = inputLabel(run, capture, input);
    if (heard.empty())
        return text + "  (nothing above " + fixed(ToneLevels::kPresentDbfs, 0) + " dBFS)";
    std::string tones;
    for (const unsigned h : heard)
        tones += (tones.empty() ? "" : ", ") + std::string("out ") + std::to_string(h + 1) + " at " +
                 fixed(levels.dbfs[h], 1) + " dBFS";
    text += "  (" + tones + "; " +
            (exact(levels) ? std::string("bit-exact: digital")
                           : "through converters or a resampler, noise " + fixed(levels.residualDbfs, 1) + " dBFS") +
            ")";
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
// Signal and measurement
// ---------------------------------------------------------------------------

unsigned RoutingSignal::toneHz(unsigned channel)
{
    // 25 Hz times a prime: 34 x prime periods in the 65280-frame ring, prime
    // periods in 1920 frames, and no tone a harmonic, sum or difference of
    // two others.
    static const unsigned primes[kChannels] = {17, 23, 29, 37, 43, 53, 59, 67, 73, 83, 89, 97, 103, 109, 113, 127};
    return 25u * primes[channel % kChannels];
}

std::int32_t RoutingSignal::sample(std::int64_t frame, unsigned channel)
{
    const double amplitude = std::pow(10.0, kToneDbfs / 20.0);
    const double x = amplitude * sineTable()[phaseIndex(toneHz(channel), frame)];
    return static_cast<std::int32_t>(std::lround(x * 8388608.0)) * 256;
}

std::vector<unsigned> ToneLevels::heard() const
{
    std::vector<unsigned> out;
    for (unsigned k = 0; k < RoutingSignal::kChannels; ++k)
    {
        if (dbfs[k] >= kPresentDbfs)
            out.push_back(k);
    }
    std::sort(out.begin(), out.end(), [this](unsigned a, unsigned b) { return dbfs[a] > dbfs[b]; });
    return out;
}

ToneLevels measureTones(const std::int32_t* interleaved, unsigned channels, unsigned channel, std::size_t frames)
{
    const std::vector<double>& table = sineTable();
    double sines[RoutingSignal::kChannels] = {};
    double cosines[RoutingSignal::kChannels] = {};
    unsigned hz[RoutingSignal::kChannels];
    for (unsigned k = 0; k < RoutingSignal::kChannels; ++k)
        hz[k] = RoutingSignal::toneHz(k);
    double sum = 0.0, squares = 0.0;
    for (std::size_t n = 0; n < frames; ++n)
    {
        const double x = static_cast<double>(interleaved[n * channels + channel]) / kFullScale;
        sum += x;
        squares += x * x;
        for (unsigned k = 0; k < RoutingSignal::kChannels; ++k)
        {
            const std::size_t m = phaseIndex(hz[k], static_cast<std::int64_t>(n));
            sines[k] += x * table[m];
            cosines[k] += x * table[(m + static_cast<std::size_t>(kRate / 4)) % static_cast<std::size_t>(kRate)];
        }
    }

    // Whole periods of every tone: the sums are the exact least-squares fit.
    ToneLevels levels;
    const double n = static_cast<double>(frames);
    double toneEnergy = 0.0;
    for (unsigned k = 0; k < RoutingSignal::kChannels; ++k)
    {
        const double a = 2.0 * sines[k] / n;
        const double b = 2.0 * cosines[k] / n;
        levels.dbfs[k] = decibels(std::hypot(a, b));
        toneEnergy += 0.5 * n * (a * a + b * b);
    }
    const double dc = sum / n;
    const double residual = std::max(0.0, squares - n * dc * dc - toneEnergy);
    levels.residualDbfs = decibels(std::sqrt(residual / n) * std::sqrt(2.0));
    return levels;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

RoutingRun runRouting(device::CardAudio& card, Clock& clock, bool scanSelect,
                      const std::function<bool()>& stopRequested, bool oemTasks)
{
    RoutingRun run;
    run.oemTasks = oemTasks;
    run.description = card.description();
    run.haveAnalogLevel = card.readAnalogLevel(run.analogLevelDbu);
    run.haveSelect = card.readXpandSelect(run.selectBefore);
    auto stop = [&] { return stopRequested && stopRequested(); };

    run.aes.name = "input AES";
    run.analogMonitor16.name = "input analog, the monitor on 15-16";
    run.analogMonitor12.name = "input analog, the monitor on 1-2";
    runSetup(card, clock, run.aes, device::InputSource::Aes, -1, oemTasks);
    if (!stop())
        runSetup(card, clock, run.analogMonitor16, device::InputSource::Analog, int(kMonitorOn1516), oemTasks);
    if (!stop())
        runSetup(card, clock, run.analogMonitor12, device::InputSource::Analog, int(kMonitorOn12), oemTasks);

    if (scanSelect && run.haveSelect && !stop())
    {
        run.scanned = true;
        std::string error;
        if (startSetup(card, clock, device::InputSource::Analog, int(kMonitorOn1516), oemTasks, error))
        {
            for (unsigned value = 0; value < 16 && !stop(); ++value)
            {
                RoutingCapture capture;
                capture.name = "select " + std::to_string(value);
                capture.selectValue = value;
                if (!card.writeXpandSelect(value))
                    capture.error = "writing the select failed";
                else
                {
                    clock.sleepForNs(kSelectSettleNs);
                    measureInputs(card, capture);
                }
                run.scan.push_back(capture);
            }
        }
        else
        {
            RoutingCapture failed;
            failed.name = "select scan";
            failed.error = error;
            run.scan.push_back(failed);
        }
        stopBoth(card);
    }
    run.interrupted = stop();
    return run;
}

AnalogOutSource analogOutSource(const RoutingRun& run)
{
    const RoutingCapture& c = run.analogMonitor16;
    if (!c.done || c.inputs.size() < 2)
        return AnalogOutSource::Unknown;
    const ToneLevels& left = c.inputs[0];
    const ToneLevels& right = c.inputs[1];
    // Inputs 1-2 should be the analog input now, not AES.
    if (exact(left) && exact(right) && (hears(left, 0) || hears(right, 1)))
        return AnalogOutSource::Unknown;
    const bool aes = hears(left, 0) || hears(right, 1);
    const bool monitor = hears(left, 2 * kMonitorOn1516) || hears(right, 2 * kMonitorOn1516 + 1);
    if (aes && monitor)
        return AnalogOutSource::Both;
    if (aes)
        return AnalogOutSource::Aes12;
    if (monitor)
        return AnalogOutSource::Monitor;
    return AnalogOutSource::Nothing;
}

AnalogInReplaces analogInReplaces(const RoutingRun& run)
{
    if (!run.aes.done || !run.analogMonitor16.done)
        return AnalogInReplaces::Unknown;
    unsigned cabled = 0, kept = 0;
    for (unsigned input = 2; input < kInputs && input < run.aes.inputs.size(); ++input)
    {
        if (!ownOutput(run.aes.inputs[input], input))
            continue;   // not cabled
        ++cabled;
        if (ownOutput(run.analogMonitor16.inputs[input], input))
            ++kept;
    }
    if (cabled == 0)
        return AnalogInReplaces::Unknown;
    if (kept == cabled)
        return AnalogInReplaces::Aes12;
    if (kept == 0)
        return AnalogInReplaces::AllAes;
    return AnalogInReplaces::Some;
}

std::string inputLabel(const RoutingRun& run, const RoutingCapture& capture, unsigned input)
{
    if (input >= capture.inputs.size())
        return "?";
    const ToneLevels& levels = capture.inputs[input];
    const std::vector<unsigned> heard = levels.heard();
    if (heard.empty())
        return "-";
    const AnalogOutSource source = analogOutSource(run);
    const unsigned pair = monitorPairOf(run, capture);
    const std::vector<unsigned> left = analogSources(source, pair, 0);
    const std::vector<unsigned> right = analogSources(source, pair, 1);
    auto in = [](const std::vector<unsigned>& v, unsigned x) { return std::find(v.begin(), v.end(), x) != v.end(); };

    // Inputs 1-2 are the analog input in the analog set-ups: there their
    // own outputs' tones come through the analog output and the XLRs.
    const bool aesInput = &capture == &run.aes || input >= 2;
    std::string label;
    for (const unsigned h : heard)
    {
        std::string part;
        if (exact(levels))
            part = h == input ? "A" : "o" + std::to_string(h + 1);
        else if (aesInput && h == input && h == heard.front())
            part = "A~";   // its own AES output, resampled on the way
        else if (in(left, h))
            part = "L";
        else if (in(right, h))
            part = "R";
        else
            part = "o" + std::to_string(h + 1);
        if (label.find(part) == std::string::npos)
            label += (label.empty() ? "" : "+") + part;
    }
    return label;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

namespace
{

void addCapture(const RoutingRun& run, const RoutingCapture& capture, Report& report)
{
    Report::Section& section = report.addSection(capture.name);
    if (!capture.done)
    {
        section.add("result", capture.error.empty() ? std::string("not run") : "failed: " + capture.error);
        return;
    }
    for (unsigned input = 0; input < capture.inputs.size(); ++input)
        section.add("in " + std::to_string(input + 1), describeInput(run, capture, input));
}

std::string q4Text(const RoutingRun& run)
{
    switch (analogOutSource(run))
    {
        case AnalogOutSource::Aes12:
            return "it follows AES out 1-2 (with the monitor on 15-16, the analog input heard outputs 1-2)";
        case AnalogOutSource::Monitor:
            return "it follows the monitor (with the monitor on 15-16, the analog input heard outputs 15-16)";
        case AnalogOutSource::Both:
            return "it carried AES out 1-2 and the monitor at once: see the set-ups below";
        case AnalogOutSource::Nothing:
            return "the analog input heard no output: XLR out L/R not looped to XLR in L/R, or the analog "
                   "output follows something this does not route";
        case AnalogOutSource::Unknown:
            break;
    }
    return "unknown: the analog set-up did not run, or the input stayed on AES";
}

std::string q5Text(const RoutingRun& run)
{
    switch (analogInReplaces(run))
    {
        case AnalogInReplaces::Aes12:
            return "only AES in 1-2: with the analog input on, the cabled AES inputs 3-8 still heard AES";
        case AnalogInReplaces::AllAes:
            return "the whole AES input: with the analog input on, AES in 3-8 heard nothing of AES";
        case AnalogInReplaces::Some:
            return "some of AES in 3-8 kept AES and some did not: see the set-ups below";
        case AnalogInReplaces::Unknown:
            break;
    }
    return "unknown: loop AES out 3-8 to AES in 3-8 to tell";
}

std::string scanRow(const RoutingRun& run, const RoutingCapture& capture)
{
    if (!capture.done)
        return capture.error.empty() ? std::string("not run") : "failed: " + capture.error;
    std::string row;
    for (unsigned input = 0; input < capture.inputs.size(); ++input)
    {
        std::string label = inputLabel(run, capture, input);
        while (label.size() < 4)
            label += ' ';
        row += label;
    }
    while (!row.empty() && row.back() == ' ')
        row.pop_back();
    return row;
}

std::string q14Text(const RoutingRun& run)
{
    if (!run.scanned)
        return run.haveSelect ? "not scanned (run routing --scan-select)" : "no breakout board: no select";
    if (run.scan.size() < 16)
        return "the scan did not finish";
    std::vector<std::string> rows;
    for (const RoutingCapture& c : run.scan)
        rows.push_back(scanRow(run, c));

    bool bit0 = true, nonZero = true;
    for (unsigned v = 0; v < 16; ++v)
    {
        bit0 = bit0 && rows[v] == rows[v & 1u];
        nonZero = nonZero && rows[v] == rows[v == 0 ? 0 : 1];
    }
    if (rows[0] == rows[1])
    {
        // The same tones, but through the KONA X's resampling AES inputs AES
        // in 1-2 hear outputs 1-2 just as the analog input does (the analog
        // output plays AES out 1-2): the analog path's gain still tells
        // them apart (2026-11-10: -20.4 dBFS on odd values, -20.0 on even).
        auto level = [&](unsigned v) {
            const RoutingCapture& c = run.scan[v];
            return c.done && !c.inputs.empty() ? c.inputs[0].dbfs[0] : -200.0;
        };
        constexpr double kGainStep = 0.05;   // AES is exact; an analog path is rarely within this
        if (std::fabs(level(1) - level(0)) < kGainStep)
            return "values 0 and 1 heard the same, at the same level: the select did not switch anything (or the "
                   "analog path has AES's gain to 0.05 dB)";
        bool byBit0 = true, byNonZero = true;
        for (unsigned v = 0; v < 16; ++v)
        {
            const bool analog = std::fabs(level(v) - level(1)) < std::fabs(level(v) - level(0));
            byBit0 = byBit0 && analog == ((v & 1u) != 0);
            byNonZero = byNonZero && analog == (v != 0);
        }
        const std::string by = " (told by the level in 1 hears out 1 at: " + fixed(level(0), 1) + " dBFS with 0, " +
                               fixed(level(1), 1) +
                               " with 1; the tones alone cannot tell, as the AES inputs resample)";
        if (byBit0)
            return "only bit 0 counts: every even value is AES (0), every odd one analog (1)" + by;
        if (byNonZero)
            return "any value but 0 selects analog, as 1 does" + by;
        return "other values do something of their own: see the levels in the .csv" + by;
    }
    if (bit0)
        return "only bit 0 counts: every even value is AES (0), every odd one analog (1)";
    if (nonZero)
        return "any value but 0 selects analog, as 1 does";

    // Analog on other inputs, or AES 1-2 kept with analog elsewhere.
    std::string together;
    for (unsigned v = 0; v < 16; ++v)
    {
        const RoutingCapture& c = run.scan[v];
        bool aes12 = c.done && c.inputs.size() >= 2 && inputLabel(run, c, 0) == "A" && inputLabel(run, c, 1) == "A";
        bool analogElsewhere = false;
        for (unsigned input = 2; c.done && input < c.inputs.size(); ++input)
        {
            const std::string label = inputLabel(run, c, input);
            analogElsewhere = analogElsewhere || label.find('L') != std::string::npos ||
                              label.find('R') != std::string::npos;
        }
        if (aes12 && analogElsewhere)
            together += (together.empty() ? "" : ", ") + std::to_string(v);
    }
    std::string text = "other values do something of their own: see [select scan]";
    if (!together.empty())
        text += "; AES in 1-2 and the analog input at once with value(s) " + together;
    return text;
}

std::string driverText(const RoutingRun& run)
{
    std::string text;
    switch (analogOutSource(run))
    {
        case AnalogOutSource::Aes12:
            text = "outputs 1-2 feed AES 1-2 and the analog output together (\"AES 1 + Analog L\", \"AES 2 + "
                   "Analog R\", as the design names them)";
            break;
        case AnalogOutSource::Monitor:
            text = "the analog output can get its own pair through the monitor (Out 9-10 \"Analog L/R\" in the "
                   "design), apart from AES 1-2";
            break;
        default:
            break;
    }
    switch (analogInReplaces(run))
    {
        case AnalogInReplaces::Aes12:
            text += std::string(text.empty() ? "" : "; ") + "with the analog input, AES in 3-8 stay: Analog L/R "
                    "plus AES 3-8 as inputs";
            break;
        case AnalogInReplaces::AllAes:
            text += std::string(text.empty() ? "" : "; ") + "with the analog input, no AES input: 2 inputs";
            break;
        default:
            break;
    }
    return text.empty() ? std::string("nothing settled yet") : text;
}

} // namespace

void addRoutingReport(const RoutingRun& run, Report& report)
{
    Report::Section& overview = report.addSection("routing");
    overview.add("device", run.description);
    std::string level = fixed(RoutingSignal::kToneDbfs, 0) + " dBFS";
    if (run.haveAnalogLevel)
        level += " (" + signedFixed(run.analogLevelDbu + RoutingSignal::kToneDbfs, 0) + " dBu with the Xpand at " +
                 signedFixed(run.analogLevelDbu, 0) + " dBu)";
    overview.add("signal", "a tone on each of the 16 outputs, " + std::to_string(RoutingSignal::toneHz(0)) +
                               " Hz on 1 up to " + std::to_string(RoutingSignal::toneHz(15)) + " Hz on 16, at " +
                               level + "; the playback ring loops by itself (erase mode off)");
    overview.add("cabling", "AES out 1-8 to AES in 1-8, XLR out L/R to XLR in L/R; nothing else on the XLR outputs");
    overview.add("routing", "AES out 1-8 and the monitor from this audio system, the input AES then analog; put "
                            "back afterwards");
    overview.add("task mode", run.oemTasks ? "OEM while the set-ups last, so AJA's services leave the routing "
                                             "alone; put back afterwards"
                                           : "as the card had it (--tasks keep)");
    if (run.haveSelect)
        overview.add("select before", std::to_string(run.selectBefore) +
                                          (run.selectBefore == 0   ? " (AES)"
                                           : run.selectBefore == 1 ? " (analog)"
                                                                   : ""));
    overview.add("labels", "A its own AES output (A~ resampled on the way), L and R the analog output's, oN "
                           "output N's tone, - nothing");
    if (run.interrupted)
        overview.add("interrupted", "yes (Ctrl+C): the results are partial");

    Report::Section& answers = report.addSection("answers");
    answers.add("Q4, analog output", q4Text(run));
    answers.add("Q5, analog input", q5Text(run));
    answers.add("Q14, select values", q14Text(run));
    answers.add("for the driver", driverText(run));

    addCapture(run, run.aes, report);
    addCapture(run, run.analogMonitor16, report);
    addCapture(run, run.analogMonitor12, report);

    if (run.scanned)
    {
        Report::Section& scan = report.addSection("select scan");
        scan.add("value", "in 1 to in 8");
        for (const RoutingCapture& c : run.scan)
            scan.add(std::to_string(c.selectValue), scanRow(run, c));
    }
}

std::string routingCsv(const RoutingRun& run)
{
    std::string csv = "capture,select,input,label,heard,residual_dbfs,digital\n";
    auto rows = [&](const RoutingCapture& c) {
        if (!c.done)
            return;
        for (unsigned input = 0; input < c.inputs.size(); ++input)
        {
            const ToneLevels& levels = c.inputs[input];
            std::string heard;
            for (const unsigned h : levels.heard())
                heard += (heard.empty() ? "" : ";") + std::to_string(h + 1) + ":" + fixed(levels.dbfs[h], 1);
            csv += "\"" + c.name + "\"," + std::to_string(c.selectValue) + "," + std::to_string(input + 1) + "," +
                   inputLabel(run, c, input) + "," + heard + "," + fixed(levels.residualDbfs, 1) + "," +
                   (exact(levels) ? "1" : "0") + "\n";
        }
    };
    rows(run.aes);
    rows(run.analogMonitor16);
    rows(run.analogMonitor12);
    for (const RoutingCapture& c : run.scan)
        rows(c);
    return csv;
}

} // namespace ajaduplex::probe
