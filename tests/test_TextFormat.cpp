// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "common/TextFormat.h"

using namespace ajaduplex;

namespace
{

constexpr std::uint32_t fourccOf(char a, char b, char c, char d)
{
    return (std::uint32_t(std::uint8_t(a)) << 24) | (std::uint32_t(std::uint8_t(b)) << 16) |
           (std::uint32_t(std::uint8_t(c)) << 8) | std::uint32_t(std::uint8_t(d));
}

std::tm sampleTime()
{
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;   // November
    t.tm_mday = 8;
    t.tm_hour = 8;
    t.tm_min = 15;
    t.tm_sec = 30;
    return t;
}

} // namespace

TEST_CASE(TextFormat, Hex32AlwaysHasEightDigits)
{
    EXPECT_EQ(text::hex32(0), std::string("0x00000000"));
    EXPECT_EQ(text::hex32(0xABCDu), std::string("0x0000ABCD"));
    EXPECT_EQ(text::hex32(0xFFFFFFFFu), std::string("0xFFFFFFFF"));
}

TEST_CASE(TextFormat, FourccReadsMostSignificantByteFirst)
{
    EXPECT_EQ(text::fourcc(fourccOf('A', 'J', 'I', 'N')), std::string("AJIN"));
    EXPECT_EQ(text::fourcc(0), std::string("none"));
    EXPECT_EQ(text::fourcc(0x01020304u), std::string("0x01020304"));
}

TEST_CASE(TextFormat, MegabytesWholeAndFractional)
{
    EXPECT_EQ(text::megabytes(0), std::string("0 MB"));
    EXPECT_EQ(text::megabytes(8u * 1024u * 1024u), std::string("8 MB"));
    EXPECT_EQ(text::megabytes(0x78000000u), std::string("1920 MB"));
    EXPECT_EQ(text::megabytes(4177920u), std::string("3.98 MB"));   // 4 MB - 16 KB
}

TEST_CASE(TextFormat, ChannelSetAsRanges)
{
    EXPECT_EQ(text::channelSet(0, 16), std::string("none"));
    EXPECT_EQ(text::channelSet(0xFFFFu, 16), std::string("all"));
    EXPECT_EQ(text::channelSet(0xBu, 16), std::string("1-2, 4"));
    EXPECT_EQ(text::channelSet(0x8001u, 16), std::string("1, 16"));
    EXPECT_EQ(text::channelSet(0x00F0u, 16), std::string("5-8"));
    EXPECT_EQ(text::channelSet(0x10000u, 16), std::string("none"));   // beyond the count
    EXPECT_EQ(text::channelSet(0x3u, 2), std::string("all"));
}

TEST_CASE(TextFormat, BlockRegionDecode)
{
    const text::BlockRegion region = text::decodeBlockRegion((236u << 16) | 4u);
    EXPECT_EQ(region.firstBlock, 236u);
    EXPECT_EQ(region.blockCount, 4u);
}

TEST_CASE(TextFormat, BlockRegionDescription)
{
    EXPECT_EQ(text::describeBlockRegion((236u << 16) | 4u),
              std::string("blocks 236-239 (0x76000000-0x77FFFFFF, 32 MB)"));
    EXPECT_EQ(text::describeBlockRegion((239u << 16) | 1u),
              std::string("block 239 (0x77800000-0x77FFFFFF, 8 MB)"));
    EXPECT_EQ(text::describeBlockRegion(5u << 16), std::string("empty region (block 5)"));
    // Past 4 GB the addresses no longer fit in 32 bits.
    EXPECT_EQ(text::describeBlockRegion((600u << 16) | 1u),
              std::string("block 600 (0x12C000000-0x12C7FFFFF, 8 MB)"));
}

TEST_CASE(TextFormat, SanitizeLabelKeepsSafeCharacters)
{
    EXPECT_EQ(text::sanitizeLabel("idle"), std::string("idle"));
    EXPECT_EQ(text::sanitizeLabel("aja_drivers-2"), std::string("aja_drivers-2"));
    EXPECT_EQ(text::sanitizeLabel("AJA ASIO + WDM"), std::string("AJA-ASIO-WDM"));
    EXPECT_EQ(text::sanitizeLabel("plugin/2x1080p"), std::string("plugin-2x1080p"));
}

TEST_CASE(TextFormat, SanitizeLabelTrimsAndLimits)
{
    EXPECT_EQ(text::sanitizeLabel(""), std::string());
    EXPECT_EQ(text::sanitizeLabel("   "), std::string());
    EXPECT_EQ(text::sanitizeLabel("--x--"), std::string("x"));
    EXPECT_EQ(text::sanitizeLabel("\xC3\xB1" "and\xC3\xBA"), std::string("and"));   // UTF-8 letters dropped
    EXPECT_EQ(text::sanitizeLabel(std::string(50, 'a')), std::string(40, 'a'));
    EXPECT_EQ(text::sanitizeLabel(std::string(39, 'a') + " b"), std::string(39, 'a'));
}

TEST_CASE(TextFormat, Timestamps)
{
    const std::tm t = sampleTime();
    EXPECT_EQ(text::timestampForFileName(t), std::string("20261108-081530"));
    EXPECT_EQ(text::timestampForDisplay(t), std::string("2026-11-08 08:15:30"));
}

TEST_CASE(TextFormat, LocalTimeRoundTripsThroughMktime)
{
    std::tm t = sampleTime();
    t.tm_isdst = -1;
    const std::time_t when = std::mktime(&t);
    const std::tm back = text::localTime(when);
    EXPECT_EQ(back.tm_year, t.tm_year);
    EXPECT_EQ(back.tm_mon, t.tm_mon);
    EXPECT_EQ(back.tm_mday, t.tm_mday);
    EXPECT_EQ(back.tm_hour, t.tm_hour);
    EXPECT_EQ(back.tm_min, t.tm_min);
}
