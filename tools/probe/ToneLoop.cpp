// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "ToneLoop.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ajaduplex::probe
{

namespace
{

constexpr double kPi = 3.141592653589793;
constexpr std::int64_t kRate = 48000;
constexpr double kFullScale = 2147483648.0;
constexpr double kHeardLevel = 1e-3;          // -60 dBFS
constexpr double kLevelTolerance = 0.5;       // dB
constexpr std::int64_t kGlitchGap = 480;      // spikes closer than 10 ms are one glitch
constexpr double kStepSmoothing = 0.25;       // how fast the drift's step follows each still second

/// sin(2 pi m / 48000), m in [0, 48000): the tones' phases are exact for
/// any frame number, however long the run.
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

double sinAt(unsigned hz, std::int64_t frame)
{
    const std::int64_t m = (static_cast<std::int64_t>(hz) * frame) % kRate;
    return sineTable()[static_cast<std::size_t>(m < 0 ? m + kRate : m)];
}

double cosAt(unsigned hz, std::int64_t frame)
{
    const std::int64_t m = (static_cast<std::int64_t>(hz) * frame + kRate / 4) % kRate;
    return sineTable()[static_cast<std::size_t>(m < 0 ? m + kRate : m)];
}

double decibels(double ratio)
{
    return ratio > 0.0 ? 20.0 * std::log10(ratio) : -200.0;
}

double wrapPhase(double phase)
{
    while (phase > kPi)
        phase -= 2.0 * kPi;
    while (phase <= -kPi)
        phase += 2.0 * kPi;
    return phase;
}

double toneAmplitude()
{
    return std::pow(10.0, ToneSignal::kToneDbfs / 20.0);
}

/// x such that a x = 1 modulo m (a prime to m).
std::int64_t inverseModulo(std::int64_t a, std::int64_t m)
{
    std::int64_t r0 = m, r1 = a % m, t0 = 0, t1 = 1;
    while (r1 != 0)
    {
        const std::int64_t q = r0 / r1;
        const std::int64_t r = r0 - q * r1;
        r0 = r1;
        r1 = r;
        const std::int64_t t = t0 - q * t1;
        t0 = t1;
        t1 = t;
    }
    return ((t0 % m) + m) % m;
}

} // namespace

// ---------------------------------------------------------------------------
// The signal
// ---------------------------------------------------------------------------

unsigned ToneSignal::toneHz(unsigned channel)
{
    // Primes, so prime to 48000: no slip shorter than a second leaves a
    // tone's phase alone. L and R (and AES 1 and 2) as before 0.1.17.
    static constexpr unsigned kTones[kMaxTones] = {997, 1499, 1997, 2503, 2999, 3499, 4001, 4507};
    return kTones[channel % kMaxTones];
}

const std::vector<double>& ToneSignal::sweep()
{
    static const std::vector<double> sweep = [] {
        const double m = static_cast<double>(kSweepFrames);
        std::vector<double> s(static_cast<std::size_t>(kSweepFrames));
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const double n = static_cast<double>(i);
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * n / (m - 1.0));
            const double phase =
                2.0 * kPi * (kSweepFromHz * n + (kSweepToHz - kSweepFromHz) * n * n / (2.0 * m)) / double(kRate);
            s[i] = toneAmplitude() * window * std::sin(phase);
        }
        return s;
    }();
    return sweep;
}

double ToneSignal::value(std::int64_t frame, unsigned channel)
{
    double x = 0.0;
    if (frame >= kSweepStart && frame < kSweepStart + kSweepFrames)
        x = sweep()[static_cast<std::size_t>(frame - kSweepStart)];
    else if (frame >= kToneStart)
    {
        x = toneAmplitude() * sinAt(toneHz(channel), frame);
        const std::int64_t in = frame - kToneStart;
        if (in < kFadeFrames)
            x *= 0.5 - 0.5 * std::cos(kPi * static_cast<double>(in) / static_cast<double>(kFadeFrames));
    }
    return x;
}

std::int32_t ToneSignal::word(double value)
{
    // 24 bits in the top of the word, as the card takes them.
    return static_cast<std::int32_t>(std::lround(value * 8388608.0)) * 256;
}

double ToneSignal::scaleFor(double toneDbfs)
{
    return std::pow(10.0, (toneDbfs - kToneDbfs) / 20.0);
}

std::int32_t ToneSignal::sample(std::int64_t frame, unsigned channel, double toneDbfs)
{
    return word(value(frame, channel) * (toneDbfs == kToneDbfs ? 1.0 : scaleFor(toneDbfs)));
}

// ---------------------------------------------------------------------------
// The verifier
// ---------------------------------------------------------------------------

ToneVerifier::ToneVerifier(unsigned channel, unsigned channels, double toneDbfs)
    : toneDbfs_(toneDbfs), channel_(channel % std::max(1u, std::min(channels, ToneSignal::kMaxTones))),
      ownHz_(ToneSignal::toneHz(channel_)),
      // Its pair's tone (L and R, AES 1 and 2, ...), or its neighbour's
      // when the pair is not looped.
      otherHz_(ToneSignal::toneHz((channel_ ^ 1u) < std::max(2u, channels) ? channel_ ^ 1u : channel_ + 1u)),
      ownHzInverse_(inverseModulo(ownHz_, kRate)),
      c_(2.0 * std::cos(2.0 * kPi * double(ToneSignal::toneHz(channel_)) / double(kRate))),
      slipPhase_(2.0 * kPi * double(ToneSignal::toneHz(channel_)) * kSlipFrames / double(kRate)),
      fineSlipPhase_(2.0 * kPi * double(ToneSignal::toneHz(channel_)) * kFineSlipFrames / double(kRate)),
      noiseHistogram_(kNoiseBins, 0), start_(static_cast<std::size_t>(kFirstWindow), 0.0), histogram_(kBins, 0)
{
    // Everything the engine's thread will touch exists before it runs.
    sineTable();
    ToneSignal::sweep();
    glitchFrames.reserve(kMaxGlitchTimes);
}

void ToneVerifier::feed(std::int64_t inputFrame, std::int32_t sample)
{
    const double x = static_cast<double>(sample) / kFullScale;
    const bool consecutive = inputFrame == lastFrame_ + 1;
    const bool haveHistory = consecutive && run_ >= 2;

    if (inputFrame < kFirstWindow)
    {
        if (inputFrame >= 0)
            start_[static_cast<std::size_t>(inputFrame)] = x;
    }
    else
    {
        if (haveHistory)
        {
            // Zero for a pure tone at its frequency, whatever its level and phase.
            const double e = std::fabs(x - c_ * x1_ + x2_);
            if (threshold_ < 0.0)
            {
                const int bin = e > 0.0 ? std::clamp(static_cast<int>((std::log10(e) + 12.0) * 20.0), 0, kBins - 1) : 0;
                ++histogram_[static_cast<std::size_t>(bin)];
                // The largest few, in case the first second has glitches too.
                if (peakCount_ < kPeaks)
                    peaks_[peakCount_++] = Peak{e, inputFrame};
                else
                {
                    std::size_t smallest = 0;
                    for (std::size_t i = 1; i < kPeaks; ++i)
                        smallest = peaks_[i].value < peaks_[smallest].value ? i : smallest;
                    if (e > peaks_[smallest].value)
                        peaks_[smallest] = Peak{e, inputFrame};
                }
            }
            else if (e > threshold_)
                spike(inputFrame);
        }
        if (!consecutive)
        {
            // The input jumped (the engine counts it): start a new second here.
            windowStart_ = inputFrame;
            sumSin_ = sumCos_ = otherSin_ = otherCos_ = sum_ = sumSquares_ = 0.0;
            count_ = 0;
        }

        if (inputFrame >= windowStart_)
        {
            sumSin_ += x * sinAt(ownHz_, inputFrame);
            sumCos_ += x * cosAt(ownHz_, inputFrame);
            otherSin_ += x * sinAt(otherHz_, inputFrame);
            otherCos_ += x * cosAt(otherHz_, inputFrame);
            sum_ += x;
            sumSquares_ += x * x;
            if (++count_ == kWindowFrames)
                closeWindow();
        }
    }
    x2_ = x1_;
    x1_ = x;
    run_ = consecutive ? std::min(run_ + 1, 2) : 1;
    lastFrame_ = inputFrame;
}

void ToneVerifier::spike(std::int64_t inputFrame)
{
    windowSpiked_ = true;
    if (inputFrame - lastSpike_ >= kGlitchGap)
    {
        ++glitches;
        if (glitchFrames.size() < kMaxGlitchTimes)
            glitchFrames.push_back(inputFrame);
    }
    lastSpike_ = inputFrame;
}

void ToneVerifier::closeWindow()
{
    // A second holds a whole number of periods of both tones: the sums are
    // the exact least-squares fit of DC plus each tone.
    const double n = static_cast<double>(count_);
    const double a = 2.0 * sumSin_ / n;
    const double b = 2.0 * sumCos_ / n;
    const double level = std::hypot(a, b);
    const double phase = std::atan2(b, a);
    const double other = 2.0 * std::hypot(otherSin_, otherCos_) / n;
    const double dc = sum_ / n;
    const double residual = std::max(0.0, sumSquares_ - n * dc * dc - 0.5 * n * (a * a + b * b));
    const double noise = decibels(std::sqrt(residual / n) * std::sqrt(2.0));   // AES17: against a full-scale sine

    if (!haveReference_)
    {
        haveReference_ = true;
        checked = true;
        heard = level > kHeardLevel;
        crossed = !heard && other > kHeardLevel;
        levelDbfs = decibels(level);
        gainDb = levelDbfs - toneDbfs_;
        noiseDbfs = noise;
        firstNoiseDbfs = noise;
        worstNoiseDbfs = noise;
        referenceLevel_ = level;
        stillPhase_ = phase;
        stillSecond_ = 0;

        // The per-sample detector's threshold: far above the noise it sees
        // (the median of |e| is 0.67 sigma for Gaussian noise).
        if (heard)
        {
            std::uint64_t total = 0;
            for (const std::uint32_t c : histogram_)
                total += c;
            std::uint64_t below = 0;
            int median = 0;
            for (; median < kBins; ++median)
            {
                below += histogram_[static_cast<std::size_t>(median)];
                if (2 * below >= total)
                    break;
            }
            const double medianValue = std::pow(10.0, (static_cast<double>(median) + 0.5) / 20.0 - 12.0);
            threshold_ = std::max(12.0 * medianValue / 0.6745, 2e-6);
            std::sort(peaks_, peaks_ + peakCount_, [](const Peak& l, const Peak& r) { return l.frame < r.frame; });
            for (std::size_t i = 0; i < peakCount_; ++i)
            {
                if (peaks_[i].value > threshold_)
                    spike(peaks_[i].frame);
            }
        }
        else
            threshold_ = 1e300;   // nothing to follow
        if (heard)
        {
            ++seconds;
            addNoise(noise);
        }
        // A glitch in it: its phase stands, measured roughly, and the
        // next second's move counts from there as after any glitch.
        stillValid_ = !windowSpiked_;
        glitchSinceStill_ = windowSpiked_;
    }
    else if (heard)
    {
        ++seconds;
        ++second_;
        addNoise(noise);
        worstNoiseDbfs = std::max(worstNoiseDbfs, noise);

        const double change = decibels(level / referenceLevel_);
        if (std::fabs(change) > kLevelTolerance)
        {
            ++levelChanges;
            if (std::fabs(change) > std::fabs(worstLevelChangeDb))
                worstLevelChangeDb = change;
        }
        followPhase(phase);
    }

    windowStart_ += count_;
    sumSin_ = sumCos_ = otherSin_ = otherCos_ = sum_ = sumSquares_ = 0.0;
    count_ = 0;
    windowSpiked_ = false;
}

void ToneVerifier::followPhase(double phase)
{
    // The loop is synchronous: the phase stays put, but for drift, unless
    // frames were lost (it moves forward) or repeated (back). A second with
    // a glitch in it is not measured: a burst of something else (stale
    // data, silence) moves its phase without any frames lost.
    if (windowSpiked_)
    {
        if (!inSlip_)
            glitchSinceStill_ = true;
        return;
    }
    if (!inSlip_)
    {
        // Held against the last still second, moved on by the drift: within
        // the tolerance it is still too (its move is drift, added up);
        // further off, a slip begins, which counts once however many
        // seconds it takes to settle (the second it happens in is only
        // partly moved, when no glitch shows where).
        const double gap = static_cast<double>(second_ - stillSecond_);
        const double deviation = wrapPhase(phase - stillPhase_ - step_ * gap);
        if (std::fabs(deviation) > tolerance(glitchSinceStill_ ? fineSlipPhase_ : slipPhase_, gap))
        {
            ++slips;
            inSlip_ = true;
            phaseBeforeMove_ = stillPhase_;
            secondBeforeMove_ = stillSecond_;
            moveMeasurable_ = stillValid_;
            movedPhase_ = phase;
            movedSecond_ = second_;
            return;
        }
        // What the drift so far did not foresee: the wander (a random walk's
        // grows as the root of the time). The first move has no drift to
        // go by, so it is all wander as far as the tolerance knows, until
        // the next seconds outvote it; the wander reported leaves it out.
        const double perRootSecond = std::fabs(deviation) / std::sqrt(gap);
        if (haveStep_)
        {
            wanderSquares_ += perRootSecond * perRootSecond;
            ++wanderCount_;
        }
        recentWander_[recentNext_] = perRootSecond;
        recentNext_ = (recentNext_ + 1) % kWanderSeconds;
        recentCount_ = std::min(recentCount_ + 1, kWanderSeconds);
        const double move = wrapPhase(phase - stillPhase_);
        driftPhase_ += move;
        step_ = haveStep_ ? step_ + kStepSmoothing * (move / gap - step_) : move / gap;
        haveStep_ = true;
    }
    else
    {
        // Settled when it holds still, drift apart, from one second to the
        // next one measured.
        const double gap = static_cast<double>(second_ - movedSecond_);
        if (std::fabs(wrapPhase(phase - movedPhase_ - step_ * gap)) > tolerance(fineSlipPhase_, gap))
        {
            movedPhase_ = phase;
            movedSecond_ = second_;
            return;
        }
        noteSlip(phase, second_);
        inSlip_ = false;
    }
    stillPhase_ = phase;
    stillSecond_ = second_;
    stillValid_ = true;
    glitchSinceStill_ = false;
}

void ToneVerifier::noteSlip(double phaseAfter, std::uint64_t secondAfter)
{
    // What the delay drifted meanwhile, as the seconds before had it, is
    // drift; the rest, in 48000ths of a turn, is Hz x k modulo 48000, k
    // whole.
    const double drift = step_ * static_cast<double>(secondAfter - secondBeforeMove_);
    driftPhase_ += drift;
    const double turns = wrapPhase(phaseAfter - phaseBeforeMove_ - drift) / (2.0 * kPi) * double(kRate);
    const double whole = std::round(turns);
    // The size needs the move to a quarter of a 48000th of a turn: with
    // wander to go by (a glitch-free second before it) and little of it.
    const double gap = static_cast<double>(secondAfter - secondBeforeMove_);
    const double spread = 1.5 * medianWander() * std::sqrt(gap) / (2.0 * kPi) * double(kRate);
    if (!moveMeasurable_ || recentCount_ == 0 || spread > 0.08 || std::fabs(turns - whole) > 0.25)
    {
        ++slipsOfUnknownSize;   // no still second before it, or the delay wandered too much meanwhile
        return;
    }
    const std::int64_t m = ((static_cast<std::int64_t>(whole) % kRate) + kRate) % kRate;
    std::int64_t k = (m * ownHzInverse_) % kRate;
    if (k > kRate / 2)
        k -= kRate;
    if (std::llabs(static_cast<long long>(k)) > std::llabs(static_cast<long long>(worstSlipFrames)))
        worstSlipFrames = k;
}

double ToneVerifier::medianWander() const
{
    if (recentCount_ == 0)
        return 0.0;
    double recent[kWanderSeconds];
    std::copy(recentWander_, recentWander_ + recentCount_, recent);
    std::nth_element(recent, recent + recentCount_ / 2, recent + recentCount_);
    return recent[recentCount_ / 2];
}

double ToneVerifier::tolerance(double base, double gap) const
{
    // Never under eight times the median wander of the last seconds: a
    // jittery converter's seconds are not slips.
    return std::max(base, kWanderFactor * medianWander() * std::sqrt(gap));
}

void ToneVerifier::addNoise(double noise)
{
    const int bin = std::clamp(static_cast<int>(std::floor((noise + 200.0) * 10.0)), 0, kNoiseBins - 1);
    ++noiseHistogram_[static_cast<std::size_t>(bin)];
}

void ToneVerifier::finish()
{
    if (inSlip_)
    {
        noteSlip(movedPhase_, movedSecond_);   // still moving when the run ended
        inSlip_ = false;
    }

    // The median second's noise: the first one may still be settling (the
    // Xpand's input just switched, say).
    std::uint64_t total = 0;
    for (const std::uint32_t n : noiseHistogram_)
        total += n;
    if (total > 0)
    {
        std::uint64_t below = 0;
        for (int bin = 0; bin < kNoiseBins; ++bin)
        {
            below += noiseHistogram_[static_cast<std::size_t>(bin)];
            if (2 * below >= total)
            {
                noiseDbfs = static_cast<double>(bin) / 10.0 - 200.0 + 0.05;
                break;
            }
        }
    }
    // A tone that comes back later has its phase behind: delay = -phase / w.
    const double radiansPerUs = 2.0 * kPi * static_cast<double>(ownHz_) * 1e-6;
    driftUs = -driftPhase_ / radiansPerUs;
    wanderUs = wanderCount_ ? std::sqrt(wanderSquares_ / static_cast<double>(wanderCount_)) / radiansPerUs : 0.0;

    // Where the sweep came back: the lag with the strongest correlation.
    const std::vector<double>& s = ToneSignal::sweep();
    const std::int64_t length = ToneSignal::kSweepFrames;
    const std::int64_t first = ToneSignal::kSweepStart;
    const std::int64_t lags = kFirstWindow - first - length;
    auto correlation = [&](std::int64_t lag) {
        double r = 0.0;
        for (std::int64_t i = 0; i < length; ++i)
            r += start_[static_cast<std::size_t>(first + lag + i)] * s[static_cast<std::size_t>(i)];
        return r;
    };

    double best = 0.0;
    std::int64_t bestLag = -1;
    for (std::int64_t lag = 0; lag < lags; ++lag)
    {
        const double r = correlation(lag);
        if (std::fabs(r) > std::fabs(best))
        {
            best = r;
            bestLag = lag;
        }
    }
    if (bestLag < 0)
        return;

    double sweepEnergy = 0.0, inputEnergy = 0.0;
    for (std::int64_t i = 0; i < length; ++i)
    {
        sweepEnergy += s[static_cast<std::size_t>(i)] * s[static_cast<std::size_t>(i)];
        const double x = start_[static_cast<std::size_t>(first + bestLag + i)];
        inputEnergy += x * x;
    }
    sweepMatch = inputEnergy > 0.0 ? std::fabs(best) / std::sqrt(sweepEnergy * inputEnergy) : 0.0;
    roundTripFound = sweepMatch > 0.5;
    inverted = best < 0.0;

    // A parabola through the peak and its neighbours: the fraction of a frame.
    double fraction = 0.0;
    if (bestLag > 0 && bestLag + 1 < lags)
    {
        const double y0 = std::fabs(correlation(bestLag - 1));
        const double y1 = std::fabs(best);
        const double y2 = std::fabs(correlation(bestLag + 1));
        const double curve = y0 - 2.0 * y1 + y2;
        if (curve < 0.0)
            fraction = std::clamp(0.5 * (y0 - y2) / curve, -0.5, 0.5);
    }
    roundTrip = static_cast<double>(bestLag) + fraction;
}

} // namespace ajaduplex::probe
