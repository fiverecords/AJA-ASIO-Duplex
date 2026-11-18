// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Windows only: the settings dialog, opened modeless, filled in and read
// back the way a user would, without a user. Where there is no desktop to
// open it on, the cases say so and pass.

#include "test_framework.h"

#if defined(_WIN32)

#include "ui/SettingsDialog.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace ajaduplex;
using namespace ajaduplex::ui;

namespace
{

std::string narrow(const std::wstring& text)
{
    std::string out;
    for (const wchar_t c : text)
        out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

DriverSettings unusual()
{
    DriverSettings s;
    s.input = InputChoice::Aes;
    s.bufferFrames = 256;
    s.guardFrames = 48;
    s.aggressive = true;
    s.keepRouting = true;
    s.analogLevelDbu = 18;
    s.oemTasks = true;
    s.inputLatencyFrames = 30;
    s.outputLatencyFrames = 42;
    s.card = 1;
    s.audioSystem = 2;
    return s;
}

HWND open(DialogContent& content)
{
    content.quiet = true;
    HWND dialog = openSettingsDialog(nullptr, content);
    if (dialog == nullptr)
        std::printf("    (skipped: no desktop to open a dialog on)\n");
    return dialog;
}

} // namespace

TEST_CASE(SettingsDialog, ShowsTheSettingsAndChecksWhatIsEntered)
{
    DialogContent content;
    content.settings = unusual();
    content.status = L"Running.";
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;

    // What it shows reads back as what it was given.
    DriverSettings read;
    std::wstring error;
    ASSERT_TRUE(readSettingsDialog(dialog, read, error));
    EXPECT_TRUE(read.input == InputChoice::Aes);
    EXPECT_EQ(read.bufferFrames, 256u);
    EXPECT_EQ(read.guardFrames, 48u);
    EXPECT_TRUE(read.aggressive);
    EXPECT_TRUE(read.keepRouting);
    EXPECT_EQ(read.analogLevelDbu, 18u);
    EXPECT_TRUE(read.oemTasks);
    EXPECT_EQ(read.inputLatencyFrames, 30u);
    EXPECT_EQ(read.outputLatencyFrames, 42u);
    EXPECT_EQ(read.card, 1u);
    EXPECT_EQ(read.audioSystem, 2u);

    // A guard out of range: OK keeps the dialog open and says why.
    SetDlgItemInt(dialog, kGuardEdit, 5000, FALSE);
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    EXPECT_FALSE(content.saved);
    EXPECT_TRUE(IsWindow(dialog) != FALSE);
    EXPECT_CONTAINS(narrow(content.error), "guard");

    // Put right, with another input and buffer size: saved, closed.
    SetDlgItemInt(dialog, kGuardEdit, 16, FALSE);
    SendDlgItemMessageW(dialog, kInputCombo, CB_SETCURSEL, 0, 0);
    SendDlgItemMessageW(dialog, kBufferCombo, CB_SETCURSEL, 1, 0);
    CheckDlgButton(dialog, kKeepRoutingCheck, BST_UNCHECKED);
    SendDlgItemMessageW(dialog, kAnalogLevelCombo, CB_SETCURSEL, 4, 0);   // +12 dBu
    CheckDlgButton(dialog, kOemTasksCheck, BST_UNCHECKED);
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    EXPECT_TRUE(content.saved);
    EXPECT_FALSE(IsWindow(dialog) != FALSE);
    EXPECT_TRUE(content.settings.input == InputChoice::Analog);
    EXPECT_EQ(content.settings.bufferFrames, 128u);
    EXPECT_EQ(content.settings.guardFrames, 16u);
    EXPECT_FALSE(content.settings.keepRouting);
    EXPECT_EQ(content.settings.analogLevelDbu, 12u);
    EXPECT_FALSE(content.settings.oemTasks);
    EXPECT_EQ(content.settings.outputLatencyFrames, 42u);   // untouched
}

TEST_CASE(SettingsDialog, TheEmbeddedInputs)
{
    DialogContent content;
    content.settings.input = InputChoice::Sdi;
    content.settings.sdiInput = 2;
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    DriverSettings read;
    std::wstring error;
    ASSERT_TRUE(readSettingsDialog(dialog, read, error));
    EXPECT_TRUE(read.input == InputChoice::Sdi);
    EXPECT_EQ(read.sdiInput, 2u);
    SendDlgItemMessageW(dialog, kInputCombo, CB_SETCURSEL, 4, 0);   // HDMI
    ASSERT_TRUE(readSettingsDialog(dialog, read, error));
    EXPECT_TRUE(read.input == InputChoice::Hdmi);
    SendDlgItemMessageW(dialog, kInputCombo, CB_SETCURSEL, 2, 0);   // SDI 1
    ASSERT_TRUE(readSettingsDialog(dialog, read, error));
    EXPECT_TRUE(read.input == InputChoice::Sdi);
    EXPECT_EQ(read.sdiInput, 1u);
    SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}

TEST_CASE(SettingsDialog, TheVideoOutputs)
{
    DialogContent content;
    content.settings.sdiOutputs = 2 | 4;   // SDI out 2, and SDI out 3, which the dialog does not show
    content.settings.hdmiOutput = true;
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    EXPECT_EQ(IsDlgButtonChecked(dialog, kSdiOut1Check), UINT(BST_UNCHECKED));
    EXPECT_EQ(IsDlgButtonChecked(dialog, kSdiOut2Check), UINT(BST_CHECKED));
    EXPECT_EQ(IsDlgButtonChecked(dialog, kHdmiOutCheck), UINT(BST_CHECKED));
    DriverSettings read = content.settings;
    std::wstring error;
    ASSERT_TRUE(readSettingsDialog(dialog, read, error));
    EXPECT_EQ(read.sdiOutputs, 6u);
    EXPECT_TRUE(read.hdmiOutput);

    CheckDlgButton(dialog, kSdiOut1Check, BST_CHECKED);
    CheckDlgButton(dialog, kSdiOut2Check, BST_UNCHECKED);
    CheckDlgButton(dialog, kHdmiOutCheck, BST_UNCHECKED);
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    EXPECT_TRUE(content.saved);
    EXPECT_EQ(content.settings.sdiOutputs, 5u);   // SDI out 1, and 3 as it was
    EXPECT_FALSE(content.settings.hdmiOutput);
}

TEST_CASE(SettingsDialog, CancelSavesNothing)
{
    DialogContent content;
    content.settings = unusual();
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    SetDlgItemInt(dialog, kCardEdit, 3, FALSE);
    SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    EXPECT_FALSE(content.saved);
    EXPECT_EQ(content.settings.card, 1u);
    EXPECT_FALSE(IsWindow(dialog) != FALSE);
}

// The card's own latency is the driver's to know, by input: the box shows
// it and goes with the input, until a number of this PC's is in it.
TEST_CASE(SettingsDialog, TheCardsLatencyFollowsTheInput)
{
    DialogContent content;   // the defaults: the analog input
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    const auto shown = [dialog] { return GetDlgItemInt(dialog, kOutputLatencyEdit, nullptr, FALSE); };
    // As a user does it: the combo tells the dialog its choice changed.
    const auto choose = [dialog](int index) {
        SendDlgItemMessageW(dialog, kInputCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kInputCombo, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(GetDlgItem(dialog, kInputCombo)));
    };
    EXPECT_EQ(shown(), 208u);
    wchar_t note[256] = {};
    GetDlgItemTextW(dialog, kLatencyNote, note, 256);
    EXPECT_CONTAINS(narrow(note), "208 out with the analog input (4.3 ms)");
    EXPECT_CONTAINS(narrow(note), "in samples");

    choose(1);   // AES
    EXPECT_EQ(shown(), 130u);
    choose(3);   // SDI 2: not measured
    EXPECT_EQ(shown(), 0u);
    choose(0);   // analog again
    EXPECT_EQ(shown(), 208u);
    EXPECT_EQ(GetDlgItemInt(dialog, kInputLatencyEdit, nullptr, FALSE), 0u);   // all of it is on the way out

    // What this PC measured (card-session's step 7) is none of the card's
    // own figures: it stays, whatever the input.
    SetDlgItemInt(dialog, kOutputLatencyEdit, 215, FALSE);
    choose(1);
    EXPECT_EQ(shown(), 215u);
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    EXPECT_TRUE(content.saved);
    EXPECT_TRUE(content.settings.input == InputChoice::Aes);
    EXPECT_EQ(content.settings.outputLatencyFrames, 215u);
}

TEST_CASE(SettingsDialog, SpeaksOfSamplesWithTheirMilliseconds)
{
    DialogContent content;   // the defaults: 128, guard 256
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    wchar_t text[160] = {};
    // The buffer sizes offered, each with what it lasts.
    const int chosen = static_cast<int>(SendDlgItemMessageW(dialog, kBufferCombo, CB_GETCURSEL, 0, 0));
    ASSERT_TRUE(chosen >= 0);
    SendDlgItemMessageW(dialog, kBufferCombo, CB_GETLBTEXT, static_cast<WPARAM>(chosen),
                        reinterpret_cast<LPARAM>(text));
    EXPECT_EQ(narrow(text), std::string("128 samples (2.7 ms)"));
    SendDlgItemMessageW(dialog, kBufferCombo, CB_GETLBTEXT, 5, reinterpret_cast<LPARAM>(text));
    EXPECT_EQ(narrow(text), std::string("2048 samples (42.7 ms)"));

    // The guard's, following what is typed in its box.
    GetDlgItemTextW(dialog, kGuardText, text, 160);
    EXPECT_EQ(narrow(text), std::string("samples (5.3 ms) kept ahead beyond a block"));
    const auto type = [dialog](const wchar_t* value) {
        SetDlgItemTextW(dialog, kGuardEdit, value);
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kGuardEdit, EN_CHANGE),
                     reinterpret_cast<LPARAM>(GetDlgItem(dialog, kGuardEdit)));
    };
    type(L"32");
    GetDlgItemTextW(dialog, kGuardText, text, 160);
    EXPECT_EQ(narrow(text), std::string("samples (0.7 ms) kept ahead beyond a block"));
    type(L"5000");   // out of range: says what it can be
    GetDlgItemTextW(dialog, kGuardText, text, 160);
    EXPECT_EQ(narrow(text), std::string("samples, 0 to 1024, kept ahead beyond a block"));
    type(L"");
    GetDlgItemTextW(dialog, kGuardText, text, 160);
    EXPECT_EQ(narrow(text), std::string("samples, 0 to 1024, kept ahead beyond a block"));

    // And what OK says when the box is wrong.
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    EXPECT_FALSE(content.saved);
    EXPECT_CONTAINS(narrow(content.error), "number of samples from 0 to 1024");
    SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}

TEST_CASE(SettingsDialog, SaysWhichVersionWhoseAndWhereTheProjectLives)
{
    DialogContent content;
    content.about = L"AJA Duplex ASIO 9.9.9, 2026-11-11, by fiverecords";
    content.homepage = L"https://github.com/fiverecords/AJA-ASIO-Duplex";
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    wchar_t text[160] = {};
    GetDlgItemTextW(dialog, kAboutText, text, 160);
    EXPECT_EQ(narrow(text), std::string("AJA Duplex ASIO 9.9.9, 2026-11-11, by fiverecords"));
    // The address as people read it; the click opens it whole (not made here:
    // it would open a browser).
    GetDlgItemTextW(dialog, kHomepageButton, text, 160);
    EXPECT_EQ(narrow(text), std::string("github.com/fiverecords/AJA-ASIO-Duplex"));
    EXPECT_TRUE((GetWindowLongPtrW(GetDlgItem(dialog, kHomepageButton), GWL_STYLE) & WS_VISIBLE) != 0);
    SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);

    // No address, or something that is not one: no button to click.
    DialogContent bare;
    bare.homepage = L"cmd.exe";
    HWND plain = open(bare);
    if (plain == nullptr)
        return;
    EXPECT_TRUE((GetWindowLongPtrW(GetDlgItem(plain, kHomepageButton), GWL_STYLE) & WS_VISIBLE) == 0);
    SendMessageW(plain, WM_COMMAND, MAKEWPARAM(kHomepageButton, BN_CLICKED), 0);   // and a click does nothing
    EXPECT_TRUE(IsWindow(plain) != FALSE);
    SendMessageW(plain, WM_COMMAND, IDCANCEL, 0);
}

TEST_CASE(SettingsDialog, SimulatedCardsLatenciesCannotBeEdited)
{
    DialogContent content;
    content.latenciesFromCard = true;
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    EXPECT_FALSE(IsWindowEnabled(GetDlgItem(dialog, kInputLatencyEdit)) != FALSE);
    EXPECT_FALSE(IsWindowEnabled(GetDlgItem(dialog, kOutputLatencyEdit)) != FALSE);
    wchar_t note[128] = {};
    GetDlgItemTextW(dialog, kLatencyNote, note, 128);
    EXPECT_CONTAINS(narrow(note), "simulated card");
    SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}

namespace
{

/// What the timers of OneAtATime saw.
struct Seen
{
    int dialogsAtFirst = -1;
    int dialogsDuringSecond = -1;
    DialogResult second = DialogResult::Saved;
    UINT_PTR safety = 0;
    int ticks = 0;
};
Seen gSeen;

bool isDialog(HWND window)
{
    wchar_t name[16] = {};
    return GetClassNameW(window, name, 16) > 0 && std::wstring(name) == L"#32770";
}

BOOL CALLBACK countVisibleDialog(HWND window, LPARAM count)
{
    if (isDialog(window) && IsWindowVisible(window))
        ++*reinterpret_cast<int*>(count);
    return TRUE;
}

int dialogsOfThisThread()
{
    int count = 0;
    EnumThreadWindows(GetCurrentThreadId(), countVisibleDialog, reinterpret_cast<LPARAM>(&count));
    return count;
}

BOOL CALLBACK cancelDialog(HWND window, LPARAM)
{
    if (isDialog(window))
        PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
    return TRUE;
}

void cancelEveryDialog()
{
    EnumThreadWindows(GetCurrentThreadId(), cancelDialog, 0);
}

void CALLBACK cancelEverything(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(nullptr, id);
    cancelEveryDialog();
}

/// The dialog shown yet? Under Wine on a slow X server it can take more
/// than the timer's first tick (2026-11-10: once in two runs): then the
/// next tick, for up to 3 s.
bool shownYet()
{
    return dialogsOfThisThread() > 0 || ++gSeen.ticks >= 30;
}

void CALLBACK countAndCancel(HWND, UINT, UINT_PTR id, DWORD)
{
    if (!shownYet())
        return;
    KillTimer(nullptr, id);
    gSeen.dialogsAtFirst = dialogsOfThisThread();
    cancelEveryDialog();
}

/// While the first dialog runs: the host asks again.
void CALLBACK openASecond(HWND, UINT, UINT_PTR id, DWORD)
{
    if (!shownYet())
        return;
    KillTimer(nullptr, id);
    gSeen.dialogsAtFirst = dialogsOfThisThread();
    // Were a second one to open after all, this closes both rather than
    // leaving the test hanging.
    gSeen.safety = SetTimer(nullptr, 0, 3000, cancelEverything);
    DialogContent again;
    again.quiet = true;
    gSeen.second = runSettingsDialog(nullptr, again);
    gSeen.dialogsDuringSecond = dialogsOfThisThread();
    KillTimer(nullptr, gSeen.safety);
    cancelEveryDialog();
}

} // namespace

TEST_CASE(SettingsDialog, OneAtATime)
{
    DialogContent probe;
    HWND desktop = open(probe);
    if (desktop == nullptr)
        return;
    DestroyWindow(desktop);

    // Asked again while open (a host whose window stays clickable): the
    // one open comes to the front, and no second one opens.
    gSeen = Seen();
    DialogContent first;
    first.quiet = true;
    UINT_PTR timer = SetTimer(nullptr, 0, 100, openASecond);
    ASSERT_TRUE(timer != 0);
    EXPECT_TRUE(runSettingsDialog(nullptr, first) == DialogResult::Cancelled);
    KillTimer(nullptr, timer);
    EXPECT_EQ(gSeen.dialogsAtFirst, 1);
    EXPECT_TRUE(gSeen.second == DialogResult::AlreadyOpen);
    EXPECT_EQ(gSeen.dialogsDuringSecond, 1);

    // Once closed, it opens again.
    gSeen = Seen();
    timer = SetTimer(nullptr, 0, 100, countAndCancel);
    EXPECT_TRUE(runSettingsDialog(nullptr, first) == DialogResult::Cancelled);
    KillTimer(nullptr, timer);
    EXPECT_EQ(gSeen.dialogsAtFirst, 1);
}

TEST_CASE(SettingsDialog, IsOnTheTaskbar)
{
    // With an owner it would have no taskbar button, and behind the host it
    // would look as if nothing had opened.
    DialogContent content;
    HWND dialog = open(content);
    if (dialog == nullptr)
        return;
    EXPECT_TRUE((GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_APPWINDOW) != 0);
    SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}

TEST_CASE(SettingsDialog, TheOwnerIsAWindowOfThisThread)
{
    std::atomic<HWND> theirs{nullptr};
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    std::thread other([&] {
        HWND window = CreateWindowExW(0, L"STATIC", L"another thread", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        theirs = window;
        ready = true;
        while (!done)
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageW(&message);
            Sleep(5);
        }
        if (window != nullptr)
            DestroyWindow(window);
    });
    while (!ready)
        Sleep(1);
    const DWORD thread = GetCurrentThreadId();
    if (theirs.load() == nullptr)
        std::printf("    (skipped: no desktop to make a window on)\n");
    else
    {
        // Another thread's window is never the owner, even when it is the
        // one init was given.
        const HWND chosen = chooseDialogOwner(theirs.load());
        EXPECT_TRUE(chosen != theirs.load());
        EXPECT_TRUE(chosen == nullptr || GetWindowThreadProcessId(chosen, nullptr) == thread);

        // This thread's is taken (unless the window the user clicked in is
        // this thread's too, which is taken first).
        // (No ASSERT here: the other thread must be joined.)
        HWND mine = CreateWindowExW(0, L"STATIC", L"this thread", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        EXPECT_TRUE(mine != nullptr);
        const HWND picked = chooseDialogOwner(mine);
        EXPECT_TRUE(picked != nullptr && GetWindowThreadProcessId(picked, nullptr) == thread);
        if (mine != nullptr)
            DestroyWindow(mine);
    }
    done = true;
    other.join();
}

#endif
