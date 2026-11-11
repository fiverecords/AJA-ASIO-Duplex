// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CardOpeners.h
 * @brief   The two cards the driver can run on: the KONA X, or the simulated one.
 */

#pragma once

#include "asio/DriverCore.h"
#include "common/Clock.h"

namespace ajaduplex::asio
{

/// The real card, through libajantv2: the device index and audio system
/// the settings name. Refuses an audio system some other program runs.
class Ntv2Opener final : public CardOpener
{
public:
    std::unique_ptr<device::CardAudio> open(const DriverSettings& settings, std::string& error) override;
};

/// The simulated card on the PC's clock, in real time, with AES out 1-8
/// looped to AES in 1-8 and XLR out L/R to XLR in L/R: whatever a host
/// plays on outputs 1-2 comes back on its inputs, after the simulated
/// card's and converters' delays, which the driver then reports exactly.
class SimulatedOpener final : public CardOpener
{
public:
    explicit SimulatedOpener(Clock& clock) : clock_(clock) {}

    std::unique_ptr<device::CardAudio> open(const DriverSettings& settings, std::string& error) override;
    std::string clockSourceName() override { return "Simulated card"; }
    bool hardwareLatencies(const DriverSettings& settings, std::uint32_t& inputFrames,
                           std::uint32_t& outputFrames) override;

private:
    Clock& clock_;
};

} // namespace ajaduplex::asio
