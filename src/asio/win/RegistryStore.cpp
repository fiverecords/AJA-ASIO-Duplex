// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/win/RegistryStore.h"

namespace ajaduplex::asio::win
{

std::wstring widen(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

bool RegistryStore::readNumber(const char* name, std::uint32_t& value)
{
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LSTATUS status =
        RegGetValueW(root_, subKey_.c_str(), widen(name).c_str(), RRF_RT_REG_DWORD, nullptr, &data, &size);
    if (status != ERROR_SUCCESS)
        return false;
    value = static_cast<std::uint32_t>(data);
    return true;
}

bool RegistryStore::writeNumber(const char* name, std::uint32_t value)
{
    const DWORD data = static_cast<DWORD>(value);
    return RegSetKeyValueW(root_, subKey_.c_str(), widen(name).c_str(), REG_DWORD, &data, sizeof(data)) ==
           ERROR_SUCCESS;
}

} // namespace ajaduplex::asio::win
