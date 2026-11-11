// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "engine/RingMath.h"

namespace ajaduplex::engine
{

std::uint32_t RingGeometry::distance(std::uint32_t from, std::uint32_t to) const
{
    const std::uint64_t L = ringBytes_;
    return static_cast<std::uint32_t>((std::uint64_t(to % ringBytes_) + L - from % ringBytes_) % L);
}

std::uint32_t RingGeometry::advance(std::uint32_t position, std::uint64_t bytes) const
{
    return static_cast<std::uint32_t>((std::uint64_t(position) + bytes) % ringBytes_);
}

std::uint32_t RingGeometry::retreat(std::uint32_t position, std::uint64_t bytes) const
{
    const std::uint64_t L = ringBytes_;
    return static_cast<std::uint32_t>((std::uint64_t(position % ringBytes_) + L - bytes % L) % L);
}

std::uint32_t RingGeometry::dataEnd(std::uint32_t head, HeadMode mode) const
{
    switch (mode)
    {
        case HeadMode::LastByte:
            // The byte after the last one written, down to a whole word (a
            // head inside a word does not vouch for that word).
            return advance(head, 1) & ~3u;
        case HeadMode::NextByte:
            return (head & ~3u) % ringBytes_;
        case HeadMode::Conservative:
            break;
    }
    return (head & ~3u) % ringBytes_;
}

std::uint32_t RingGeometry::framesAvailable(std::uint32_t head, std::uint32_t readPosition, HeadMode mode) const
{
    return distance(readPosition, dataEnd(head, mode)) / frameBytes_;
}

int RingGeometry::split(std::uint32_t offset, std::uint32_t bytes, Segment out[2]) const
{
    if (bytes == 0)
        return 0;
    offset %= ringBytes_;
    const std::uint32_t toEnd = ringBytes_ - offset;
    if (bytes <= toEnd)
    {
        out[0] = Segment{offset, bytes};
        return 1;
    }
    out[0] = Segment{offset, toEnd};
    out[1] = Segment{0, bytes - toEnd};
    return 2;
}

} // namespace ajaduplex::engine
