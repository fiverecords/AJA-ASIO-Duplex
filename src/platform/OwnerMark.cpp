// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "platform/OwnerMark.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <csignal>
#   include <unistd.h>
#endif

namespace ajaduplex::platform
{

namespace
{

/// The first word of a mark. Version 2 is what is written: version 1's
/// words, and after the task mode one more with the audio system's own
/// settings as they were found. Version 1 (0.1.21 and 0.1.22) is still
/// read: a program of those that was closed by force may have left one.
/// What starts with neither is not read as a mark.
constexpr const char* kMarkVersion = "v2";
constexpr const char* kMarkVersionOne = "v1";

/// The next word of `text` from `at`, which moves past it and the space
/// after it. Empty at the end.
std::string nextWord(const std::string& text, std::size_t& at)
{
    const std::size_t end = text.find(' ', at);
    const std::string word = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    at = end == std::string::npos ? text.size() : end + 1;
    return word;
}

bool parseNumber(const std::string& word, std::uint64_t& value)
{
    if (word.empty() || word.size() > 20 || word.find_first_not_of("0123456789") != std::string::npos)
        return false;
    errno = 0;
    char* end = nullptr;
    value = std::strtoull(word.c_str(), &end, 10);
    return errno == 0 && end != nullptr && *end == '\0';
}

bool parseHex32(const std::string& word, std::uint32_t& value)
{
    if (word.size() != 8 || word.find_first_not_of("0123456789ABCDEF") != std::string::npos)
        return false;
    value = static_cast<std::uint32_t>(std::strtoul(word.c_str(), nullptr, 16));
    return true;
}

/// The settings as one word: seven numbers and the source select register
/// in hexadecimal, with commas between them. "-" when there are none.
std::string formatSettings(const OwnerRecord& record)
{
    if (!record.haveSettings)
        return "-";
    const FoundSettings& s = record.settings;
    char sourceSelect[16] = {};
    std::snprintf(sourceSelect, sizeof(sourceSelect), "%08lX", static_cast<unsigned long>(s.sourceSelect));
    return std::to_string(s.channels) + "," + std::to_string(s.rate) + "," + std::to_string(s.bufferSize) + "," +
           std::to_string(s.loopBack) + "," + (s.captureEnable ? "1" : "0") + "," + std::to_string(s.inputDelay) +
           "," + std::to_string(s.outputDelay) + "," + sourceSelect;
}

bool parseSettings(const std::string& word, OwnerRecord& record)
{
    record.haveSettings = false;
    record.settings = FoundSettings();
    if (word == "-")
        return true;
    constexpr std::size_t kNumbers = 7;
    std::uint64_t numbers[kNumbers] = {};
    std::size_t at = 0;
    for (std::size_t n = 0; n < kNumbers; ++n)
    {
        const std::size_t comma = word.find(',', at);
        if (comma == std::string::npos || !parseNumber(word.substr(at, comma - at), numbers[n]) ||
            numbers[n] > 0xFFFFFFFFull)
            return false;
        at = comma + 1;
    }
    FoundSettings s;
    if (numbers[4] > 1 || !parseHex32(word.substr(at), s.sourceSelect))
        return false;
    s.channels = static_cast<std::uint32_t>(numbers[0]);
    s.rate = static_cast<std::uint32_t>(numbers[1]);
    s.bufferSize = static_cast<std::uint32_t>(numbers[2]);
    s.loopBack = static_cast<std::uint32_t>(numbers[3]);
    s.captureEnable = numbers[4] == 1;
    s.inputDelay = static_cast<std::uint32_t>(numbers[5]);
    s.outputDelay = static_cast<std::uint32_t>(numbers[6]);
    record.settings = s;
    record.haveSettings = true;
    return true;
}

#if defined(_WIN32)

constexpr const wchar_t* kSettingsKey = L"Software\\AJA Duplex ASIO";
constexpr const wchar_t* kMarksKey = L"Running";
constexpr const wchar_t* kMarksPath = L"Software\\AJA Duplex ASIO\\Running";

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

std::string narrow(const std::wstring& wide)
{
    if (wide.empty())
        return std::string();
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr,
                            nullptr);
    return text;
}

std::uint64_t toNumber(const FILETIME& time)
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

#endif

} // namespace

std::string formatOwner(const OwnerRecord& record)
{
    // The program last: its name may hold spaces.
    return std::string(kMarkVersion) + " " + std::to_string(record.process.id) + " " +
           std::to_string(record.process.started) + " " +
           (record.taskModeFound.empty() ? std::string("-") : record.taskModeFound) + " " +
           formatSettings(record) + " " + record.program;
}

bool parseOwner(const std::string& text, OwnerRecord& record)
{
    std::size_t at = 0;
    const std::string version = nextWord(text, at);
    const bool withSettings = version == kMarkVersion;
    if (!withSettings && version != kMarkVersionOne)
        return false;
    std::uint64_t id = 0, started = 0;
    if (!parseNumber(nextWord(text, at), id) || id == 0 || id > 0xFFFFFFFFull ||
        !parseNumber(nextWord(text, at), started))
        return false;
    const std::string mode = nextWord(text, at);
    if (mode.empty())
        return false;
    OwnerRecord read;
    if (withSettings && !parseSettings(nextWord(text, at), read))
        return false;
    read.process.id = static_cast<std::uint32_t>(id);
    read.process.started = started;
    read.taskModeFound = mode == "-" ? std::string() : mode;
    read.program = text.substr(at);
    record = read;
    return true;
}

bool processRuns(std::uint32_t id)
{
    ProcessIdentity process;
    process.id = id;
    return stateOf(process) != ProcessState::Gone;
}

#if defined(_WIN32)

ProcessIdentity thisProcess()
{
    ProcessIdentity process;
    process.id = GetCurrentProcessId();
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        process.started = toNumber(created);
    return process;
}

std::string thisProgramName()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring name(path, length);
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        name.erase(0, slash + 1);
    return narrow(name);
}

ProcessState stateOf(const ProcessIdentity& process)
{
    if (process.id == 0)
        return ProcessState::Gone;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.id);
    if (handle == nullptr)
    {
        // No such process, or one that may not be asked about (another
        // user's, a protected one): the first is gone, the second is not
        // ours to call gone.
        return GetLastError() == ERROR_INVALID_PARAMETER ? ProcessState::Gone : ProcessState::Unknown;
    }
    ProcessState state = ProcessState::Unknown;
    DWORD code = 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetExitCodeProcess(handle, &code) && code != STILL_ACTIVE)
        state = ProcessState::Gone;   // ended, and still there while something holds a handle to it
    else if (GetProcessTimes(handle, &created, &exited, &kernel, &user))
        state = process.started != 0 && toNumber(created) != process.started ? ProcessState::Gone
                                                                              : ProcessState::Running;
    CloseHandle(handle);
    return state;
}

namespace ownermarks
{

bool write(const std::string& name, const OwnerRecord& record)
{
    // The settings' key as it always is, and under it a volatile one: a
    // volatile key is the system's to forget at a restart, and all of what
    // is under it with it.
    HKEY settings = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_CREATE_SUB_KEY,
                        nullptr, &settings, nullptr) != ERROR_SUCCESS)
        return false;
    HKEY marks = nullptr;
    const LSTATUS made = RegCreateKeyExW(settings, kMarksKey, 0, nullptr, REG_OPTION_VOLATILE, KEY_SET_VALUE,
                                         nullptr, &marks, nullptr);
    RegCloseKey(settings);
    if (made != ERROR_SUCCESS)
        return false;
    const std::wstring text = widen(formatOwner(record));
    const LSTATUS set = RegSetValueExW(marks, widen(name).c_str(), 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(text.c_str()),
                                       static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(marks);
    return set == ERROR_SUCCESS;
}

bool read(const std::string& name, OwnerRecord& record)
{
    wchar_t text[512] = {};
    DWORD bytes = sizeof(text) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kMarksPath, widen(name).c_str(), RRF_RT_REG_SZ, nullptr, text, &bytes) !=
        ERROR_SUCCESS)
        return false;
    return parseOwner(narrow(text), record);
}

void remove(const std::string& name)
{
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kMarksPath, widen(name).c_str());
}

} // namespace ownermarks

#else

ProcessIdentity thisProcess()
{
    ProcessIdentity process;
    process.id = static_cast<std::uint32_t>(getpid());
    return process;
}

std::string thisProgramName()
{
    return std::string();
}

ProcessState stateOf(const ProcessIdentity& process)
{
    if (process.id == 0)
        return ProcessState::Gone;
    if (kill(static_cast<pid_t>(process.id), 0) == 0)
        return ProcessState::Running;
    return errno == ESRCH ? ProcessState::Gone : ProcessState::Unknown;
}

namespace ownermarks
{

// Nowhere to keep one that a restart forgets: no marks, and an audio system
// left running is refused as another program's.
bool write(const std::string&, const OwnerRecord&)
{
    return false;
}

bool read(const std::string&, OwnerRecord&)
{
    return false;
}

void remove(const std::string&) {}

} // namespace ownermarks

#endif

} // namespace ajaduplex::platform
