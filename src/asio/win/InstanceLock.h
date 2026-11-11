// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    InstanceLock.h
 * @brief   One program at a time on a card's audio system.
 *
 * A named mutex per card and audio system: whoever creates it holds the
 * audio system for as long as it keeps the handle, and the next driver
 * instance, in this program or another, finds it there and refuses to
 * initialize, naming who has it (kept beside the mutex in a small named
 * shared-memory block). Nothing is left behind if a program dies: the name
 * goes with its last handle.
 */

#pragma once

#include <windows.h>

#include <string>

namespace ajaduplex::asio::win
{

class InstanceLock
{
public:
    InstanceLock() = default;
    ~InstanceLock() { release(); }
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    /// True if this instance now holds (card, audioSystem); false with
    /// `holder` naming the program that does.
    bool acquire(unsigned card, unsigned audioSystem, std::string& holder);
    void release();

private:
    HANDLE mutex_ = nullptr;
    HANDLE owner_ = nullptr;
};

/**
 * A mark other programs can see: while the driver answers for a card's
 * task mode (it put the card in OEM task mode and will put the mode back
 * when the host lets it go), the named mutex
 *
 *     Local\AJADuplexASIO-card<card>-oem
 *
 * exists, <card> being the card's index in the SDK's device scan. The
 * driver does not reserve the card (AcquireStreamForApplication), so this
 * is how another program that changes the task mode of the same card knows
 * that the driver is there: while the mark is set it leaves the mode
 * alone, and an OEM mode it found with the mark set is the driver's, not
 * the card's own. AJA Input (github.com/fiverecords/AJA-Input-FFGL,
 * src/DuplexAsioPresence.h) asks with OpenMutexW(SYNCHRONIZE, FALSE, name).
 * The name is part of that agreement: it stays as it is.
 *
 * Nothing is left behind if the program dies: the name goes with its last
 * handle.
 */
class TaskModeMark
{
public:
    TaskModeMark() = default;
    ~TaskModeMark() { clear(); }
    TaskModeMark(const TaskModeMark&) = delete;
    TaskModeMark& operator=(const TaskModeMark&) = delete;

    void set(unsigned card);
    void clear();
    bool isSet() const { return mutex_ != nullptr; }

    static std::wstring name(unsigned card);
    /// Whether some program, this one included, has the mark set for `card`.
    static bool present(unsigned card);

private:
    HANDLE mutex_ = nullptr;
};

} // namespace ajaduplex::asio::win
