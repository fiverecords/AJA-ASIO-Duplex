// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    RegistryStore.h
 * @brief   DriverSettings kept as DWORD values under one registry key.
 */

#pragma once

#include "common/DriverSettings.h"

#include <windows.h>

#include <string>

namespace ajaduplex::asio::win
{

class RegistryStore final : public SettingsStore
{
public:
    RegistryStore(HKEY root, std::wstring subKey) : root_(root), subKey_(std::move(subKey)) {}

    bool readNumber(const char* name, std::uint32_t& value) override;
    bool writeNumber(const char* name, std::uint32_t value) override;

private:
    HKEY root_;
    std::wstring subKey_;
};

/// UTF-8 to UTF-16, for names and paths.
std::wstring widen(const std::string& text);

} // namespace ajaduplex::asio::win
