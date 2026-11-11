// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// What duplex-asiotest --installed says of the driver hosts load.

#include "test_framework.h"

#include "asio/InstallState.h"

using namespace ajaduplex::asio;

namespace
{

const char* const kName = "AJA Duplex ASIO";
const char* const kInstalled = "C:\\Program Files\\AJA Duplex ASIO\\AJADuplexASIO.dll";

/// The driver installed and listed, with the DLL of this build beside the
/// program: each test changes what it is about.
InstallFacts installedFacts()
{
    InstallFacts facts;
    facts.listed = true;
    facts.server = kInstalled;
    facts.serverThere = true;
    facts.serverWritten = "2026-11-11 10:52";
    facts.besideThere = true;
    facts.sameAsBeside = true;
    return facts;
}

} // namespace

TEST_CASE(InstallState, TheInstalledCopyOfThisBuildIsFine)
{
    const InstallVerdict verdict = judgeInstall(installedFacts(), kName);
    EXPECT_TRUE(verdict.kind == InstallKind::ThisBuild);
    EXPECT_TRUE(verdict.ok);
    EXPECT_TRUE(verdict.line == std::string("installed   ") + kInstalled + " (this build)");
}

TEST_CASE(InstallState, NothingRegisteredIsNotInstalled)
{
    InstallFacts facts;
    facts.besideThere = true;
    const InstallVerdict verdict = judgeInstall(facts, kName);
    EXPECT_TRUE(verdict.kind == InstallKind::NotInstalled);
    EXPECT_FALSE(verdict.ok);
    EXPECT_CONTAINS(verdict.line, "installed   no: hosts do not list AJA Duplex ASIO");
    EXPECT_CONTAINS(verdict.line, "install-driver.cmd");
}

// What a folder deleted after installing left, up to 0.1.21: a name in the
// hosts' lists and nothing to load (Resolume showed the driver greyed out,
// vMix left it out, 2026-11-11).
TEST_CASE(InstallState, ARegisteredFileThatIsGoneIsSaid)
{
    InstallFacts facts = installedFacts();
    facts.server = "C:\\dev\\dist\\AJA-Duplex-ASIO-0.1.20-win64\\AJADuplexASIO.dll";
    facts.serverThere = false;
    facts.sameAsBeside = false;
    const InstallVerdict verdict = judgeInstall(facts, kName);
    EXPECT_TRUE(verdict.kind == InstallKind::FileMissing);
    EXPECT_FALSE(verdict.ok);
    EXPECT_CONTAINS(verdict.line, "registered at C:\\dev\\dist\\AJA-Duplex-ASIO-0.1.20-win64\\AJADuplexASIO.dll");
    EXPECT_CONTAINS(verdict.line, "that file is not there");
    EXPECT_CONTAINS(verdict.line, "hosts list the driver and cannot open it");

    // Not in the hosts' list either: they do not list what they cannot open.
    facts.listed = false;
    const InstallVerdict unlisted = judgeInstall(facts, kName);
    EXPECT_TRUE(unlisted.kind == InstallKind::FileMissing);
    EXPECT_CONTAINS(unlisted.line, "hosts cannot open it");
}

TEST_CASE(InstallState, AnotherBuildThanTheOneBesideIsSaid)
{
    InstallFacts facts = installedFacts();
    facts.sameAsBeside = false;
    const InstallVerdict verdict = judgeInstall(facts, kName);
    EXPECT_TRUE(verdict.kind == InstallKind::OtherBuild);
    EXPECT_FALSE(verdict.ok);
    EXPECT_CONTAINS(verdict.line, std::string(kInstalled) + ", ANOTHER BUILD than the DLL beside this program");
    EXPECT_CONTAINS(verdict.line, "(written 2026-11-11 10:52)");
    EXPECT_CONTAINS(verdict.line, "until install-driver.cmd runs again");

    // Its date not known: said without it.
    facts.serverWritten.clear();
    const InstallVerdict undated = judgeInstall(facts, kName);
    EXPECT_TRUE(undated.kind == InstallKind::OtherBuild);
    EXPECT_TRUE(undated.line.find("written") == std::string::npos);
}

TEST_CASE(InstallState, WithNoDllBesideTheProgramThereIsNothingToCompare)
{
    InstallFacts facts = installedFacts();
    facts.besideThere = false;
    facts.sameAsBeside = false;
    const InstallVerdict verdict = judgeInstall(facts, kName);
    EXPECT_TRUE(verdict.kind == InstallKind::NothingToCompare);
    EXPECT_TRUE(verdict.ok);
    EXPECT_CONTAINS(verdict.line, "no DLL beside this program");
}

TEST_CASE(InstallState, HalfARegistrationIsNotInstalled)
{
    // The hosts' key without the class: listed, and nothing to create.
    InstallFacts listedOnly;
    listedOnly.listed = true;
    const InstallVerdict noClass = judgeInstall(listedOnly, kName);
    EXPECT_TRUE(noClass.kind == InstallKind::ClassMissing);
    EXPECT_FALSE(noClass.ok);
    EXPECT_CONTAINS(noClass.line, "its class is not registered");

    // The class without the hosts' key: nothing lists it.
    InstallFacts classOnly = installedFacts();
    classOnly.listed = false;
    const InstallVerdict notListed = judgeInstall(classOnly, kName);
    EXPECT_TRUE(notListed.kind == InstallKind::NotListed);
    EXPECT_FALSE(notListed.ok);
    EXPECT_CONTAINS(notListed.line, "is not in the hosts' list");
}

TEST_CASE(InstallState, EveryVerdictIsOneLineThatStartsAlike)
{
    InstallFacts gone = installedFacts();
    gone.serverThere = false;
    InstallFacts other = installedFacts();
    other.sameAsBeside = false;
    InstallFacts listedOnly;
    listedOnly.listed = true;
    const InstallFacts all[] = {InstallFacts{}, installedFacts(), gone, other, listedOnly};
    for (const InstallFacts& facts : all)
    {
        const InstallVerdict verdict = judgeInstall(facts, kName);
        EXPECT_TRUE(verdict.line.rfind("installed   ", 0) == 0);
        EXPECT_TRUE(verdict.line.find('\n') == std::string::npos);
        // card-session and build.ps1 go by the first word after it.
        const bool saysFine = verdict.line.rfind("installed   no:", 0) != 0 &&
                              verdict.line.find("ANOTHER BUILD") == std::string::npos;
        EXPECT_TRUE(verdict.ok == saysFine);
    }
}
