// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    ProbeOptions.h
 * @brief   Command line of duplex-probe.
 *
 *   duplex-probe <command> [options]
 *
 * One command per run. Options take their value as the next argument or
 * after '=' (--label=idle); --simulate takes none. Each command accepts only
 * the options it uses, so a typo is reported instead of silently ignored.
 */

#pragma once

#include <string>
#include <vector>

namespace ajaduplex::probe
{

enum class Command
{
    Help,
    Version,
    List,
    Info,
    Heads,
    Cost,
    Loop,
    Routing,
    Watch,
};

/// Which loop: AES out to AES in (tones, or word by word), or XLR out to
/// XLR in.
enum class PathChoice
{
    Aes,
    AesWords,
    Analog,
};

/// How loop reads LastIn (engine::HeadMode, without pulling the engine in).
enum class HeadChoice
{
    Conservative,
    LastByte,
    NextByte,
};

struct Options
{
    Command command = Command::Help;
    std::string device = "0";      ///< --device: index, serial number or model name
    std::string label;             ///< --label: free text for the report name and header
    std::string outputDir = ".";   ///< --out: folder for report files
    unsigned audioSystem = 0;      ///< --audio-system: 1-based, 0 = the card's last
    double seconds = 10.0;         ///< --seconds: heads sampling time, loop time per run, watch time (60 by default)
    bool secondsGiven = false;
    bool simulate = false;         ///< --simulate: run against the simulated card
    unsigned frames = 0;           ///< --frames: loop block size N (0: sweep)
    int guard = -1;                ///< --guard: loop guard G (-1: sweep)
    std::vector<unsigned> channels = {0, 1};   ///< --channels: looped AES channels, 0-based
    bool channelsGiven = false;
    PathChoice path = PathChoice::Aes;          ///< --path
    bool scanSelect = false;                    ///< --scan-select
    HeadChoice head = HeadChoice::Conservative; ///< --head
    bool analogInput = true;                    ///< --input: watch's input, analog (the driver's default) or aes
    bool keep = false;                          ///< --keep: watch puts the routing back
    bool oemTasks = true;                       ///< --tasks: loop and routing hold OEM task mode (keep: as it is)
};

/// "1-2", "1-8", "1,3,5-6" into 0-based channel numbers, sorted, without
/// repeats; channels are 1 to `highest`. False on anything else.
bool parseChannelList(const std::string& text, unsigned highest, std::vector<unsigned>& out);

struct ParseResult
{
    bool ok = true;
    std::string error;             ///< Set when ok is false.
    Options options;
};

/// Parses the arguments after the program name.
ParseResult parseArguments(const std::vector<std::string>& args);

/// The help text, ending with a line break.
std::string usageText();

} // namespace ajaduplex::probe
