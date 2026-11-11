// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/CardOpeners.h"

#include "sim/FakeCardAudio.h"

#if AJADUPLEX_WITH_NTV2
#include "device/Ntv2CardAudio.h"
#endif

namespace ajaduplex::asio
{

namespace
{

/// The simulated card, cabled as the settings want it: XLR and AES looped;
/// the SDI connectors the settings play to send and the others receive, and
/// with an SDI input the first of them is looped to it, so the outputs come
/// back on the inputs there too.
sim::FakeCardModel simulatedModel(const DriverSettings& settings)
{
    sim::FakeCardModel model = sim::FakeCardModel::withAnalogLoop();
    model.sdiSending = settings.sdiOutputs & ((1u << model.sdiOutputs) - 1u);
    if (settings.input == InputChoice::Sdi && settings.sdiInput >= 1)
    {
        const unsigned input = settings.sdiInput - 1;
        model.sdiSending &= ~(1u << input);
        for (unsigned n = 0; n < model.sdiOutputs; ++n)
        {
            if ((model.sdiSending & (1u << n)) == 0)
                continue;
            model.sdiLoop.looped = true;
            model.sdiLoop.output = n;
            model.sdiLoop.input = input;
            break;
        }
    }
    return model;
}

} // namespace

std::unique_ptr<device::CardAudio> Ntv2Opener::open(const DriverSettings& settings, std::string& error)
{
#if AJADUPLEX_WITH_NTV2
    return device::openNtv2CardAudio(std::to_string(settings.card), settings.audioSystem, error,
                                     "in the driver's settings (Audio system)");
#else
    // Only the checks made off Windows build the DLL without the SDK.
    (void)settings;
    error = "This build of the driver has no AJA SDK: only the simulated card works.";
    return nullptr;
#endif
}

std::unique_ptr<device::CardAudio> SimulatedOpener::open(const DriverSettings& settings, std::string& error)
{
    (void)error;
    return std::make_unique<sim::FakeCardAudio>(clock_, simulatedModel(settings));
}

bool SimulatedOpener::hardwareLatencies(const DriverSettings& settings, std::uint32_t& inputFrames,
                                        std::uint32_t& outputFrames)
{
    // The simulated card's output fetches ahead of what it plays; what it
    // plays reaches the input through the converters (analog) or the cable
    // (AES). The split between in and out is a convention: hosts add them.
    // Its video inputs carry tones made where they are captured, unless an
    // SDI output is looped to the SDI input.
    const sim::FakeCardModel model = simulatedModel(settings);
    outputFrames = model.outputPrefetchFrames;
    switch (settings.input)
    {
        case InputChoice::Analog: inputFrames = model.analog.delayFrames; break;
        case InputChoice::Aes:    inputFrames = model.cableDelayFrames; break;
        case InputChoice::Sdi:    inputFrames = model.sdiLoop.looped ? model.sdiLoop.delayFrames : 0; break;
        case InputChoice::Hdmi:   inputFrames = 0; break;
    }
    return true;
}

} // namespace ajaduplex::asio
