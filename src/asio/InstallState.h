// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    InstallState.h
 * @brief   What hosts find of the driver on this PC, said in one line.
 *
 * Since 0.1.22 install-driver.cmd copies the DLL to Program Files and
 * registers that copy, so there are two DLLs on a PC that builds: the one
 * hosts load and the one beside the tools. Up to 0.1.21 the DLL was
 * registered where it was, and a folder deleted or made again left hosts
 * with a name in their list and nothing to load (2026-11-11: Resolume
 * showed the driver greyed out and vMix left it out). Either way the
 * question is the same: is the driver hosts load there, and is it this
 * build?
 *
 * The facts come from the registry and the file system (duplex-asiotest
 * --installed reads them); the verdict is made here, so that it can be
 * tested anywhere.
 */

#pragma once

#include <string>

namespace ajaduplex::asio
{

struct InstallFacts
{
    bool listed = false;         ///< HKLM\SOFTWARE\ASIO\<name> is there and names the driver's class
    std::string server;          ///< the DLL the class is registered with ("": the class is not registered)
    bool serverThere = false;    ///< that file exists
    std::string serverWritten;   ///< when it was last written, as text ("": not known)
    bool besideThere = false;    ///< a DLL is beside the program, to compare it with
    bool sameAsBeside = false;   ///< and the two are the same, byte for byte
};

enum class InstallKind
{
    NotInstalled,       ///< neither listed nor registered
    ClassMissing,       ///< listed, but the class is not registered: hosts cannot open it
    FileMissing,        ///< registered with a file that is not there: hosts cannot open it
    NotListed,          ///< registered and there, but not in the hosts' list
    OtherBuild,         ///< hosts load another build than the DLL beside the program
    ThisBuild,          ///< hosts load this very build
    NothingToCompare,   ///< hosts load it; no DLL beside the program to say which build
};

struct InstallVerdict
{
    InstallKind kind = InstallKind::NotInstalled;
    bool ok = false;     ///< hosts can open the driver, and it is not known to be another build
    std::string line;    ///< "installed   ...", one line
};

inline InstallVerdict judgeInstall(const InstallFacts& facts, const std::string& name)
{
    const std::string head = "installed   ";
    const std::string fix = "run install-driver.cmd (as administrator)";
    InstallVerdict verdict;
    if (facts.server.empty())
    {
        if (!facts.listed)
        {
            verdict.kind = InstallKind::NotInstalled;
            verdict.line = head + "no: hosts do not list " + name + "; to install it, " + fix;
        }
        else
        {
            verdict.kind = InstallKind::ClassMissing;
            verdict.line = head + "no: hosts list " + name + ", but its class is not registered, so they cannot "
                                  "open it; " + fix;
        }
        return verdict;
    }
    if (!facts.serverThere)
    {
        verdict.kind = InstallKind::FileMissing;
        verdict.line = head + "no: registered at " + facts.server + ", and that file is not there: hosts " +
                       (facts.listed ? "list the driver and " : "") + "cannot open it; " + fix;
        return verdict;
    }
    if (!facts.listed)
    {
        verdict.kind = InstallKind::NotListed;
        verdict.line = head + "no: " + facts.server + " is registered, but " + name +
                       " is not in the hosts' list (HKLM\\SOFTWARE\\ASIO); " + fix;
        return verdict;
    }
    if (!facts.besideThere)
    {
        verdict.kind = InstallKind::NothingToCompare;
        verdict.ok = true;
        verdict.line = head + facts.server + " (no DLL beside this program to compare it with)";
        return verdict;
    }
    if (facts.sameAsBeside)
    {
        verdict.kind = InstallKind::ThisBuild;
        verdict.ok = true;
        verdict.line = head + facts.server + " (this build)";
        return verdict;
    }
    verdict.kind = InstallKind::OtherBuild;
    verdict.line = head + facts.server + ", ANOTHER BUILD than the DLL beside this program" +
                   (facts.serverWritten.empty() ? std::string() : " (written " + facts.serverWritten + ")") +
                   ": hosts load that one until install-driver.cmd runs again";
    return verdict;
}

} // namespace ajaduplex::asio
