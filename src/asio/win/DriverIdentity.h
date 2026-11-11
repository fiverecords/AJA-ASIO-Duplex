// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    DriverIdentity.h
 * @brief   The COM class the DLL registers, its name, and the class it
 *          keeps for its own test.
 *
 * One driver in the hosts' lists: "AJA Duplex ASIO", on the card. The name
 * is also its key under HKLM\SOFTWARE\ASIO, where hosts find ASIO drivers;
 * AJA's own ASIO driver registers under another name. Hosts show at most
 * 31 characters of a driver's name.
 *
 * The DLL has a second class, which runs the same driver on the simulated
 * card (its AES and XLR outputs looped to its inputs). Nothing registers
 * it, so no host can list it: only a program that loads the DLL itself and
 * asks for that class gets it, which is how duplex-asiotest tries the
 * whole driver, as a host would, on a PC without the card (the build does
 * so after every build).
 *
 * Up to 0.1.19 that class was registered too, as a second driver for
 * trying the driver in a host without the card: "AJA Duplex Simulator",
 * and up to 0.1.16 "AJA Duplex ASIO (simulated)", with which vMix left the
 * card's driver out of its audio inputs (2026-11-10). With every test left
 * needing the card, it was taken out (0.1.20, 2026-11-11). Registering and
 * unregistering take away what those versions left.
 */

#pragma once

#include <windows.h>

namespace ajaduplex::asio::win
{

// {1423D042-ACD6-4FF8-AD72-A1A7AD036043}
inline constexpr CLSID kDriverClsid = {0x1423d042, 0xacd6, 0x4ff8, {0xad, 0x72, 0xa1, 0xa7, 0xad, 0x03, 0x60, 0x43}};

// {88651C34-A298-43A1-8DEC-33A159560E7F}
inline constexpr CLSID kSimulatedClsid = {0x88651c34, 0xa298, 0x43a1, {0x8d, 0xec, 0x33, 0xa1, 0x59, 0x56, 0x0e, 0x7f}};

inline constexpr const wchar_t* kDriverClsidText = L"{1423D042-ACD6-4FF8-AD72-A1A7AD036043}";
inline constexpr const wchar_t* kSimulatedClsidText = L"{88651C34-A298-43A1-8DEC-33A159560E7F}";

inline constexpr const char* kDriverName = "AJA Duplex ASIO";
inline constexpr const wchar_t* kDriverNameW = L"AJA Duplex ASIO";
/// What the class on the simulated card calls itself (to duplex-asiotest
/// and in its log: no host sees it).
inline constexpr const char* kSimulatedName = "AJA Duplex Simulator";
/// The names the simulated card was registered under as a driver: 0.1.17
/// to 0.1.19, and up to 0.1.16. Their keys go when the DLL registers or
/// unregisters, if they are still ours.
inline constexpr const wchar_t* kFormerSimulatedNamesW[] = {L"AJA Duplex Simulator", L"AJA Duplex ASIO (simulated)"};

/// HKCU key with the settings (DriverSettings); the class on the simulated
/// card reads them too.
inline constexpr const wchar_t* kSettingsKey = L"Software\\AJA Duplex ASIO";

} // namespace ajaduplex::asio::win
