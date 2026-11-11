// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    OwnerMark.h
 * @brief   Which program has an audio system of the card in hand, kept
 *          where it outlives that program.
 *
 * AJA's driver stops nothing when a program ends. A host closed by force,
 * or one that crashed, while the driver played leaves its audio system
 * running, the card in OEM task mode and the AES outputs on that audio
 * system (KONA X, 2026-11-11: duplex-probe ended from outside in the
 * middle of a loop), and the next program to open the audio system finds
 * it in use. Restarting the PC cleared it. This is so that it takes no
 * restart.
 *
 * From the first thing a program changes on the card for an audio system
 * until it has put everything back, a mark says which program that is: its
 * process id and the time it started (an id alone comes round again), its
 * name, and what it found: the task mode, and the audio system's own
 * settings. The next one to open the audio system reads the mark. If that
 * process is gone, what it left is this driver's own: to stop, and to put
 * back as that program would have at its close. With no mark, an audio
 * system that runs is another program's and is left alone, as it always
 * was.
 *
 * On Windows the marks are the values of a volatile registry key,
 * HKCU\Software\AJA Duplex ASIO\Running: they outlive the process that
 * wrote them and are gone with a restart of the PC (and when the user logs
 * off), as is what they are about. Elsewhere there are none.
 */

#pragma once

#include <cstdint>
#include <string>

namespace ajaduplex::platform
{

struct ProcessIdentity
{
    std::uint32_t id = 0;
    /// When it started, in 100 ns since 1601 (a FILETIME); 0 where that is
    /// not known, and then the id alone tells.
    std::uint64_t started = 0;
};

/// This process.
ProcessIdentity thisProcess();

/// This process's program without its folder: "Arena.exe".
std::string thisProgramName();

enum class ProcessState
{
    Running,   ///< a process with that id that started when the identity says
    Gone,      ///< none, or one that started at another time: the id came round again
    Unknown,   ///< it cannot be asked about (no right to): to be taken as running
};

ProcessState stateOf(const ProcessIdentity& process);

/// False only when no process has that id. For a reservation of the card,
/// which names its holder by id alone.
bool processRuns(std::uint32_t id);

/// An audio system's own settings, which no other audio system shares, as
/// a program found them: what it puts back when it closes, and what the
/// next one puts back for it when it was closed by force. In the card's own
/// numbers (the SDK's), which this code keeps and does not read.
struct FoundSettings
{
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::uint32_t bufferSize = 0;
    std::uint32_t loopBack = 0;
    bool captureEnable = false;
    std::uint32_t inputDelay = 0;     ///< 0 on a card without audio delays
    std::uint32_t outputDelay = 0;
    /// The source select register, whole: the source, the embedded input
    /// and its clock, and the erase mode.
    std::uint32_t sourceSelect = 0;
};

struct OwnerRecord
{
    ProcessIdentity process;
    std::string program;         ///< for the log: "vMix64.exe"
    std::string taskModeFound;   ///< the card's task mode before the program set its own; empty if it could not be read
    /// The audio system's own settings before the program set its own. Not
    /// in a mark of 0.1.21 or 0.1.22, which says the task mode alone.
    bool haveSettings = false;
    FoundSettings settings;
};

/// A record as one line of text, and back. False for anything that is not
/// one (a later version's, or not a mark at all). What is written is
/// version 2, with the settings; version 1, without them, is still read.
std::string formatOwner(const OwnerRecord& record);
bool parseOwner(const std::string& text, OwnerRecord& record);

enum class OpenAction
{
    Proceed,    ///< nothing left behind: open as ever
    Refuse,     ///< it runs, and not as something a program of ours that is gone left: another program's
    TakeOver,   ///< it runs because a program of ours that is gone left it running: stop it, put back, open
    PutBack,    ///< it does not run, but a program of ours that is gone left its mark: put back, open
};

/// What opening an audio system does with what it finds: whether its input
/// or output runs, whether a mark is there, and what became of the
/// process the mark names.
constexpr OpenAction onOpen(bool running, bool marked, ProcessState owner) noexcept
{
    const bool leftByOurs = marked && owner == ProcessState::Gone;
    if (running)
        return leftByOurs ? OpenAction::TakeOver : OpenAction::Refuse;
    return leftByOurs ? OpenAction::PutBack : OpenAction::Proceed;
}

namespace ownermarks
{

/// The mark of `name` (a card's serial number and an audio system:
/// "00A12345-as2"). write is false where marks cannot be kept; read is
/// false when there is none, or what is there is not a mark.
bool write(const std::string& name, const OwnerRecord& record);
bool read(const std::string& name, OwnerRecord& record);
void remove(const std::string& name);

} // namespace ownermarks

} // namespace ajaduplex::platform
