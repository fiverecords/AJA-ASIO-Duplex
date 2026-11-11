// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "ProbeOptions.h"

#include "DuplexVersion.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <utility>

namespace ajaduplex::probe
{

namespace
{

using Apply = std::function<bool(Options&, const std::string& value, std::string& error)>;

struct OptionSpec
{
    const char* longName;
    char shortName;
    bool takesValue;
    std::vector<Command> commands;   ///< Commands that accept the option.
    Apply apply;
};

bool parseAudioSystem(const std::string& value, unsigned& out)
{
    if (value == "last")
    {
        out = 0;
        return true;
    }
    if (value.size() != 1 || value[0] < '1' || value[0] > '8')
        return false;
    out = unsigned(value[0] - '0');
    return true;
}

bool parseSeconds(const std::string& value, double highest, double& out)
{
    char* end = nullptr;
    const double v = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !(v >= 1.0 && v <= highest))
        return false;
    out = v;
    return true;
}

bool parseUnsigned(const std::string& value, unsigned low, unsigned high, unsigned& out)
{
    if (value.empty() || value.size() > 6)
        return false;
    unsigned v = 0;
    for (const char c : value)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + unsigned(c - '0');
    }
    if (v < low || v > high)
        return false;
    out = v;
    return true;
}

const std::vector<OptionSpec>& optionSpecs()
{
    static const std::vector<Command> cardCommands = {Command::Info, Command::Heads, Command::Cost, Command::Loop,
                                                      Command::Routing, Command::Watch};
    static const std::vector<Command> audioCommands = {Command::Heads, Command::Cost, Command::Loop, Command::Routing,
                                                       Command::Watch};
    static const std::vector<OptionSpec> specs = {
        {"device", 'd', true, cardCommands,
         [](Options& o, const std::string& v, std::string&) { o.device = v; return true; }},
        {"label", 'l', true, cardCommands,
         [](Options& o, const std::string& v, std::string&) { o.label = v; return true; }},
        {"out", 'o', true, cardCommands,
         [](Options& o, const std::string& v, std::string&) { o.outputDir = v; return true; }},
        {"audio-system", 'a', true, audioCommands,
         [](Options& o, const std::string& v, std::string& e) {
             if (parseAudioSystem(v, o.audioSystem))
                 return true;
             e = "--audio-system takes 1-8 or 'last', not '" + v + "'";
             return false;
         }},
        {"seconds", 's', true, {Command::Heads, Command::Loop, Command::Watch},
         [](Options& o, const std::string& v, std::string& e) {
             // heads keeps every sample in memory; a loop run keeps only counts.
             const double highest = o.command == Command::Heads ? 600.0 : 86400.0;
             o.secondsGiven = true;
             if (parseSeconds(v, highest, o.seconds))
                 return true;
             e = "--seconds takes a number from 1 to " + std::to_string(static_cast<int>(highest)) + ", not '" + v +
                 "'";
             return false;
         }},
        {"simulate", '\0', false, audioCommands,
         [](Options& o, const std::string&, std::string&) { o.simulate = true; return true; }},
        {"frames", 'n', true, {Command::Loop},
         [](Options& o, const std::string& v, std::string& e) {
             if (parseUnsigned(v, 16, 1024, o.frames))
                 return true;
             e = "--frames takes a block size from 16 to 1024, not '" + v + "'";
             return false;
         }},
        {"guard", 'g', true, {Command::Loop},
         [](Options& o, const std::string& v, std::string& e) {
             unsigned g = 0;
             if (parseUnsigned(v, 0, 1024, g))
             {
                 o.guard = static_cast<int>(g);
                 return true;
             }
             e = "--guard takes a number of frames from 0 to 1024, not '" + v + "'";
             return false;
         }},
        {"channels", 'c', true, {Command::Loop},
         [](Options& o, const std::string& v, std::string& e) {
             if (parseChannelList(v, 8, o.channels))
             {
                 o.channelsGiven = true;
                 return true;
             }
             e = "--channels takes AES channels 1-8 like 1-2 or 1,2,5-8, not '" + v + "'";
             return false;
         }},
        {"scan-select", '\0', false, {Command::Routing},
         [](Options& o, const std::string&, std::string&) { o.scanSelect = true; return true; }},
        {"path", 'p', true, {Command::Loop},
         [](Options& o, const std::string& v, std::string& e) {
             if (v == "aes") o.path = PathChoice::Aes;
             else if (v == "aes-words") o.path = PathChoice::AesWords;
             else if (v == "analog") o.path = PathChoice::Analog;
             else
             {
                 e = "--path takes aes, aes-words or analog, not '" + v + "'";
                 return false;
             }
             return true;
         }},
        {"input", 'i', true, {Command::Watch},
         [](Options& o, const std::string& v, std::string& e) {
             if (v == "analog") o.analogInput = true;
             else if (v == "aes") o.analogInput = false;
             else
             {
                 e = "--input takes analog or aes, not '" + v + "'";
                 return false;
             }
             return true;
         }},
        {"tasks", '\0', true, {Command::Loop, Command::Routing},
         [](Options& o, const std::string& v, std::string& e) {
             if (v == "oem") o.oemTasks = true;
             else if (v == "keep") o.oemTasks = false;
             else
             {
                 e = "--tasks takes oem or keep, not '" + v + "'";
                 return false;
             }
             return true;
         }},
        {"keep", '\0', false, {Command::Watch},
         [](Options& o, const std::string&, std::string&) { o.keep = true; return true; }},
        {"head", '\0', true, {Command::Loop},
         [](Options& o, const std::string& v, std::string& e) {
             if (v == "conservative") o.head = HeadChoice::Conservative;
             else if (v == "last") o.head = HeadChoice::LastByte;
             else if (v == "next") o.head = HeadChoice::NextByte;
             else
             {
                 e = "--head takes conservative, last or next, not '" + v + "'";
                 return false;
             }
             return true;
         }},
    };
    return specs;
}

const OptionSpec* findLong(const std::string& name)
{
    for (const OptionSpec& spec : optionSpecs())
    {
        if (name == spec.longName)
            return &spec;
    }
    return nullptr;
}

const OptionSpec* findShort(char name)
{
    for (const OptionSpec& spec : optionSpecs())
    {
        if (spec.shortName != '\0' && name == spec.shortName)
            return &spec;
    }
    return nullptr;
}

bool commandFromName(const std::string& name, Command& out)
{
    if (name == "list")    { out = Command::List;    return true; }
    if (name == "info")    { out = Command::Info;    return true; }
    if (name == "heads")   { out = Command::Heads;   return true; }
    if (name == "cost")    { out = Command::Cost;    return true; }
    if (name == "loop")    { out = Command::Loop;    return true; }
    if (name == "routing") { out = Command::Routing; return true; }
    if (name == "watch")   { out = Command::Watch;   return true; }
    if (name == "help")    { out = Command::Help;    return true; }
    if (name == "version") { out = Command::Version; return true; }
    return false;
}

const char* commandName(Command command)
{
    switch (command)
    {
        case Command::Help:    return "help";
        case Command::Version: return "version";
        case Command::List:    return "list";
        case Command::Info:    return "info";
        case Command::Heads:   return "heads";
        case Command::Cost:    return "cost";
        case Command::Loop:    return "loop";
        case Command::Routing: return "routing";
        case Command::Watch:   return "watch";
    }
    return "?";
}

} // namespace

bool parseChannelList(const std::string& text, unsigned highest, std::vector<unsigned>& out)
{
    std::vector<unsigned> channels;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const std::size_t dash = item.find('-');
        unsigned first = 0, last = 0;
        if (dash == std::string::npos)
        {
            if (!parseUnsigned(item, 1, highest, first))
                return false;
            last = first;
        }
        else if (!parseUnsigned(item.substr(0, dash), 1, highest, first) ||
                 !parseUnsigned(item.substr(dash + 1), 1, highest, last) || last < first)
            return false;
        for (unsigned c = first; c <= last; ++c)
            channels.push_back(c - 1);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    std::sort(channels.begin(), channels.end());
    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
    if (channels.empty())
        return false;
    out = channels;
    return true;
}

namespace
{

ParseResult failure(std::string message)
{
    ParseResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

} // namespace

ParseResult parseArguments(const std::vector<std::string>& args)
{
    struct Given
    {
        const OptionSpec* spec;
        std::string spelled;   ///< As typed, without "=value".
        std::string value;
    };

    ParseResult result;

    // Help, then version, win over everything else on the line, wherever
    // they are and whatever else is wrong with it.
    for (const std::string& arg : args)
    {
        if (arg == "-h" || arg == "--help")
        {
            result.options.command = Command::Help;
            return result;
        }
    }
    for (const std::string& arg : args)
    {
        if (arg == "--version")
        {
            result.options.command = Command::Version;
            return result;
        }
    }

    bool haveCommand = false;
    std::vector<Given> given;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];

        if (arg.size() > 1 && arg[0] == '-')
        {
            const OptionSpec* spec = nullptr;
            std::string spelled = arg;
            std::string value;
            bool hasInlineValue = false;

            if (arg.compare(0, 2, "--") == 0)
            {
                std::string name = arg.substr(2);
                const std::size_t eq = name.find('=');
                if (eq != std::string::npos)
                {
                    value = name.substr(eq + 1);
                    name.resize(eq);
                    hasInlineValue = true;
                    spelled = "--" + name;
                }
                spec = findLong(name);
            }
            else if (arg.size() == 2)
            {
                spec = findShort(arg[1]);
            }

            if (spec == nullptr)
                return failure("unknown option '" + arg + "'");
            if (!spec->takesValue)
            {
                if (hasInlineValue)
                    return failure("option " + spelled + " takes no value");
            }
            else
            {
                if (!hasInlineValue)
                {
                    if (i + 1 >= args.size())
                        return failure("option " + spelled + " needs a value");
                    value = args[++i];
                }
                if (value.empty())
                    return failure("option " + spelled + " needs a value");
            }
            given.push_back(Given{spec, spelled, value});
            continue;
        }

        if (haveCommand)
            return failure("unexpected argument '" + arg + "'");
        if (!commandFromName(arg, result.options.command))
            return failure("unknown command '" + arg + "'");
        haveCommand = true;
    }

    for (const Given& option : given)
    {
        if (!haveCommand)
            return failure("option " + option.spelled + " needs a command before or after it");
        const std::vector<Command>& accepted = option.spec->commands;
        if (std::find(accepted.begin(), accepted.end(), result.options.command) == accepted.end())
            return failure(std::string("option ") + option.spelled + " does not apply to '" +
                           commandName(result.options.command) + "'");
        std::string error;
        if (!option.spec->apply(result.options, option.value, error))
            return failure(error);
    }

    // watch is about minutes, not seconds.
    if (result.options.command == Command::Watch && !result.options.secondsGiven)
        result.options.seconds = 60.0;

    // The analog loop is L and R; tones are judged by whole seconds.
    const Options& o = result.options;
    if (o.command == Command::Loop && o.path == PathChoice::Analog && o.channelsGiven)
        return failure("--channels is for the AES paths; the analog loop is always L and R");
    if (o.command == Command::Loop && o.path != PathChoice::AesWords && o.seconds < 3.0)
        return failure(std::string("--path ") + (o.path == PathChoice::Analog ? "analog" : "aes") +
                       " needs --seconds 3 or more (its tones are checked by whole seconds)");
    return result;
}

std::string usageText()
{
    return std::string("duplex-probe ") + AJADUPLEX_VERSION_STRING +
           " - phase 0 measurement tool of AJA-Duplex-ASIO\n"
           "\n"
           "Usage:\n"
           "  duplex-probe list\n"
           "  duplex-probe info  [--device <d>] [--label <text>] [--out <folder>]\n"
           "  duplex-probe heads [--device <d>] [--audio-system <n>] [--seconds <s>] [--simulate]\n"
           "                     [--label <text>] [--out <folder>]\n"
           "  duplex-probe cost  [--device <d>] [--audio-system <n>] [--simulate]\n"
           "                     [--label <text>] [--out <folder>]\n"
           "  duplex-probe loop  [--path aes|aes-words|analog] [--frames <N>] [--guard <G>] [--seconds <s>]\n"
           "                     [--channels <list>] [--head conservative|last|next] [--tasks oem|keep]\n"
           "                     [--device <d>] [--audio-system <n>] [--simulate] [--label <text>]\n"
           "                     [--out <folder>]\n"
           "  duplex-probe routing [--scan-select] [--tasks oem|keep] [--device <d>] [--audio-system <n>]\n"
           "                     [--simulate] [--label <text>] [--out <folder>]\n"
           "  duplex-probe watch [--seconds <s>] [--input analog|aes] [--keep] [--device <d>]\n"
           "                     [--audio-system <n>] [--simulate] [--label <text>] [--out <folder>]\n"
           "  duplex-probe help | version\n"
           "\n"
           "Commands:\n"
           "  list    The AJA devices the driver sees.\n"
           "  info    Read-only snapshot of the card's audio state: audio systems, routing,\n"
           "          Xpand, audio mixer, AutoCirculate and the SDRAM map. Changes nothing.\n"
           "  heads   How the audio heads move: step sizes, last byte or next, rate against\n"
           "          the PC and the 48 kHz counter, erase mode, audio events, capture enable.\n"
           "          Uses one audio system (no cables) and puts it back as it was. ~25 s.\n"
           "  cost    Register read, DMA of 4/8/16 KB (plain and locked) and wake-up timer\n"
           "          lateness, and a first estimate of the output guard G. ~15 s.\n"
           "  loop    The engine itself with AES out cabled to AES in: a tone on each looped\n"
           "          channel, checked for glitches, slips and level, and the round trip\n"
           "          (the KONA X's AES inputs resample: no word comes back as sent). With\n"
           "          --path aes-words, every frame word by word instead, where the inputs\n"
           "          do not resample: frames lost, repeated or silent, the exact round\n"
           "          trip. With --path analog, XLR out L/R cabled to XLR in L/R: tones\n"
           "          through the converters, their round trip, gain and noise. Without\n"
           "          --frames and --guard it sweeps N 256/128/64/32 against G 64/32/16/8\n"
           "          (16 runs of --seconds); --frames 128 --guard 256 --seconds 3600, the\n"
           "          driver's own guard, on each path, is the exit test of phase 0.\n"
           "  routing A tone on each of the 16 outputs, AES 1-8 and XLR L/R looped: where\n"
           "          the Xpand's analog output takes its signal from (Q4), what the analog\n"
           "          input replaces (Q5); with --scan-select, what each value of the\n"
           "          Xpand's select does (Q14). ~2 s, ~8 s with the scan.\n"
           "  watch   The audio system set up and running as the driver's, in silence, and\n"
           "          its routing read back once a second, as the driver does: every change\n"
           "          another program makes is printed as it comes. Left alone for five\n"
           "          minutes in standard task mode, it answers Q9 (do AJA's services rewrite\n"
           "          the audio?). No cables needed. 60 s unless --seconds says otherwise.\n"
           "\n"
           "Each command prints a summary and writes <command>-<label>-<date>-<time>.txt;\n"
           "heads, cost and loop also write a .csv with the raw data.\n"
           "\n"
           "Options:\n"
           "  -d, --device        Device to open: index (default 0), serial number or model.\n"
           "  -a, --audio-system  Audio system to use, 1-8 or 'last' (default: last, 2 on a\n"
           "                      KONA X). It must not be in use by another program.\n"
           "  -s, --seconds       heads: how long to sample the heads, 1-600 (default 10).\n"
           "                      loop: how long each run lasts, 1-86400 (default 10).\n"
           "                      watch: how long to watch, 1-86400 (default 60).\n"
           "  -n, --frames        loop: block size N, 16-1024 (default: sweep).\n"
           "  -g, --guard         loop: output guard G, 0-1024 frames (default: sweep).\n"
           "      --scan-select   routing: also write the Xpand's select with every value\n"
           "                      0-15 (the SDK only writes 0 and 1); it is put back after.\n"
           "  -p, --path          loop: aes (the default), aes-words or analog. The analog\n"
           "                      loop plays -20 dBFS tones: only the loop cables on the XLR\n"
           "                      outputs. aes plays -40 dBFS tones, which the KONA X's\n"
           "                      analog output plays too (AES 1-2).\n"
           "  -c, --channels      loop: the AES channels cabled out to in, 1-8: 1-2 for one\n"
           "                      BNC on pair 1-2 (the default), 1-8 for four. Only these\n"
           "                      outputs carry the test signal.\n"
           "  -i, --input         watch: the input to set up, analog (the default, as the\n"
           "                      driver's) or aes.\n"
           "      --keep          watch: put the routing back when another program changes\n"
           "                      it, as the driver's Keep my routing does.\n"
           "      --head          loop: how to read LastIn once heads has said (default\n"
           "                      conservative, safe either way).\n"
           "      --tasks         loop, routing: oem (the default) holds the card in OEM task\n"
           "                      mode while they run, so AJA's services leave the routing\n"
           "                      alone, and puts it back after; keep leaves it as it is.\n"
           "      --simulate      Run against the simulated card instead (no hardware needed;\n"
           "                      the timer part of cost still measures this PC).\n"
           "  -l, --label         Short text for the report's file name and header, e.g. idle.\n"
           "  -o, --out           Folder for the report files (default: the current folder).\n";
}

} // namespace ajaduplex::probe
