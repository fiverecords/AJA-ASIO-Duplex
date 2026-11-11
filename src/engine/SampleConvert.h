// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SampleConvert.h
 * @brief   Card frames to per-channel buffers and back.
 *
 * The card stores each sample as a little-endian signed 32-bit integer with
 * the audio in the upper bits, which is exactly ASIOSTInt32LSB: converting
 * is only interleaving and de-interleaving.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ajaduplex::engine
{

/// `frames` interleaved frames of `channels` samples into one buffer per
/// channel. A null destination skips that channel.
void deinterleave(const std::int32_t* interleaved, unsigned channels, std::size_t frames,
                  std::int32_t* const* perChannel);

/// One buffer per channel into `frames` interleaved frames. A null source
/// writes zeros: channels the host has not activated are silent.
void interleave(const std::int32_t* const* perChannel, unsigned channels, std::size_t frames,
                std::int32_t* interleaved);

} // namespace ajaduplex::engine
