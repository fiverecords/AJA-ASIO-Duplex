// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    main.cpp
 * @brief   AJADuplexASIOSettings.exe: the driver's settings, for programs
 *          with no ASIO settings button of their own (vMix has none).
 *
 * It opens the driver that hosts load, the copy install-driver.cmd
 * registered, the way a host does (CoCreateInstance), and asks it for its
 * settings dialog without opening the card. The dialog is the driver's
 * own, the one a host's button opens; what is saved there is kept for this
 * Windows user, and every program takes it the next time it opens the
 * driver. The dialog's "Now" box says which program has the driver open,
 * if one has: that one takes the settings once it is closed and opened
 * again.
 *
 *     AJADuplexASIOSettings                    the settings
 *     AJADuplexASIOSettings --add-shortcut     "AJA Duplex ASIO Settings" in every user's Start menu
 *     AJADuplexASIOSettings --remove-shortcut  and away again
 *
 * install-driver.cmd copies it beside the driver in Program Files and adds
 * the Start menu's entry; uninstall-driver.cmd takes both away. Both
 * options end with exit code 0 when done and 1 when not, and show nothing.
 */

#include "asio/AsioSdk.h"
#include "asio/win/DriverIdentity.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include "iasiodrv.h"

#include <cwchar>
#include <string>

namespace
{

using ajaduplex::asio::win::kDriverClsid;

constexpr const wchar_t* kTitle = L"AJA Duplex ASIO Settings";
/// The dialog's caption, as SettingsDialog.cpp makes it: how a second start
/// finds the first one's window.
constexpr const wchar_t* kDialogCaption = L"AJA Duplex ASIO settings";
constexpr const wchar_t* kShortcutFile = L"AJA Duplex ASIO Settings.lnk";
constexpr const wchar_t* kSingleInstance = L"Local\\AJADuplexASIO-settings";

/// The first one's hold on kSingleInstance, kept to the end.
HANDLE gSingleInstance = nullptr;

void say(const std::wstring& text)
{
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

/// Sharp on a screen scaled above 100 %: dialogs made from a template, as
/// the driver's is, scale with the monitor they are on from Windows 10
/// 1703 on (per-monitor v2); before that, with the system's scale. Asked
/// for by name, as the first is not in every SDK's headers.
void beDpiAware()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
        return;
    using SetContext = BOOL(WINAPI*)(HANDLE);
    using SetAware = BOOL(WINAPI*)();
    // The cast through void (*)() is the one that says "any function".
    const auto setContext = reinterpret_cast<SetContext>(
        reinterpret_cast<void (*)()>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
    if (setContext != nullptr && setContext(perMonitorV2))
        return;
    const auto setAware =
        reinterpret_cast<SetAware>(reinterpret_cast<void (*)()>(GetProcAddress(user32, "SetProcessDPIAware")));
    if (setAware != nullptr)
        setAware();
}

/// One at a time: a second start brings the first one's dialog to the
/// front and ends. The first keeps its handle to the end, which is what
/// keeps the name.
bool anotherIsOpen()
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kSingleInstance);
    if (mutex == nullptr)
        return false;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
    {
        gSingleInstance = mutex;
        return false;
    }
    CloseHandle(mutex);
    HWND dialog = FindWindowW(L"#32770", kDialogCaption);
    if (dialog != nullptr)
    {
        if (IsIconic(dialog))
            ShowWindow(dialog, SW_RESTORE);
        SetForegroundWindow(dialog);
    }
    return true;
}

int openSettings()
{
    if (anotherIsOpen())
        return 0;
    InitCommonControls();
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // As hosts do: the driver's CLSID as the interface too.
    IASIO* driver = nullptr;
    const HRESULT made = CoCreateInstance(kDriverClsid, nullptr, CLSCTX_INPROC_SERVER, kDriverClsid,
                                          reinterpret_cast<void**>(&driver));
    int code = 0;
    if (SUCCEEDED(made) && driver != nullptr)
    {
        // Without init: the card is not opened, and the dialog says so.
        driver->controlPanel();
        driver->Release();
    }
    else if (made == REGDB_E_CLASSNOTREG)
    {
        say(L"AJA Duplex ASIO is not installed on this PC.\n\nTo install it, right-click install-driver.cmd, in "
            L"the folder of the driver's ZIP, and choose Run as administrator.");
        code = 1;
    }
    else
    {
        wchar_t number[16] = {};
        std::swprintf(number, sizeof(number) / sizeof(number[0]), L"0x%08lX", static_cast<unsigned long>(made));
        say(std::wstring(L"AJA Duplex ASIO could not be loaded (error ") + number +
            L").\n\nduplex-asiotest --installed, in the folder of the driver's ZIP, says what programs find of "
            L"it; installing it again (install-driver.cmd) puts it right.");
        code = 1;
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return code;
}

/// The Start menu's programs folder for every user ("C:\ProgramData\Microsoft
/// \Windows\Start Menu\Programs"), made if it is not there. Empty if there
/// is none.
std::wstring startMenuFolder()
{
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT,
                                path)))
        return std::wstring();
    return path;
}

/// This program's own full path.
std::wstring thisProgram()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return std::wstring();
        if (length < path.size())
        {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

int addShortcut()
{
    const std::wstring folder = startMenuFolder();
    const std::wstring program = thisProgram();
    if (folder.empty() || program.empty())
        return 1;
    const std::size_t slash = program.find_last_of(L'\\');
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                      reinterpret_cast<void**>(&link));
    if (SUCCEEDED(result))
    {
        link->SetPath(program.c_str());
        if (slash != std::wstring::npos)
            link->SetWorkingDirectory(program.substr(0, slash).c_str());
        link->SetDescription(L"The AJA Duplex ASIO driver's settings, for programs with no ASIO settings button");
        // Windows' own speaker, from the volume mixer: this program has no
        // icon of its own.
        wchar_t icon[MAX_PATH] = {};
        const DWORD expanded = ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\SndVol.exe", icon, MAX_PATH);
        if (expanded > 0 && expanded <= MAX_PATH && GetFileAttributesW(icon) != INVALID_FILE_ATTRIBUTES)
            link->SetIconLocation(icon, 0);
        IPersistFile* file = nullptr;
        result = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
        if (SUCCEEDED(result))
        {
            result = file->Save((folder + L"\\" + kShortcutFile).c_str(), TRUE);
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return SUCCEEDED(result) ? 0 : 1;
}

int removeShortcut()
{
    const std::wstring folder = startMenuFolder();
    if (folder.empty())
        return 1;
    if (DeleteFileW((folder + L"\\" + kShortcutFile).c_str()))
        return 0;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    LPWSTR* words = CommandLineToArgvW(GetCommandLineW(), &count);
    const std::wstring option = words != nullptr && count > 1 ? words[1] : L"";
    const bool more = words != nullptr && count > 2;
    if (words != nullptr)
        LocalFree(words);

    if (option.empty())
    {
        beDpiAware();
        return openSettings();
    }
    if (option == L"--add-shortcut" && !more)
        return addShortcut();
    if (option == L"--remove-shortcut" && !more)
        return removeShortcut();
    say(L"AJADuplexASIOSettings opens the AJA Duplex ASIO driver's settings. Its options, for the install "
        L"scripts:\n\n--add-shortcut\tthe Start menu's \"AJA Duplex ASIO Settings\", for every user\n"
        L"--remove-shortcut\tand away again");
    return 2;
}
