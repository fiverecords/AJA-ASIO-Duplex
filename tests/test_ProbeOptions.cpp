// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "ProbeOptions.h"
#include "DuplexVersion.h"

using namespace ajaduplex::probe;

namespace
{

ParseResult parse(std::vector<std::string> args)
{
    return parseArguments(args);
}

} // namespace

TEST_CASE(ProbeOptions, NoArgumentsMeansHelp)
{
    const ParseResult r = parse({});
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Help);
}

TEST_CASE(ProbeOptions, CommandsByName)
{
    EXPECT_EQ(parse({"list"}).options.command, Command::List);
    EXPECT_EQ(parse({"info"}).options.command, Command::Info);
    EXPECT_EQ(parse({"help"}).options.command, Command::Help);
    EXPECT_EQ(parse({"version"}).options.command, Command::Version);
}

TEST_CASE(ProbeOptions, InfoDefaults)
{
    const ParseResult r = parse({"info"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.device, std::string("0"));
    EXPECT_EQ(r.options.label, std::string());
    EXPECT_EQ(r.options.outputDir, std::string("."));
}

TEST_CASE(ProbeOptions, LongShortAndInlineValues)
{
    const ParseResult a = parse({"info", "--device", "1", "--label", "idle", "--out", "reports"});
    ASSERT_TRUE(a.ok);
    EXPECT_EQ(a.options.device, std::string("1"));
    EXPECT_EQ(a.options.label, std::string("idle"));
    EXPECT_EQ(a.options.outputDir, std::string("reports"));

    const ParseResult b = parse({"info", "-d", "KONA X", "-l", "plugin", "-o", "r"});
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(b.options.device, std::string("KONA X"));
    EXPECT_EQ(b.options.label, std::string("plugin"));
    EXPECT_EQ(b.options.outputDir, std::string("r"));

    const ParseResult c = parse({"info", "--label=AJA drivers"});
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.options.label, std::string("AJA drivers"));
}

TEST_CASE(ProbeOptions, OptionsMayComeBeforeTheCommand)
{
    const ParseResult r = parse({"--label", "idle", "info"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Info);
    EXPECT_EQ(r.options.label, std::string("idle"));
}

TEST_CASE(ProbeOptions, HelpAndVersionWinOverEverything)
{
    EXPECT_EQ(parse({"info", "--help"}).options.command, Command::Help);
    EXPECT_EQ(parse({"-h"}).options.command, Command::Help);
    EXPECT_EQ(parse({"info", "--bogus", "-h"}).options.command, Command::Help);
    EXPECT_EQ(parse({"--version"}).options.command, Command::Version);
    EXPECT_EQ(parse({"bogus", "--version"}).options.command, Command::Version);
    EXPECT_EQ(parse({"--version", "--help"}).options.command, Command::Help);
    EXPECT_TRUE(parse({"bogus", "--version"}).ok);
}

TEST_CASE(ProbeOptions, Errors)
{
    ParseResult r = parse({"info", "--bogus"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "unknown option '--bogus'");

    r = parse({"info", "-x"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "unknown option '-x'");

    r = parse({"info", "--label"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--label needs a value");

    r = parse({"info", "--label="});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--label needs a value");

    r = parse({"list", "--label", "x"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--label does not apply to 'list'");

    r = parse({"info", "list"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "unexpected argument 'list'");

    r = parse({"frobnicate"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "unknown command 'frobnicate'");

    r = parse({"--label", "x"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "needs a command");
}

TEST_CASE(ProbeOptions, UsageMentionsCommandsAndVersion)
{
    const std::string usage = usageText();
    EXPECT_CONTAINS(usage, "duplex-probe list");
    EXPECT_CONTAINS(usage, "duplex-probe info");
    EXPECT_CONTAINS(usage, "duplex-probe heads");
    EXPECT_CONTAINS(usage, "duplex-probe cost");
    EXPECT_CONTAINS(usage, "--simulate");
    EXPECT_CONTAINS(usage, AJADUPLEX_VERSION_STRING);
    EXPECT_EQ(usage.back(), '\n');
}

TEST_CASE(ProbeOptions, HeadsAndCost)
{
    ParseResult r = parse({"heads"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Heads);
    EXPECT_EQ(r.options.audioSystem, 0u);   // last
    EXPECT_EQ(r.options.seconds, 10.0);
    EXPECT_FALSE(r.options.simulate);

    r = parse({"heads", "--audio-system", "1", "--seconds", "2.5", "--simulate", "-l", "sim"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.audioSystem, 1u);
    EXPECT_EQ(r.options.seconds, 2.5);
    EXPECT_TRUE(r.options.simulate);
    EXPECT_EQ(r.options.label, std::string("sim"));

    r = parse({"cost", "-a", "last", "--simulate"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Cost);
    EXPECT_EQ(r.options.audioSystem, 0u);
    EXPECT_TRUE(r.options.simulate);
}

TEST_CASE(ProbeOptions, HeadsAndCostErrors)
{
    ParseResult r = parse({"heads", "--audio-system", "9"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--audio-system takes 1-8 or 'last'");

    r = parse({"heads", "--seconds", "0.5"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--seconds takes a number from 1 to 600");

    r = parse({"heads", "--seconds=abc"});
    EXPECT_FALSE(r.ok);

    r = parse({"heads", "--simulate=yes"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--simulate takes no value");

    r = parse({"info", "--simulate"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--simulate does not apply to 'info'");

    r = parse({"cost", "--seconds", "5"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--seconds does not apply to 'cost'");
}

TEST_CASE(ProbeOptions, Loop)
{
    ParseResult r = parse({"loop"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Loop);
    EXPECT_EQ(r.options.frames, 0u);   // sweep
    EXPECT_EQ(r.options.guard, -1);
    EXPECT_EQ(r.options.seconds, 10.0);
    ASSERT_EQ(r.options.channels.size(), std::size_t(2));
    EXPECT_EQ(r.options.channels[1], 1u);
    EXPECT_EQ(r.options.head, HeadChoice::Conservative);

    r = parse({"loop", "--frames", "128", "--guard", "0", "--seconds", "3600", "--channels", "1-8", "--head", "last",
               "--simulate", "-a", "2", "-l", "exit"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.frames, 128u);
    EXPECT_EQ(r.options.guard, 0);
    EXPECT_EQ(r.options.seconds, 3600.0);
    EXPECT_EQ(r.options.channels.size(), std::size_t(8));
    EXPECT_EQ(r.options.head, HeadChoice::LastByte);
    EXPECT_TRUE(r.options.simulate);
    EXPECT_EQ(r.options.audioSystem, 2u);

    r = parse({"loop", "-n", "32", "-g", "8", "-c", "3,1", "--head=next"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.frames, 32u);
    EXPECT_EQ(r.options.guard, 8);
    ASSERT_EQ(r.options.channels.size(), std::size_t(2));
    EXPECT_EQ(r.options.channels[0], 0u);   // sorted
    EXPECT_EQ(r.options.channels[1], 2u);
    EXPECT_EQ(r.options.head, HeadChoice::NextByte);
}

TEST_CASE(ProbeOptions, LoopErrors)
{
    ParseResult r = parse({"loop", "--frames", "2048"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--frames takes a block size from 16 to 1024");

    r = parse({"loop", "--frames", "8"});
    EXPECT_FALSE(r.ok);

    r = parse({"loop", "--guard", "-4"});
    EXPECT_FALSE(r.ok);

    r = parse({"loop", "--channels", "9"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--channels takes AES channels 1-8");

    r = parse({"loop", "--head", "middle"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--head takes conservative, last or next");

    r = parse({"loop", "--seconds", "90000"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--seconds takes a number from 1 to 86400");

    r = parse({"heads", "--seconds", "3600"});   // heads keeps every sample
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "from 1 to 600");

    r = parse({"heads", "--frames", "128"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--frames does not apply to 'heads'");
}

TEST_CASE(ProbeOptions, ChannelLists)
{
    std::vector<unsigned> c;
    ASSERT_TRUE(parseChannelList("1,3,5-6", 8, c));
    ASSERT_EQ(c.size(), std::size_t(4));
    EXPECT_EQ(c[0], 0u);
    EXPECT_EQ(c[1], 2u);
    EXPECT_EQ(c[2], 4u);
    EXPECT_EQ(c[3], 5u);

    ASSERT_TRUE(parseChannelList("1-2,2,1", 8, c));
    EXPECT_EQ(c.size(), std::size_t(2));

    ASSERT_TRUE(parseChannelList("16", 16, c));
    EXPECT_EQ(c[0], 15u);

    const std::vector<unsigned> before = c;
    EXPECT_FALSE(parseChannelList("", 8, c));
    EXPECT_FALSE(parseChannelList("2-1", 8, c));
    EXPECT_FALSE(parseChannelList("1,,2", 8, c));
    EXPECT_FALSE(parseChannelList("0", 8, c));
    EXPECT_FALSE(parseChannelList("1-9", 8, c));
    EXPECT_FALSE(parseChannelList("a", 8, c));
    EXPECT_FALSE(parseChannelList("1-", 8, c));
    EXPECT_EQ(c, before);   // untouched on failure
}

TEST_CASE(ProbeOptions, LoopPath)
{
    ParseResult r = parse({"loop"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.path, PathChoice::Aes);

    r = parse({"loop", "--path", "analog", "--frames", "128", "--guard", "32", "--seconds", "3600"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.path, PathChoice::Analog);
    EXPECT_FALSE(r.options.channelsGiven);

    r = parse({"loop", "-p", "aes", "-c", "1-8"});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.options.channelsGiven);

    r = parse({"loop", "--path", "analog", "--channels", "1-2"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--channels is for the AES paths");

    r = parse({"loop", "--path", "analog", "--seconds", "2"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--path analog needs --seconds 3 or more");

    // Since 0.1.17 aes plays tones (the KONA X's AES inputs resample), also
    // judged by whole seconds; the words, as before, are aes-words.
    r = parse({"loop", "--seconds", "2"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--path aes needs --seconds 3 or more");

    r = parse({"loop", "--path", "aes-words", "--channels", "1-8", "--seconds", "1"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.path, PathChoice::AesWords);
    EXPECT_TRUE(r.options.channelsGiven);

    r = parse({"loop", "--path", "xlr"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--path takes aes, aes-words or analog");

    r = parse({"heads", "--path", "analog"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "does not apply to 'heads'");
}

TEST_CASE(ProbeOptions, Routing)
{
    ParseResult r = parse({"routing"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Routing);
    EXPECT_FALSE(r.options.scanSelect);

    r = parse({"routing", "--scan-select", "--simulate", "-a", "2", "-l", "q14"});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.options.scanSelect);
    EXPECT_TRUE(r.options.simulate);

    r = parse({"loop", "--scan-select"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "does not apply to 'loop'");

    r = parse({"routing", "--seconds", "5"});
    EXPECT_FALSE(r.ok);
}

TEST_CASE(ProbeOptions, TaskMode)
{
    // In standard task mode AJA's services may re-route while a loop runs
    // (the KONA X's first sweep, 2026-11-10): loop and routing hold OEM
    // unless told to keep the card's mode.
    ParseResult r = parse({"loop"});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.options.oemTasks);
    r = parse({"loop", "--tasks", "keep"});
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.options.oemTasks);
    r = parse({"routing", "--tasks=oem"});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.options.oemTasks);
    r = parse({"loop", "--tasks", "standard"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--tasks takes oem or keep");
    r = parse({"watch", "--tasks", "oem"});   // watch is there to see what the services do
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "does not apply to 'watch'");
}

TEST_CASE(ProbeOptions, Watch)
{
    ParseResult r = parse({"watch"});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.options.command, Command::Watch);
    EXPECT_TRUE(r.options.seconds == 60.0);   // minutes matter here, not seconds
    EXPECT_TRUE(r.options.analogInput);       // the driver's default input (D2)
    EXPECT_FALSE(r.options.keep);

    r = parse({"watch", "--seconds", "300", "--input", "aes", "--keep", "--simulate", "-a", "2", "-l", "services"});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.options.seconds == 300.0);
    EXPECT_FALSE(r.options.analogInput);
    EXPECT_TRUE(r.options.keep);
    EXPECT_TRUE(r.options.simulate);
    EXPECT_EQ(r.options.audioSystem, 2u);
    EXPECT_TRUE(parse({"watch", "-i", "analog"}).options.analogInput);
    EXPECT_TRUE(parse({"heads"}).options.seconds == 10.0);   // the others keep their default

    r = parse({"watch", "--input", "sdi"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "--input takes analog or aes");
    r = parse({"loop", "--keep"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(r.error, "does not apply to 'loop'");
    r = parse({"watch", "--frames", "64"});
    EXPECT_FALSE(r.ok);
    EXPECT_CONTAINS(usageText(), "duplex-probe watch");
}
