// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    TextFormat.h
 * @brief   Small text helpers shared by the probe and, later, the driver log.
 *
 * Nothing here depends on the AJA SDK or on Windows, so all of it is covered
 * by the unit tests.
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace ajaduplex::text
{

/// Eight upper-case hex digits with a 0x prefix: 0xABCD -> "0x0000ABCD".
std::string hex32(std::uint32_t value);

/// A four-character code as AJA builds them with NTV2_FOURCC (first character
/// in the most significant byte), e.g. the application code a program passes
/// to AcquireStreamForApplication: 'AJIN' -> "AJIN". Zero -> "none". A code
/// with a non-printable byte comes back as hex.
std::string fourcc(std::uint32_t code);

/// A byte count in MB (2^20 bytes): 8388608 -> "8 MB", 4177920 -> "3.98 MB".
std::string megabytes(std::uint64_t bytes);

/// Samples at 48 kHz (the only rate there is) as milliseconds, to the
/// nearest tenth, for people to read: 128 -> "2.7 ms", 256 -> "5.3 ms",
/// 720 -> "15.0 ms", 0 -> "0.0 ms".
std::string milliseconds(std::uint64_t samples);

/// "yes" or "no".
const char* yesNo(bool value);

/// The channels set in a bit mask (bit 0 = channel 1), as ranges:
/// 0b1011 -> "1-2, 4". Returns "none" or "all" when that is the case for the
/// first `channelCount` channels.
std::string channelSet(std::uint32_t mask, unsigned channelCount);

/// One entry of the region lists SDRAMAuditor hands out: the first 8 MB block
/// in the high 16 bits, the number of blocks in the low 16 bits.
struct BlockRegion
{
    std::uint32_t firstBlock = 0;
    std::uint32_t blockCount = 0;
};

BlockRegion decodeBlockRegion(std::uint32_t encoded);

/// "blocks 236-239 (0x76000000-0x77FFFFFF, 32 MB)"; a single block reads
/// "block 239 (...)".
std::string describeBlockRegion(std::uint32_t encoded);

/// Makes a user-supplied label safe for a file name: keeps ASCII letters,
/// digits, '_' and '-'; turns every other run of characters into one '-';
/// trims '-' at both ends; keeps at most 40 characters. Returns an empty
/// string when nothing usable is left.
std::string sanitizeLabel(std::string_view label);

/// Thread-safe local time (localtime_s / localtime_r).
std::tm localTime(std::time_t when);

/// "20261108-081530", for file names.
std::string timestampForFileName(const std::tm& when);

/// "2026-11-08 08:15:30", for reports.
std::string timestampForDisplay(const std::tm& when);

} // namespace ajaduplex::text
