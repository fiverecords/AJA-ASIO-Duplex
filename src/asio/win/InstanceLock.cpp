// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/win/InstanceLock.h"

#include <cstring>

namespace ajaduplex::asio::win
{

namespace
{

constexpr DWORD kOwnerBytes = 1024;

std::string narrow(const std::wstring& wide)
{
    if (wide.empty())
        return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr,
                            nullptr);
    return text;
}

std::wstring thisProgram()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring name(path, length);
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        name = name.substr(slash + 1);
    return name + L" (process " + std::to_wstring(GetCurrentProcessId()) + L")";
}

} // namespace

bool InstanceLock::acquire(unsigned card, unsigned audioSystem, std::string& holder)
{
    release();
    const std::wstring base = L"Local\\AJADuplexASIO-card" + std::to_wstring(card) + L"-as" +
                              std::to_wstring(audioSystem);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, base.c_str());
    if (mutex == nullptr)
        return true;   // no lock to be had: let the card's own checks decide
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        holder = "another program";
        HANDLE owner = OpenFileMappingW(FILE_MAP_READ, FALSE, (base + L"-owner").c_str());
        if (owner != nullptr)
        {
            const void* view = MapViewOfFile(owner, FILE_MAP_READ, 0, 0, kOwnerBytes);
            if (view != nullptr)
            {
                const wchar_t* text = static_cast<const wchar_t*>(view);
                std::size_t n = 0;
                while (n < kOwnerBytes / sizeof(wchar_t) - 1 && text[n] != L'\0')
                    ++n;
                if (n > 0)
                    holder = narrow(std::wstring(text, n));
                UnmapViewOfFile(view);
            }
            CloseHandle(owner);
        }
        CloseHandle(mutex);
        return false;
    }
    mutex_ = mutex;
    owner_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kOwnerBytes,
                                (base + L"-owner").c_str());
    if (owner_ != nullptr)
    {
        void* view = MapViewOfFile(owner_, FILE_MAP_WRITE, 0, 0, kOwnerBytes);
        if (view != nullptr)
        {
            const std::wstring name = thisProgram();
            const std::size_t chars = name.size() < kOwnerBytes / sizeof(wchar_t) - 1
                                          ? name.size()
                                          : kOwnerBytes / sizeof(wchar_t) - 1;
            std::memcpy(view, name.c_str(), chars * sizeof(wchar_t));
            static_cast<wchar_t*>(view)[chars] = L'\0';
            UnmapViewOfFile(view);
        }
    }
    return true;
}

void InstanceLock::release()
{
    if (owner_ != nullptr)
        CloseHandle(owner_);
    if (mutex_ != nullptr)
        CloseHandle(mutex_);
    owner_ = nullptr;
    mutex_ = nullptr;
}

std::wstring TaskModeMark::name(unsigned card)
{
    return L"Local\\AJADuplexASIO-card" + std::to_wstring(card) + L"-oem";
}

void TaskModeMark::set(unsigned card)
{
    clear();
    // Never owned or waited for: it only has to exist. A second holder (a
    // driver in another program, on another audio system of the card) gets
    // a handle to the same one, and the name lasts until the last is gone.
    mutex_ = CreateMutexW(nullptr, FALSE, name(card).c_str());
}

void TaskModeMark::clear()
{
    if (mutex_ != nullptr)
        CloseHandle(mutex_);
    mutex_ = nullptr;
}

bool TaskModeMark::present(unsigned card)
{
    HANDLE mark = OpenMutexW(SYNCHRONIZE, FALSE, name(card).c_str());
    if (mark == nullptr)
        return false;
    CloseHandle(mark);
    return true;
}

} // namespace ajaduplex::asio::win
