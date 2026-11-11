// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Ntv2CardAudio.h
 * @brief   CardAudio on a real card, through libajantv2.
 *
 * Only the factory is public, so the SDK headers stay inside the .cpp.
 */

#pragma once

#include "device/CardAudio.h"

#include <memory>
#include <string>

namespace ajaduplex::device
{

/// Opens the device named by `selector` (index, serial number or model name)
/// and binds to one audio system: `audioSystem` is 1-based, 0 means the last
/// full audio system of the card (2 on the KONA X).
///
/// Refuses an audio system whose input or output is already running: some
/// other program is using it. Saves the audio system's settings on open and
/// puts them back in restore() or on destruction, the Xpand's analog/AES
/// select included.
///
/// Returns null with `error` set on failure. `pickAnother` ends the error
/// for an audio system in use: how the caller's user picks another one.
std::unique_ptr<CardAudio> openNtv2CardAudio(const std::string& selector, unsigned audioSystem,
                                             std::string& error,
                                             const std::string& pickAnother = "with --audio-system");

} // namespace ajaduplex::device
