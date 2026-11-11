// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// The mark that says which program has an audio system in hand, and what
// opening the audio system makes of it.

#include "test_framework.h"

#include "platform/OwnerMark.h"

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <sys/wait.h>
#   include <unistd.h>
#endif

using namespace ajaduplex::platform;

TEST_CASE(OwnerMark, ARecordGoesToTextAndBack)
{
    OwnerRecord written;
    written.process.id = 29780;
    written.process.started = 134073481881090000ull;
    written.program = "Resolume Arena.exe";   // a name with a space in it
    written.taskModeFound = "standard";
    // The KONA X's audio system 2 as the card session found it: 16
    // channels, loopback on, capture enable off, the analog source and no
    // erase mode.
    written.haveSettings = true;
    written.settings.channels = 16;
    written.settings.bufferSize = 1;
    written.settings.loopBack = 1;
    written.settings.sourceSelect = 0x00000009u;
    const std::string text = formatOwner(written);
    EXPECT_TRUE(text == "v2 29780 134073481881090000 standard 16,0,1,1,0,0,0,00000009 Resolume Arena.exe");

    OwnerRecord read;
    ASSERT_TRUE(parseOwner(text, read));
    EXPECT_EQ(read.process.id, written.process.id);
    EXPECT_EQ(read.process.started, written.process.started);
    EXPECT_TRUE(read.program == written.program);
    EXPECT_TRUE(read.taskModeFound == written.taskModeFound);
    ASSERT_TRUE(read.haveSettings);
    EXPECT_EQ(read.settings.channels, std::uint32_t(16));
    EXPECT_EQ(read.settings.rate, std::uint32_t(0));
    EXPECT_EQ(read.settings.bufferSize, std::uint32_t(1));
    EXPECT_EQ(read.settings.loopBack, std::uint32_t(1));
    EXPECT_FALSE(read.settings.captureEnable);
    EXPECT_EQ(read.settings.inputDelay, std::uint32_t(0));
    EXPECT_EQ(read.settings.outputDelay, std::uint32_t(0));
    EXPECT_EQ(read.settings.sourceSelect, std::uint32_t(0x00000009u));

    // Every number of the settings, each in its place.
    OwnerRecord full;
    full.process.id = 4294967295u;
    full.haveSettings = true;
    full.settings.channels = 8;
    full.settings.rate = 2;
    full.settings.bufferSize = 0;
    full.settings.loopBack = 0;
    full.settings.captureEnable = true;
    full.settings.inputDelay = 12;
    full.settings.outputDelay = 8160;
    full.settings.sourceSelect = 0xFFF8A0B1u;
    const std::string fullText = formatOwner(full);
    EXPECT_TRUE(fullText == "v2 4294967295 0 - 8,2,0,0,1,12,8160,FFF8A0B1 ");
    OwnerRecord fullRead;
    ASSERT_TRUE(parseOwner(fullText, fullRead));
    ASSERT_TRUE(fullRead.haveSettings);
    EXPECT_EQ(fullRead.settings.channels, std::uint32_t(8));
    EXPECT_EQ(fullRead.settings.rate, std::uint32_t(2));
    EXPECT_EQ(fullRead.settings.bufferSize, std::uint32_t(0));
    EXPECT_EQ(fullRead.settings.loopBack, std::uint32_t(0));
    EXPECT_TRUE(fullRead.settings.captureEnable);
    EXPECT_EQ(fullRead.settings.inputDelay, std::uint32_t(12));
    EXPECT_EQ(fullRead.settings.outputDelay, std::uint32_t(8160));
    EXPECT_EQ(fullRead.settings.sourceSelect, std::uint32_t(0xFFF8A0B1u));

    // A task mode that could not be read, no settings and no name.
    OwnerRecord bare;
    bare.process.id = 7;
    EXPECT_TRUE(formatOwner(bare) == "v2 7 0 - - ");
    OwnerRecord back;
    back.program = "x";
    back.taskModeFound = "x";
    back.haveSettings = true;
    back.settings.channels = 16;
    ASSERT_TRUE(parseOwner(formatOwner(bare), back));
    EXPECT_EQ(back.process.id, std::uint32_t(7));
    EXPECT_EQ(back.process.started, std::uint64_t(0));
    EXPECT_TRUE(back.program.empty());
    EXPECT_TRUE(back.taskModeFound.empty());
    EXPECT_FALSE(back.haveSettings);
    EXPECT_EQ(back.settings.channels, std::uint32_t(0));
}

TEST_CASE(OwnerMark, AMarkOfAnEarlierVersionIsStillRead)
{
    // What 0.1.21 and 0.1.22 wrote: no settings. A program of those that
    // was closed by force is still this driver's to take over from, with
    // the settings left as they are.
    OwnerRecord read;
    read.haveSettings = true;
    read.settings.captureEnable = true;
    ASSERT_TRUE(parseOwner("v1 29780 134073481881090000 standard Resolume Arena.exe", read));
    EXPECT_EQ(read.process.id, std::uint32_t(29780));
    EXPECT_EQ(read.process.started, std::uint64_t(134073481881090000ull));
    EXPECT_TRUE(read.taskModeFound == "standard");
    EXPECT_TRUE(read.program == "Resolume Arena.exe");
    EXPECT_FALSE(read.haveSettings);
    EXPECT_FALSE(read.settings.captureEnable);
    // A name that looks like settings is a name there.
    ASSERT_TRUE(parseOwner("v1 7 0 - 16,0,1,1,0,0,0,00000009 x.exe", read));
    EXPECT_FALSE(read.haveSettings);
    EXPECT_TRUE(read.program == "16,0,1,1,0,0,0,00000009 x.exe");
}

TEST_CASE(OwnerMark, WhatIsNotAMarkIsNotReadAsOne)
{
    OwnerRecord record;
    record.process.id = 5;
    record.program = "kept.exe";
    EXPECT_FALSE(parseOwner("", record));
    EXPECT_FALSE(parseOwner("v3 29780 1 standard - vMix64.exe", record));   // a later version's
    EXPECT_FALSE(parseOwner("29780 1 standard vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v1 0 1 standard vMix64.exe", record));       // no process has id 0
    EXPECT_FALSE(parseOwner("v1 -3 1 standard vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v1 4294967296 1 standard vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v1 29780 soon standard vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v1 29780 1", record));
    EXPECT_FALSE(parseOwner("v1 29780", record));
    EXPECT_FALSE(parseOwner("v2 0 1 standard - vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v2 29780 1 standard", record));
    // Settings that are not eight numbers as they are written.
    EXPECT_FALSE(parseOwner("v2 29780 1 standard vMix64.exe", record));                        // version 1's words
    EXPECT_FALSE(parseOwner("v2 29780 1 standard  vMix64.exe", record));                       // none at all
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,0,1,1,0,0,0 vMix64.exe", record));         // one short
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,0,1,1,0,0,0,00000009,1 vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,0,1,1,2,0,0,00000009 vMix64.exe", record));   // capture enable is 0 or 1
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,0,1,1,0,0,0,9 vMix64.exe", record));       // eight digits
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,0,1,1,0,0,0,0000000g vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,0,1,1,0,0,0,0008000a vMix64.exe", record));   // as written: capitals
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,,1,1,0,0,0,00000009 vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 16,-1,1,1,0,0,0,00000009 vMix64.exe", record));
    EXPECT_FALSE(parseOwner("v2 29780 1 standard 4294967296,0,1,1,0,0,0,00000009 vMix64.exe", record));
    // And what was there is as it was.
    EXPECT_EQ(record.process.id, std::uint32_t(5));
    EXPECT_TRUE(record.program == "kept.exe");
}

TEST_CASE(OwnerMark, OnlyWhatAProgramOfOursThatIsGoneLeftIsTaken)
{
    // It runs. With the mark of a process that is gone it is ours to take
    // over; anything else is another program's, as before there were marks.
    EXPECT_TRUE(onOpen(true, true, ProcessState::Gone) == OpenAction::TakeOver);
    EXPECT_TRUE(onOpen(true, true, ProcessState::Running) == OpenAction::Refuse);
    EXPECT_TRUE(onOpen(true, true, ProcessState::Unknown) == OpenAction::Refuse);
    EXPECT_TRUE(onOpen(true, false, ProcessState::Gone) == OpenAction::Refuse);
    EXPECT_TRUE(onOpen(true, false, ProcessState::Unknown) == OpenAction::Refuse);
    // It does not run. A mark of a process that is gone still says the task
    // mode is to go back; otherwise there is nothing to do.
    EXPECT_TRUE(onOpen(false, true, ProcessState::Gone) == OpenAction::PutBack);
    EXPECT_TRUE(onOpen(false, true, ProcessState::Running) == OpenAction::Proceed);
    EXPECT_TRUE(onOpen(false, true, ProcessState::Unknown) == OpenAction::Proceed);
    EXPECT_TRUE(onOpen(false, false, ProcessState::Unknown) == OpenAction::Proceed);
}

TEST_CASE(OwnerMark, ThisProcessRuns)
{
    const ProcessIdentity me = thisProcess();
    EXPECT_TRUE(me.id != 0);
    EXPECT_TRUE(stateOf(me) == ProcessState::Running);
    EXPECT_TRUE(processRuns(me.id));
    EXPECT_FALSE(processRuns(0));
    ProcessIdentity nobody;
    EXPECT_TRUE(stateOf(nobody) == ProcessState::Gone);
}

#if defined(_WIN32)

TEST_CASE(OwnerMark, AnIdThatCameRoundAgainIsAnotherProcess)
{
    // The same id, started at another time: not the process the mark names.
    ProcessIdentity other = thisProcess();
    ASSERT_TRUE(other.started != 0);
    other.started -= 10000000;   // a second earlier
    EXPECT_TRUE(stateOf(other) == ProcessState::Gone);
    // By id alone, as a reservation of the card names its holder, it runs.
    EXPECT_TRUE(processRuns(other.id));
    // The program's name, without its folder.
    const std::string program = thisProgramName();
    EXPECT_CONTAINS(program, ".exe");
    EXPECT_TRUE(program.find('\\') == std::string::npos && program.find('/') == std::string::npos);
}

TEST_CASE(OwnerMark, AProcessThatEndedIsGone)
{
    wchar_t command[] = L"cmd.exe /c exit 0";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    ASSERT_TRUE(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup, &child) != FALSE);
    ProcessIdentity identity;
    identity.id = child.dwProcessId;
    FILETIME created{}, exited{}, kernel{}, user{};
    ASSERT_TRUE(GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user) != FALSE);
    identity.started = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    ASSERT_TRUE(WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0);
    // Ended, and still to be opened while this handle to it is held: gone.
    EXPECT_TRUE(stateOf(identity) == ProcessState::Gone);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    // And once nothing holds it.
    EXPECT_TRUE(stateOf(identity) == ProcessState::Gone);
}

TEST_CASE(OwnerMark, AMarkIsKeptUntilItIsRemoved)
{
    const std::string name = "duplex-tests-as2";
    ownermarks::remove(name);
    OwnerRecord read;
    EXPECT_FALSE(ownermarks::read(name, read));

    OwnerRecord mine;
    mine.process = thisProcess();
    mine.program = thisProgramName();
    mine.taskModeFound = "standard";
    mine.haveSettings = true;
    mine.settings.channels = 16;
    mine.settings.loopBack = 1;
    mine.settings.sourceSelect = 0x00080009u;
    ASSERT_TRUE(ownermarks::write(name, mine));
    ASSERT_TRUE(ownermarks::read(name, read));
    EXPECT_EQ(read.process.id, mine.process.id);
    EXPECT_EQ(read.process.started, mine.process.started);
    EXPECT_TRUE(read.program == mine.program);
    EXPECT_TRUE(read.taskModeFound == "standard");
    EXPECT_TRUE(stateOf(read.process) == ProcessState::Running);
    ASSERT_TRUE(read.haveSettings);
    EXPECT_EQ(read.settings.channels, std::uint32_t(16));
    EXPECT_EQ(read.settings.loopBack, std::uint32_t(1));
    EXPECT_EQ(read.settings.sourceSelect, std::uint32_t(0x00080009u));

    // Another audio system's, another card's: marks of their own.
    EXPECT_FALSE(ownermarks::read("duplex-tests-as1", read));

    // Written again, the newer one stands.
    mine.taskModeFound = "oem";
    ASSERT_TRUE(ownermarks::write(name, mine));
    ASSERT_TRUE(ownermarks::read(name, read));
    EXPECT_TRUE(read.taskModeFound == "oem");

    ownermarks::remove(name);
    EXPECT_FALSE(ownermarks::read(name, read));
    ownermarks::remove(name);   // removing what is not there is nothing
}

TEST_CASE(OwnerMark, TheMarksKeyIsOneARestartForgets)
{
    // The key the marks are values of is volatile: the system keeps it in
    // memory only. A key that is kept cannot be made under one, which is
    // how to tell from here.
    const std::string name = "duplex-tests-volatile";
    OwnerRecord mine;
    mine.process = thisProcess();
    ASSERT_TRUE(ownermarks::write(name, mine));
    HKEY marks = nullptr;
    ASSERT_TRUE(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AJA Duplex ASIO\\Running", 0, KEY_CREATE_SUB_KEY,
                              &marks) == ERROR_SUCCESS);
    HKEY kept = nullptr;
    const LSTATUS made = RegCreateKeyExW(marks, L"kept", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ, nullptr,
                                         &kept, nullptr);
    EXPECT_TRUE(made == ERROR_CHILD_MUST_BE_VOLATILE);
    if (made == ERROR_SUCCESS)
    {
        RegCloseKey(kept);
        RegDeleteKeyW(marks, L"kept");
    }
    RegCloseKey(marks);
    ownermarks::remove(name);
}

#else

TEST_CASE(OwnerMark, AProcessThatEndedIsGone)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
        _exit(0);
    int status = 0;
    ASSERT_TRUE(waitpid(child, &status, 0) == child);
    ProcessIdentity identity;
    identity.id = static_cast<std::uint32_t>(child);
    EXPECT_TRUE(stateOf(identity) == ProcessState::Gone);
    EXPECT_FALSE(processRuns(identity.id));
}

TEST_CASE(OwnerMark, NoMarksOffWindows)
{
    // Nowhere to keep one that a restart forgets: none are kept, and an
    // audio system left running is refused as another program's.
    OwnerRecord mine;
    mine.process = thisProcess();
    EXPECT_FALSE(ownermarks::write("duplex-tests-as2", mine));
    OwnerRecord read;
    EXPECT_FALSE(ownermarks::read("duplex-tests-as2", read));
    ownermarks::remove("duplex-tests-as2");
}

#endif
