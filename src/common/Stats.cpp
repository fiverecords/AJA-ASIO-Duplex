// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "common/Stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ajaduplex
{

void Distribution::add(std::int64_t value)
{
    values_.push_back(value);
    sortedValid_ = false;
}

const std::vector<std::int64_t>& Distribution::sorted() const
{
    if (!sortedValid_)
    {
        sorted_ = values_;
        std::sort(sorted_.begin(), sorted_.end());
        sortedValid_ = true;
    }
    return sorted_;
}

std::int64_t Distribution::min() const
{
    return values_.empty() ? 0 : sorted().front();
}

std::int64_t Distribution::max() const
{
    return values_.empty() ? 0 : sorted().back();
}

double Distribution::mean() const
{
    if (values_.empty())
        return 0.0;
    long double sum = 0.0L;
    for (const std::int64_t v : values_)
        sum += static_cast<long double>(v);
    return static_cast<double>(sum / static_cast<long double>(values_.size()));
}

std::int64_t Distribution::percentile(double p) const
{
    if (values_.empty())
        return 0;
    const std::vector<std::int64_t>& s = sorted();
    if (p <= 0.0)
        return s.front();
    if (p >= 100.0)
        return s.back();
    // Nearest rank: the smallest value with at least p% of the values at or below it.
    const double rank = std::ceil(p / 100.0 * static_cast<double>(s.size()));
    const std::size_t index = rank < 1.0 ? 0 : static_cast<std::size_t>(rank) - 1;
    return s[std::min(index, s.size() - 1)];
}

std::size_t Distribution::countAtOrAbove(std::int64_t threshold) const
{
    const std::vector<std::int64_t>& s = sorted();
    return static_cast<std::size_t>(s.end() - std::lower_bound(s.begin(), s.end(), threshold));
}

std::vector<Distribution::Bin> Distribution::logBins(int binsPerDecade) const
{
    std::vector<Bin> bins;
    if (values_.empty() || binsPerDecade < 1)
        return bins;

    const std::vector<std::int64_t>& s = sorted();
    std::size_t i = 0;

    const std::size_t belowOne = static_cast<std::size_t>(std::lower_bound(s.begin(), s.end(), 1) - s.begin());
    if (belowOne > 0)
        bins.push_back(Bin{s.front() < 0 ? s.front() : 0, 1, belowOne});
    i = belowOne;

    // Bin k covers [10^(k/n), 10^((k+1)/n)), rounded to integers; bins that
    // round to the same integer edges are merged by construction.
    int k = 0;
    std::int64_t lower = 1;
    while (i < s.size())
    {
        std::int64_t upper = lower;
        while (upper <= lower)
        {
            ++k;
            upper = static_cast<std::int64_t>(std::llround(std::pow(10.0, static_cast<double>(k) / binsPerDecade)));
        }
        const std::size_t end = static_cast<std::size_t>(std::lower_bound(s.begin() + static_cast<std::ptrdiff_t>(i), s.end(), upper) - s.begin());
        if (end > i)
            bins.push_back(Bin{lower, upper, end - i});
        i = end;
        lower = upper;
    }
    return bins;
}

std::string formatNs(std::int64_t ns)
{
    const double magnitude = std::fabs(static_cast<double>(ns));
    char buf[32];
    if (magnitude < 1000.0)
        std::snprintf(buf, sizeof(buf), "%lld ns", static_cast<long long>(ns));
    else if (magnitude < 1e6)
        std::snprintf(buf, sizeof(buf), magnitude < 1e4 ? "%.2f us" : "%.1f us", static_cast<double>(ns) / 1e3);
    else if (magnitude < 1e9)
        std::snprintf(buf, sizeof(buf), magnitude < 1e7 ? "%.2f ms" : "%.1f ms", static_cast<double>(ns) / 1e6);
    else
        std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(ns) / 1e9);
    return buf;
}

std::string summarizeNs(const Distribution& d)
{
    if (d.empty())
        return "no values";
    return "min " + formatNs(d.min()) + ", median " + formatNs(d.percentile(50)) +
           ", p99 " + formatNs(d.percentile(99)) + ", p99.9 " + formatNs(d.percentile(99.9)) +
           ", max " + formatNs(d.max()) + " (" + std::to_string(d.count()) + ")";
}

} // namespace ajaduplex
