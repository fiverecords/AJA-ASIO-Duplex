// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "common/DriverSettings.h"

using namespace ajaduplex;

TEST_CASE(DriverSettings, DefaultsWhenNothingIsStored)
{
    MemorySettingsStore store;
    const DriverSettings s = loadDriverSettings(store);
    EXPECT_EQ(s.card, 0u);
    EXPECT_EQ(s.audioSystem, 0u);
    EXPECT_TRUE(s.input == InputChoice::Analog);   // D2
    EXPECT_EQ(s.bufferFrames, 128u);
    EXPECT_EQ(s.guardFrames, 256u);
    EXPECT_FALSE(s.aggressive);
    EXPECT_EQ(s.inputLatencyFrames, 0u);
    EXPECT_EQ(s.outputLatencyFrames, 208u);   // the KONA X's own, XLR out to XLR in
    EXPECT_FALSE(s.keepRouting);   // other programs' changes are reported, not undone
    EXPECT_EQ(s.analogLevelDbu, 0u);  // the Xpand's level as the card has it
    EXPECT_EQ(s.sdiInput, 1u);
    EXPECT_TRUE(s.oemTasks);          // Q9: AJA's services re-route the outputs in standard task mode
    EXPECT_EQ(s.sdiOutputs, 0u);      // their video may be another program's
    EXPECT_FALSE(s.hdmiOutput);
    EXPECT_EQ(describeOutputs(s), std::string("AES 1-8, analog out"));
}

TEST_CASE(DriverSettings, RoundTripThroughTheStore)
{
    DriverSettings s;
    s.card = 1;
    s.audioSystem = 2;
    s.input = InputChoice::Aes;
    s.bufferFrames = 256;
    s.guardFrames = 48;
    s.aggressive = true;
    s.inputLatencyFrames = 33;
    s.outputLatencyFrames = 41;
    s.keepRouting = true;
    s.analogLevelDbu = 15;
    s.sdiInput = 2;
    s.oemTasks = true;
    s.sdiOutputs = 2;
    s.hdmiOutput = true;
    MemorySettingsStore store;
    ASSERT_TRUE(saveDriverSettings(store, s));
    EXPECT_EQ(store.values.size(), std::size_t(14));
    EXPECT_EQ(store.values["SdiOutputs"], 2u);
    EXPECT_EQ(store.values["HdmiOutput"], 1u);
    EXPECT_EQ(store.values["OemTaskMode"], 1u);
    EXPECT_EQ(store.values["SdiInput"], 2u);
    EXPECT_EQ(store.values["KeepRouting"], 1u);
    EXPECT_EQ(store.values["AnalogLevel"], 15u);
    EXPECT_EQ(store.values["BufferSize"], 256u);
    const DriverSettings back = loadDriverSettings(store);
    EXPECT_EQ(back.card, 1u);
    EXPECT_EQ(back.audioSystem, 2u);
    EXPECT_TRUE(back.input == InputChoice::Aes);
    EXPECT_EQ(back.bufferFrames, 256u);
    EXPECT_EQ(back.guardFrames, 48u);
    EXPECT_TRUE(back.aggressive);
    EXPECT_EQ(back.inputLatencyFrames, 33u);
    EXPECT_EQ(back.outputLatencyFrames, 41u);
    EXPECT_TRUE(back.keepRouting);
    EXPECT_EQ(back.analogLevelDbu, 15u);
    EXPECT_EQ(back.sdiInput, 2u);
    EXPECT_TRUE(back.oemTasks);
    EXPECT_EQ(back.sdiOutputs, 2u);
    EXPECT_TRUE(back.hdmiOutput);
    EXPECT_EQ(describeOutputs(back), std::string("AES 1-8, analog out, SDI out 2, HDMI out"));

    // The embedded inputs.
    store.values["Input"] = 2;
    EXPECT_TRUE(loadDriverSettings(store).input == InputChoice::Sdi);
    EXPECT_EQ(describeInput(loadDriverSettings(store)), std::string("SDI 2 embedded"));
    store.values["Input"] = 3;
    EXPECT_TRUE(loadDriverSettings(store).input == InputChoice::Hdmi);
    EXPECT_EQ(describeInput(loadDriverSettings(store)), std::string("HDMI embedded"));
    EXPECT_TRUE(isXpandInput(InputChoice::Aes) && !isXpandInput(InputChoice::Sdi));
}

TEST_CASE(DriverSettings, BadValuesKeepTheirDefaults)
{
    MemorySettingsStore store;
    store.values["BufferSize"] = 100;   // not a power of two
    store.values["Guard"] = 5000;
    store.values["Input"] = 7;
    store.values["SdiInput"] = 0;
    store.values["Aggressive"] = 2;
    store.values["AudioSystem"] = 9;
    store.values["OutputLatency"] = 1000000;
    store.values["KeepRouting"] = 5;
    store.values["AnalogLevel"] = 20;   // not one of the Xpand's
    store.values["Card"] = 3;           // fine
    store.values["SdiOutputs"] = 256;   // SDI out 9: no card has it
    store.values["HdmiOutput"] = 2;
    const DriverSettings s = loadDriverSettings(store);
    EXPECT_EQ(s.sdiOutputs, 0u);
    EXPECT_FALSE(s.hdmiOutput);
    EXPECT_EQ(s.bufferFrames, 128u);
    EXPECT_EQ(s.guardFrames, 256u);
    EXPECT_TRUE(s.input == InputChoice::Analog);
    EXPECT_EQ(s.sdiInput, 1u);
    EXPECT_FALSE(s.aggressive);
    EXPECT_EQ(s.audioSystem, 0u);
    EXPECT_EQ(s.outputLatencyFrames, 208u);   // not a latency: the card's own, as if it were not there
    EXPECT_FALSE(s.keepRouting);
    EXPECT_EQ(s.analogLevelDbu, 0u);
    EXPECT_EQ(s.card, 3u);
}

TEST_CASE(DriverSettings, BufferSizes)
{
    EXPECT_TRUE(isValidBufferSize(64));
    EXPECT_TRUE(isValidBufferSize(128));
    EXPECT_TRUE(isValidBufferSize(2048));
    EXPECT_FALSE(isValidBufferSize(32));
    EXPECT_FALSE(isValidBufferSize(4096));
    EXPECT_FALSE(isValidBufferSize(96));
    EXPECT_FALSE(isValidBufferSize(0));
}

TEST_CASE(DriverSettings, OemTaskModeIsOnUnlessTurnedOffSince0117)
{
    // Up to 0.1.16 the value was "OemTasks", 0 by default, and the dialog
    // saved it with the rest: a 0 from then was no choice, and is not read.
    MemorySettingsStore old;
    old.values["OemTasks"] = 0;
    EXPECT_TRUE(loadDriverSettings(old).oemTasks);

    MemorySettingsStore off;
    off.values["OemTaskMode"] = 0;
    EXPECT_FALSE(loadDriverSettings(off).oemTasks);
    DriverSettings s;
    s.oemTasks = false;
    MemorySettingsStore saved;
    ASSERT_TRUE(saveDriverSettings(saved, s));
    EXPECT_EQ(saved.values["OemTaskMode"], 0u);
    EXPECT_EQ(saved.values.count("OemTasks"), std::size_t(0));
    EXPECT_FALSE(loadDriverSettings(saved).oemTasks);
}

// The card's own latency is the driver's to know: a PC where nothing was
// measured reports it, by input. What was measured on a PC and saved stands.
TEST_CASE(DriverSettings, TheCardsLatencyIsTheDefaultForTheInput)
{
    EXPECT_EQ(DriverSettings::cardLatencyFrames(InputChoice::Analog), 208u);
    EXPECT_EQ(DriverSettings::cardLatencyFrames(InputChoice::Aes), 130u);
    EXPECT_EQ(DriverSettings::cardLatencyFrames(InputChoice::Sdi), 0u);    // not measured
    EXPECT_EQ(DriverSettings::cardLatencyFrames(InputChoice::Hdmi), 0u);
    // As the settings are made, they are the default input's.
    EXPECT_EQ(DriverSettings{}.outputLatencyFrames, DriverSettings::cardLatencyFrames(DriverSettings{}.input));

    MemorySettingsStore aes;
    aes.values["Input"] = 1;
    EXPECT_EQ(loadDriverSettings(aes).outputLatencyFrames, 130u);
    EXPECT_EQ(loadDriverSettings(aes).inputLatencyFrames, 0u);   // all of it on the way out

    MemorySettingsStore sdi;
    sdi.values["Input"] = 2;
    EXPECT_EQ(loadDriverSettings(sdi).outputLatencyFrames, 0u);

    // Saved, it is what that PC measured: also 0, and whatever the input.
    MemorySettingsStore measured;
    measured.values["OutputLatency"] = 215;
    EXPECT_EQ(loadDriverSettings(measured).outputLatencyFrames, 215u);
    measured.values["Input"] = 1;
    EXPECT_EQ(loadDriverSettings(measured).outputLatencyFrames, 215u);
    MemorySettingsStore none;
    none.values["OutputLatency"] = 0;
    EXPECT_EQ(loadDriverSettings(none).outputLatencyFrames, 0u);

    // Saving writes it, so what a PC has shown its user stays.
    MemorySettingsStore saved;
    ASSERT_TRUE(saveDriverSettings(saved, loadDriverSettings(aes)));
    EXPECT_EQ(saved.values["OutputLatency"], 130u);
}

TEST_CASE(DriverSettings, DescribeNamesEveryValue)
{
    const std::string text = describe(DriverSettings{});
    EXPECT_CONTAINS(text, "Input = 0 (analog)");
    EXPECT_CONTAINS(text, "BufferSize = 128");
    EXPECT_CONTAINS(text, "Guard = 256");
    EXPECT_CONTAINS(text, "AudioSystem = 0 (the card's last)");
    EXPECT_CONTAINS(text, "InputLatency = 0");
    EXPECT_CONTAINS(text, "OutputLatency = 208");
    EXPECT_CONTAINS(text, "KeepRouting = 0");
    EXPECT_CONTAINS(text, "AnalogLevel = 0 (as the card has it)");
    EXPECT_CONTAINS(text, "OemTaskMode = 1");
    EXPECT_CONTAINS(text, "SdiOutputs = 0 (none)");
    EXPECT_CONTAINS(text, "HdmiOutput = 0");
    DriverSettings both;
    both.sdiOutputs = 3;
    EXPECT_CONTAINS(describe(both), "SdiOutputs = 3 (SDI out 1, 2)");
    DriverSettings level;
    level.analogLevelDbu = 18;
    EXPECT_CONTAINS(describe(level), "AnalogLevel = 18 (+18 dBu)");
    EXPECT_TRUE(isAnalogLevel(12) && isAnalogLevel(24) && !isAnalogLevel(0) && isAnalogLevel(0, true));
    EXPECT_FALSE(isAnalogLevel(20, true));
}
