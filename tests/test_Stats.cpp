// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "common/Stats.h"

using ajaduplex::Distribution;
using ajaduplex::formatNs;
using ajaduplex::summarizeNs;

namespace
{

Distribution oneToHundred()
{
    Distribution d;
    for (int i = 100; i >= 1; --i)   // added out of order on purpose
        d.add(i);
    return d;
}

} // namespace

TEST_CASE(Stats, NearestRankPercentiles)
{
    const Distribution d = oneToHundred();
    EXPECT_EQ(d.count(), std::size_t(100));
    EXPECT_EQ(d.min(), std::int64_t(1));
    EXPECT_EQ(d.max(), std::int64_t(100));
    EXPECT_EQ(d.percentile(0), std::int64_t(1));
    EXPECT_EQ(d.percentile(50), std::int64_t(50));
    EXPECT_EQ(d.percentile(99), std::int64_t(99));
    EXPECT_EQ(d.percentile(99.9), std::int64_t(100));
    EXPECT_EQ(d.percentile(100), std::int64_t(100));
    EXPECT_TRUE(d.mean() > 50.49 && d.mean() < 50.51);
}

TEST_CASE(Stats, AddingAfterAPercentileResorts)
{
    Distribution d = oneToHundred();
    EXPECT_EQ(d.max(), std::int64_t(100));
    d.add(1000);
    EXPECT_EQ(d.max(), std::int64_t(1000));
    EXPECT_EQ(d.count(), std::size_t(101));
}

TEST_CASE(Stats, EmptyDistribution)
{
    const Distribution d;
    EXPECT_TRUE(d.empty());
    EXPECT_EQ(d.percentile(50), std::int64_t(0));
    EXPECT_EQ(d.mean(), 0.0);
    EXPECT_TRUE(d.logBins().empty());
    EXPECT_EQ(summarizeNs(d), std::string("no values"));
}

TEST_CASE(Stats, CountAtOrAbove)
{
    const Distribution d = oneToHundred();
    EXPECT_EQ(d.countAtOrAbove(95), std::size_t(6));
    EXPECT_EQ(d.countAtOrAbove(101), std::size_t(0));
    EXPECT_EQ(d.countAtOrAbove(-5), std::size_t(100));
}

TEST_CASE(Stats, LogBinsCoverEveryValueOnce)
{
    Distribution d;
    for (const std::int64_t v : {0, 1, 1, 2, 5, 10, 150, 99999, 100000})
        d.add(v);
    const std::vector<Distribution::Bin> bins = d.logBins(10);
    std::size_t total = 0;
    std::int64_t previousUpper = -1;
    for (const Distribution::Bin& bin : bins)
    {
        EXPECT_TRUE(bin.lower < bin.upper);
        EXPECT_TRUE(bin.lower >= previousUpper);
        EXPECT_TRUE(bin.count > 0);
        previousUpper = bin.upper;
        total += bin.count;
    }
    EXPECT_EQ(total, d.count());
    ASSERT_TRUE(!bins.empty());
    EXPECT_EQ(bins.front().lower, std::int64_t(0));
    EXPECT_EQ(bins.front().upper, std::int64_t(1));
    EXPECT_EQ(bins[1].lower, std::int64_t(1));
    EXPECT_EQ(bins[1].count, std::size_t(2));
}

TEST_CASE(Stats, FormatNs)
{
    EXPECT_EQ(formatNs(850), std::string("850 ns"));
    EXPECT_EQ(formatNs(1234), std::string("1.23 us"));
    EXPECT_EQ(formatNs(12345), std::string("12.3 us"));
    EXPECT_EQ(formatNs(1234567), std::string("1.23 ms"));
    EXPECT_EQ(formatNs(12345678), std::string("12.3 ms"));
    EXPECT_EQ(formatNs(2500000000LL), std::string("2.50 s"));
    EXPECT_EQ(formatNs(-1500), std::string("-1.50 us"));
}

TEST_CASE(Stats, Summary)
{
    const Distribution d = oneToHundred();
    EXPECT_EQ(summarizeNs(d), std::string("min 1 ns, median 50 ns, p99 99 ns, p99.9 100 ns, max 100 ns (100)"));
}
