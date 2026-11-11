// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    RingMath.h
 * @brief   Positions in the card's audio rings.
 *
 * Both rings of an audio system wrap at the same address L (a little below
 * 4 MB) and hold frames of B bytes (4 x channels). The engine's own read
 * and write positions are always whole frames; the card's heads are not
 * necessarily, so turning a head into "frames available" is done here, in
 * one place.
 */

#pragma once

#include <cstdint>

namespace ajaduplex::engine
{

/// How to read LastIn (the card's write head). Phase 0 says which is right;
/// Conservative is the design's formula and is safe either way.
enum class HeadMode
{
    Conservative,   ///< W & ~3, as ntv2llburn: one frame short when LastIn points at the last byte
    LastByte,       ///< LastIn is the last byte written: data ends at W + 1
    NextByte,       ///< LastIn is the next byte to be written: data ends at W
};

struct Segment
{
    std::uint32_t offset = 0;
    std::uint32_t bytes = 0;
};

class RingGeometry
{
public:
    RingGeometry() = default;
    RingGeometry(std::uint32_t ringBytes, std::uint32_t frameBytes) : ringBytes_(ringBytes), frameBytes_(frameBytes) {}

    std::uint32_t ringBytes() const { return ringBytes_; }
    std::uint32_t frameBytes() const { return frameBytes_; }
    std::uint32_t frames() const { return frameBytes_ ? ringBytes_ / frameBytes_ : 0; }
    bool valid() const { return frameBytes_ != 0 && ringBytes_ != 0 && ringBytes_ % frameBytes_ == 0; }

    /// Forward distance from `from` to `to`, in bytes, in [0, L).
    std::uint32_t distance(std::uint32_t from, std::uint32_t to) const;

    /// `position` moved forward by `bytes`, wrapped.
    std::uint32_t advance(std::uint32_t position, std::uint64_t bytes) const;

    /// `position` moved back by `bytes`, wrapped.
    std::uint32_t retreat(std::uint32_t position, std::uint64_t bytes) const;

    /// A byte position rounded down to a frame boundary.
    std::uint32_t frameFloor(std::uint32_t position) const { return position - position % frameBytes_; }

    /// Where the captured data ends according to a LastIn value: the byte
    /// after the last whole word the head vouches for.
    std::uint32_t dataEnd(std::uint32_t head, HeadMode mode) const;

    /// Whole frames between the engine's read position (a frame boundary)
    /// and the data end of `head`.
    std::uint32_t framesAvailable(std::uint32_t head, std::uint32_t readPosition, HeadMode mode) const;

    /// [offset, offset + bytes) as one or two pieces inside the ring.
    /// Returns the number of pieces (0 when bytes is 0).
    int split(std::uint32_t offset, std::uint32_t bytes, Segment out[2]) const;

private:
    std::uint32_t ringBytes_ = 0;
    std::uint32_t frameBytes_ = 0;
};

} // namespace ajaduplex::engine
