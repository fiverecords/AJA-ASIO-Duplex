// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "engine/ClockModel.h"

#include <cmath>

namespace ajaduplex::engine
{

namespace
{

constexpr std::size_t kMinObservations = 8;
constexpr std::int64_t kMinSpanNs = 20000000;   // 20 ms of observations before trusting a fit

} // namespace

ClockModel::ClockModel(double nominalRate, std::size_t window)
    : nominalRate_(nominalRate), window_(window < 2 ? 2 : window), hostNs_(window_), frames_(window_)
{
}

void ClockModel::reset()
{
    next_ = 0;
    count_ = 0;
    fitValid_ = false;
}

void ClockModel::observe(std::int64_t hostNs, std::int64_t frames)
{
    hostNs_[next_] = hostNs;
    frames_[next_] = frames;
    next_ = (next_ + 1) % window_;
    if (count_ < window_)
        ++count_;
    fitValid_ = false;
}

bool ClockModel::ready() const
{
    if (count_ < kMinObservations)
        return false;
    const std::size_t newest = (next_ + window_ - 1) % window_;
    const std::size_t oldest = count_ < window_ ? 0 : next_;
    return hostNs_[newest] - hostNs_[oldest] >= kMinSpanNs;
}

void ClockModel::refit() const
{
    if (fitValid_)
        return;
    fitValid_ = true;
    rate_ = nominalRate_ / 1e9;
    if (count_ == 0)
        return;

    const std::size_t oldest = count_ < window_ ? 0 : next_;
    baseHost_ = hostNs_[oldest];
    baseFrames_ = frames_[oldest];

    // Means and the least-squares slope, on values relative to the oldest
    // observation so doubles keep their precision over hours.
    double sumH = 0.0, sumF = 0.0;
    for (std::size_t i = 0; i < count_; ++i)
    {
        const std::size_t k = (oldest + i) % window_;
        sumH += static_cast<double>(hostNs_[k] - baseHost_);
        sumF += static_cast<double>(frames_[k] - baseFrames_);
    }
    meanHost_ = sumH / static_cast<double>(count_);
    meanFrames_ = sumF / static_cast<double>(count_);

    if (!ready())
        return;   // nominal rate through the mean point
    double sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < count_; ++i)
    {
        const std::size_t k = (oldest + i) % window_;
        const double dx = static_cast<double>(hostNs_[k] - baseHost_) - meanHost_;
        const double dy = static_cast<double>(frames_[k] - baseFrames_) - meanFrames_;
        sxx += dx * dx;
        sxy += dx * dy;
    }
    if (sxx > 0.0)
    {
        const double fitted = sxy / sxx;
        // A fit more than 1000 ppm off is noise, not a clock.
        if (std::fabs(fitted * 1e9 / nominalRate_ - 1.0) < 1e-3)
            rate_ = fitted;
    }
}

double ClockModel::framesPerSecond() const
{
    refit();
    return rate_ * 1e9;
}

double ClockModel::ppm() const
{
    return (framesPerSecond() / nominalRate_ - 1.0) * 1e6;
}

std::int64_t ClockModel::hostTimeOf(std::int64_t frames) const
{
    refit();
    if (count_ == 0)
        return 0;
    const double framesFromMean = static_cast<double>(frames - baseFrames_) - meanFrames_;
    return baseHost_ + static_cast<std::int64_t>(std::llround(meanHost_ + framesFromMean / rate_));
}

} // namespace ajaduplex::engine
