// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    DriverSettings.h
 * @brief   What the driver can be told: card, input, outputs, buffer, guard,
 *          latencies, whether to keep its routing, the Xpand's analog level.
 *
 * Kept as numbers under one key (HKCU\Software\AJA Duplex ASIO on Windows)
 * and read through SettingsStore, so the rules for defaults and limits are
 * tested without the registry. A value that is missing or out of range
 * keeps its default: a mistyped value can never stop the driver loading.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ajaduplex
{

enum class InputChoice : std::uint32_t
{
    Analog = 0,   ///< the Xpand's XLR L/R
    Aes = 1,      ///< the Xpand's AES 1-8
    Sdi = 2,      ///< the audio embedded in an SDI input (sdiInput): 16 channels
    Hdmi = 3,     ///< the audio embedded in the HDMI input: 8 channels
};

const char* toString(InputChoice input);

/// True for the inputs on the Xpand (analog, AES); false for the embedded
/// ones, whose audio clock is the video input's.
inline bool isXpandInput(InputChoice input)
{
    return input == InputChoice::Analog || input == InputChoice::Aes;
}

struct DriverSettings
{
    static constexpr std::uint32_t kMinBufferFrames = 64;
    static constexpr std::uint32_t kMaxBufferFrames = 2048;
    static constexpr std::uint32_t kMaxGuardFrames = 1024;
    static constexpr std::uint32_t kMaxLatencyFrames = 48000;

    /// What the KONA X with the Xpand adds to the round trip between its
    /// connectors, by input, in frames: what outputLatencyFrames is when it
    /// has not been saved. Measured on the card on 2026-11-11: 208 from XLR
    /// out to XLR in (the driver's own round trip came out at 720 frames in
    /// three starts of six, and at 718 to 727 in all, against the 512 of
    /// N + S: it moves by a few frames from one start to the next, with
    /// the offset between the card's heads), and about 130 from AES out to
    /// AES in, most of it the AES inputs' sample rate converter (125 to 134
    /// over phase 0's loops). Through an SDI or HDMI input it has not been
    /// measured: 0.
    static constexpr std::uint32_t cardLatencyFrames(InputChoice input) noexcept
    {
        return input == InputChoice::Analog ? 208u : (input == InputChoice::Aes ? 130u : 0u);
    }

    std::uint32_t card = 0;                    ///< the SDK's device index
    std::uint32_t audioSystem = 0;             ///< 1-based; 0: the card's last full one (2 on the KONA X)
    InputChoice input = InputChoice::Analog;   ///< analog by default (D2, 2026-11-08)
    std::uint32_t sdiInput = 1;                ///< for the SDI input: which one, 1-based (the KONA X has 2)
    std::uint32_t bufferFrames = 128;          ///< the buffer size hosts are offered first
    /// G. 256 gives a block 8 ms from complete to written with N = 128:
    /// Resolume's callback took up to 8 ms on the simulated driver, and ran
    /// 4 h 48 min clean so set. Phase 0's loop finds what the card itself
    /// needs, which is less.
    std::uint32_t guardFrames = 256;
    bool aggressive = false;                   ///< S = N/2 + G instead of N + G
    /// What the card and its converters add, in frames: on the way in,
    /// added to N, and on the way out, added to S. All of it is counted on
    /// the way out, since a loop from an output to an input measures the
    /// sum and not its halves. An OutputLatency that has not been saved is
    /// the card's own figure for the input (cardLatencyFrames, which
    /// loadDriverSettings applies); a saved one stands whatever the input:
    /// it is what was measured on that PC (card-session's step 7).
    std::uint32_t inputLatencyFrames = 0;
    std::uint32_t outputLatencyFrames = cardLatencyFrames(InputChoice::Analog);
    /// Put the driver's routing back when another program changes it (the
    /// AES outputs, the monitor, the Xpand's input select, the input
    /// source). Off by default: the change is only reported, since the
    /// other program may need it more.
    bool keepRouting = false;
    /// The Xpand's analog reference level, what 0 dBFS is in dBu: 24, 18, 15
    /// or 12 (isAnalogLevel). 0, the default, leaves it as the card has it:
    /// it is the whole card's, not this audio system's.
    std::uint32_t analogLevelDbu = 0;
    /// The card in OEM task mode while the driver plays, reasserted if
    /// someone changes it (and the driver's routing with it: AJA's services
    /// had the card meanwhile), and back as it was afterwards unless another
    /// program holds the card then (the design's rule for AJA's services).
    /// On by default since 0.1.17: on the KONA X in standard task mode, the
    /// services moved AES out and the monitor to the audio mixer a second
    /// after the driver set them (Q9, 2026-11-10). Kept as "OemTaskMode":
    /// an "OemTasks" saved by an earlier version, 0 by default then, is not
    /// read.
    bool oemTasks = true;
    /// Bits: the SDI outputs that play the driver's outputs too, all 16
    /// channels embedded (1 is SDI out 1, 2 is SDI out 2). They also offer
    /// outputs 9-16, which only SDI carries. None by default: they carry
    /// video that another program may be playing, as the design has it.
    std::uint32_t sdiOutputs = 0;
    /// The HDMI output plays outputs 1-8 too. Off by default, for the same
    /// reason.
    bool hdmiOutput = false;
};

/// 24, 18, 15 or 12; and 0 when `orNone`.
bool isAnalogLevel(std::uint32_t dbu, bool orNone = false);

/// A power of two from kMinBufferFrames to kMaxBufferFrames.
bool isValidBufferSize(std::uint32_t frames);

/// Where settings live: the registry on Windows, a map in the tests.
class SettingsStore
{
public:
    virtual ~SettingsStore() = default;
    virtual bool readNumber(const char* name, std::uint32_t& value) = 0;
    virtual bool writeNumber(const char* name, std::uint32_t value) = 0;
};

class MemorySettingsStore final : public SettingsStore
{
public:
    bool readNumber(const char* name, std::uint32_t& value) override;
    bool writeNumber(const char* name, std::uint32_t value) override;

    std::map<std::string, std::uint32_t> values;
};

/// What the store says, value by value; whatever is missing or out of range
/// keeps its default.
DriverSettings loadDriverSettings(SettingsStore& store);

/// Writes every value. False if any write failed.
bool saveDriverSettings(SettingsStore& store, const DriverSettings& settings);

/// One "name = value" line per setting, for the log and the panel.
std::string describe(const DriverSettings& settings);

/// The input in a few words: "analog", "AES", "SDI 2 embedded", "HDMI
/// embedded".
std::string describeInput(const DriverSettings& settings);

/// The outputs in a few words: "AES 1-8, analog out", then ", SDI out 2"
/// and ", HDMI out" when they play too.
std::string describeOutputs(const DriverSettings& settings);

} // namespace ajaduplex
