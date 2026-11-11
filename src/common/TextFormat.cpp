// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "common/TextFormat.h"

#include <cstdio>

namespace ajaduplex::text
{

namespace
{

constexpr std::uint64_t kBlockBytes = 8ull * 1024ull * 1024ull;   // SDRAMAuditor's unit

std::string hexAddress(std::uint64_t value)
{
    char buf[24];
    if (value <= 0xFFFFFFFFull)
        std::snprintf(buf, sizeof(buf), "0x%08llX", static_cast<unsigned long long>(value));
    else
        std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return buf;
}

bool isLabelChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

std::string formatTime(const std::tm& when, const char* format)
{
    char buf[64];
    const std::size_t n = std::strftime(buf, sizeof(buf), format, &when);
    return std::string(buf, n);
}

} // namespace

std::string hex32(std::uint32_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(value));
    return buf;
}

std::string fourcc(std::uint32_t code)
{
    if (code == 0)
        return "none";
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const char c = static_cast<char>((code >> shift) & 0xFFu);
        if (c < 0x20 || c > 0x7E)
            return hex32(code);
        out.push_back(c);
    }
    return out;
}

std::string megabytes(std::uint64_t bytes)
{
    constexpr std::uint64_t kMB = 1024ull * 1024ull;
    char buf[32];
    if (bytes % kMB == 0)
        std::snprintf(buf, sizeof(buf), "%llu MB", static_cast<unsigned long long>(bytes / kMB));
    else
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / static_cast<double>(kMB));
    return buf;
}

const char* yesNo(bool value)
{
    return value ? "yes" : "no";
}

std::string channelSet(std::uint32_t mask, unsigned channelCount)
{
    if (channelCount > 32)
        channelCount = 32;
    const std::uint32_t all = channelCount == 32 ? 0xFFFFFFFFu : ((1u << channelCount) - 1u);
    mask &= all;
    if (mask == 0)
        return "none";
    if (mask == all)
        return "all";

    std::string out;
    unsigned ch = 0;
    while (ch < channelCount)
    {
        if ((mask & (1u << ch)) == 0)
        {
            ++ch;
            continue;
        }
        const unsigned first = ch;
        while (ch + 1 < channelCount && (mask & (1u << (ch + 1))) != 0)
            ++ch;
        if (!out.empty())
            out += ", ";
        out += std::to_string(first + 1);
        if (ch > first)
            out += "-" + std::to_string(ch + 1);
        ++ch;
    }
    return out;
}

BlockRegion decodeBlockRegion(std::uint32_t encoded)
{
    BlockRegion region;
    region.firstBlock = encoded >> 16;
    region.blockCount = encoded & 0xFFFFu;
    return region;
}

std::string describeBlockRegion(std::uint32_t encoded)
{
    const BlockRegion region = decodeBlockRegion(encoded);
    if (region.blockCount == 0)
        return "empty region (block " + std::to_string(region.firstBlock) + ")";

    const std::uint64_t first = region.firstBlock;
    const std::uint64_t last  = first + region.blockCount - 1;
    const std::uint64_t start = first * kBlockBytes;
    const std::uint64_t end   = (last + 1) * kBlockBytes - 1;

    std::string out = region.blockCount == 1
        ? "block " + std::to_string(first)
        : "blocks " + std::to_string(first) + "-" + std::to_string(last);
    out += " (" + hexAddress(start) + "-" + hexAddress(end) + ", " +
           megabytes(region.blockCount * kBlockBytes) + ")";
    return out;
}

std::string sanitizeLabel(std::string_view label)
{
    std::string out;
    out.reserve(label.size());
    for (const char c : label)
    {
        if (isLabelChar(c))
            out.push_back(c);
        else if (out.empty() || out.back() != '-')
            out.push_back('-');
    }

    constexpr std::size_t kMaxLength = 40;
    if (out.size() > kMaxLength)
        out.resize(kMaxLength);

    const std::size_t first = out.find_first_not_of('-');
    if (first == std::string::npos)
        return {};
    const std::size_t last = out.find_last_not_of('-');
    return out.substr(first, last - first + 1);
}

std::tm localTime(std::time_t when)
{
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &when);
#else
    localtime_r(&when, &out);
#endif
    return out;
}

std::string timestampForFileName(const std::tm& when)
{
    return formatTime(when, "%Y%m%d-%H%M%S");
}

std::string timestampForDisplay(const std::tm& when)
{
    return formatTime(when, "%Y-%m-%d %H:%M:%S");
}

} // namespace ajaduplex::text
