// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Stats.h
 * @brief   Distributions of measured durations: percentiles and log bins.
 *
 * The probe keeps every measured value (a few hundred thousand at most), so
 * percentiles are exact rather than estimated from bins. The bins are for
 * the CSV files, to plot or compare runs.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ajaduplex
{

class Distribution
{
public:
    struct Bin
    {
        std::int64_t lower = 0;   ///< inclusive
        std::int64_t upper = 0;   ///< exclusive
        std::size_t count = 0;
    };

    void reserve(std::size_t n) { values_.reserve(n); }
    void add(std::int64_t value);

    std::size_t count() const { return values_.size(); }
    bool empty() const { return values_.empty(); }
    std::int64_t min() const;
    std::int64_t max() const;
    double mean() const;

    /// Nearest-rank percentile, p in [0, 100]: 50 is the median, 100 the
    /// maximum. Zero when there are no values.
    std::int64_t percentile(double p) const;

    /// How many values are at or above `threshold`.
    std::size_t countAtOrAbove(std::int64_t threshold) const;

    /// Non-empty bins on a logarithmic scale with `binsPerDecade` bins per
    /// factor of ten, starting at 1. Values below 1 go into a [0, 1) bin.
    std::vector<Bin> logBins(int binsPerDecade = 10) const;

private:
    const std::vector<std::int64_t>& sorted() const;

    std::vector<std::int64_t> values_;
    mutable std::vector<std::int64_t> sorted_;
    mutable bool sortedValid_ = false;
};

/// A duration for people: "850 ns", "12.3 us", "1.20 ms", "2.50 s".
/// Negative values keep their sign.
std::string formatNs(std::int64_t ns);

/// "min 1.2 us, median 1.5 us, p99 3.1 us, p99.9 8.0 us, max 120 us (100000)"
std::string summarizeNs(const Distribution& d);

} // namespace ajaduplex
