// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AsioSdk.h
 * @brief   The ASIO SDK's types, included the same way everywhere.
 *
 * asio.h packs its structures to 4 bytes, but only under MSVC. Hosts are
 * built with MSVC, so with any other compiler (MinGW, for the checks made
 * off Windows) the same packing is set here, and on 64-bit Windows the
 * sizes the hosts expect are checked at compile time. On other systems
 * (the unit tests on Linux) the types never cross a process boundary and
 * only have to be consistent with themselves.
 *
 * The SDK is under third_party/asiosdk, used under its GPLv3 option.
 */

#pragma once

#if !defined(_MSC_VER)
#pragma pack(push, 4)
#endif
#include "asiosys.h"
#include "asio.h"
#if !defined(_MSC_VER)
#pragma pack(pop)
#endif

#include <cstdint>

#if defined(_WIN64)
static_assert(sizeof(long) == 4, "ASIO's long is 32-bit on Windows");
static_assert(sizeof(ASIOBufferInfo) == 24, "ASIOBufferInfo as MSVC lays it out");
static_assert(sizeof(ASIOChannelInfo) == 52, "ASIOChannelInfo as MSVC lays it out");
static_assert(sizeof(ASIOClockSource) == 48, "ASIOClockSource as MSVC lays it out");
static_assert(sizeof(ASIOCallbacks) == 32, "ASIOCallbacks as MSVC lays it out");
static_assert(sizeof(ASIOTime) == 148, "ASIOTime as MSVC lays it out (4-byte packing)");
#endif

namespace ajaduplex::asio
{

/// ASIOSamples and ASIOTimeStamp hold a 64-bit value as two 32-bit halves.
template <typename T>
inline void setHiLo(T& target, std::int64_t value)
{
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    target.hi = static_cast<unsigned long>((bits >> 32) & 0xFFFFFFFFu);
    target.lo = static_cast<unsigned long>(bits & 0xFFFFFFFFu);
}

template <typename T>
inline std::int64_t getHiLo(const T& source)
{
    const std::uint64_t hi = static_cast<std::uint64_t>(source.hi) & 0xFFFFFFFFu;
    const std::uint64_t lo = static_cast<std::uint64_t>(source.lo) & 0xFFFFFFFFu;
    return static_cast<std::int64_t>((hi << 32) | lo);
}

} // namespace ajaduplex::asio
