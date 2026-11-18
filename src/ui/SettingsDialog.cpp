// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "ui/SettingsDialog.h"

#include "common/TextFormat.h"

#include <shellapi.h>

#include <atomic>
#include <vector>

namespace ajaduplex::ui
{

namespace
{

// Predefined window classes in a dialog template.
constexpr WORD kButton = 0x0080;
constexpr WORD kEdit = 0x0081;
constexpr WORD kStatic = 0x0082;
constexpr WORD kComboBox = 0x0085;

constexpr int kAudioSystems = 2;      // the KONA X has two
constexpr int kSdiInputs = 2;         // and two SDI inputs (and one HDMI)
constexpr int kSdiOutputs = 2;        // two SDI outputs (and one HDMI)
constexpr int kBufferChoices = 6;     // 64 to 2048

/// The modal dialog open now, if any (runSettingsDialog: one at a time).
std::atomic<bool> gModalRunning{false};
std::atomic<HWND> gModalDialog{nullptr};

void bringToFront(HWND window)
{
    if (window == nullptr || !IsWindow(window))
        return;
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

/// ASCII text, as the dialog's controls take it.
std::wstring wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

/// The analog level combo's choices, in order: 0 is "as the card has it".
constexpr std::uint32_t kLevels[] = {0, 24, 18, 15, 12};
constexpr int kLevelChoices = static_cast<int>(sizeof(kLevels) / sizeof(kLevels[0]));

/// A DLGTEMPLATE and its items, laid out in memory as Windows expects:
/// WORDs, each item on a DWORD boundary.
class DialogTemplate
{
public:
    DialogTemplate(const std::wstring& title, short width, short height)
    {
        // In front when it opens, and on the taskbar even when it has an
        // owner, so it can always be found.
        dword(DS_MODALFRAME | DS_SETFONT | DS_CENTER | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU);
        dword(WS_EX_APPWINDOW);
        word(0);   // item count, kept up to date by add()
        word(0);
        word(0);
        word(static_cast<WORD>(width));
        word(static_cast<WORD>(height));
        word(0);   // no menu
        word(0);   // the standard dialog class
        text(title.c_str());
        word(9);   // font size, points
        text(L"Segoe UI");
    }

    void add(WORD windowClass, const wchar_t* caption, int id, short x, short y, short width, short height,
             DWORD style)
    {
        if (data_.size() % 2 != 0)
            word(0);
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        word(static_cast<WORD>(x));
        word(static_cast<WORD>(y));
        word(static_cast<WORD>(width));
        word(static_cast<WORD>(height));
        word(static_cast<WORD>(id));
        word(0xFFFF);
        word(windowClass);
        text(caption != nullptr ? caption : L"");
        word(0);   // no creation data
        ++data_[kCountIndex];
    }

    const DLGTEMPLATE* get() const { return reinterpret_cast<const DLGTEMPLATE*>(data_.data()); }

private:
    static constexpr std::size_t kCountIndex = 4;

    void word(WORD value) { data_.push_back(value); }
    void dword(DWORD value)
    {
        word(LOWORD(value));
        word(HIWORD(value));
    }
    void text(const wchar_t* value)
    {
        while (*value != L'\0')
            word(static_cast<WORD>(*value++));
        word(0);
    }

    std::vector<WORD> data_;
};

DialogTemplate buildTemplate(const DialogContent& content)
{
    DialogTemplate t(content.title + L" settings", 280, 358);
    const DWORD label = SS_LEFT;
    const DWORD number = ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP;
    const DWORD list = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;

    t.add(kStatic, L"Input", -1, 7, 12, 110, 10, label);
    t.add(kComboBox, nullptr, kInputCombo, 120, 10, 152, 60, list);

    // The SDI outputs carry all 16 channels, so they add outputs 9-16; the
    // HDMI output, outputs 1-8. Off by default: their video may be another
    // program's.
    t.add(kStatic, L"Outputs also go to", -1, 7, 30, 110, 10, label);
    for (int n = 0; n < kSdiOutputs; ++n)
        t.add(kButton, (L"SDI " + std::to_wstring(n + 1)).c_str(), kSdiOut1Check + n, static_cast<short>(120 + 44 * n),
              28, 40, 12, check);
    t.add(kButton, L"HDMI", kHdmiOutCheck, static_cast<short>(120 + 44 * kSdiOutputs), 28, 40, 12, check);

    t.add(kStatic, L"Analog level: 0 dBFS at", -1, 7, 48, 110, 10, label);
    t.add(kComboBox, nullptr, kAnalogLevelCombo, 120, 46, 76, 80, list);
    // Two lines high, no more: the note under it starts where it ends.
    // Static texts that overlap paint over each other.
    t.add(kStatic, L"the Xpand's, for the whole card", -1, 200, 48, 74, 16, label);

    // In samples, with what they are in milliseconds. Only an offer: a host
    // picks its own size (vMix always asks for 1024).
    t.add(kStatic, L"Buffer size offered first", -1, 7, 66, 110, 10, label);
    t.add(kComboBox, nullptr, kBufferCombo, 120, 64, 100, 100, list);
    t.add(kStatic, L"a host may pick another", -1, 224, 64, 50, 17, label);

    t.add(kStatic, L"Guard (G)", -1, 7, 84, 110, 10, label);
    t.add(kEdit, nullptr, kGuardEdit, 120, 82, 40, 12, number);
    t.add(kStatic, nullptr, kGuardText, 164, 81, 110, 18, label);

    t.add(kButton, L"Aggressive: S = N/2 + G instead of N + G, half a block less latency, for hosts that use under "
                   L"half a block",
          kAggressiveCheck, 7, 104, 266, 18, check | BS_MULTILINE);
    t.add(kButton, L"Keep my routing: if another program changes what the driver set (its outputs, the monitor, "
                   L"the Xpand's select and level, the input), put it back",
          kKeepRoutingCheck, 7, 124, 266, 18, check | BS_MULTILINE);
    t.add(kButton, L"OEM task mode while playing (recommended): AJA's services stop putting the outputs back on "
                   L"AJA's mixer (back as it was afterwards)",
          kOemTasksCheck, 7, 144, 266, 18, check | BS_MULTILINE);

    t.add(kStatic, L"The card's latency: in", -1, 7, 170, 110, 10, label);
    t.add(kEdit, nullptr, kInputLatencyEdit, 120, 168, 40, 12, number);
    t.add(kStatic, L"out", -1, 166, 170, 14, 10, label);
    t.add(kEdit, nullptr, kOutputLatencyEdit, 182, 168, 40, 12, number);
    t.add(kStatic, L"samples", -1, 226, 170, 40, 10, label);
    t.add(kStatic, nullptr, kLatencyNote, 7, 184, 266, 26, label);

    t.add(kStatic, L"Card (device index)", -1, 7, 216, 110, 10, label);
    t.add(kEdit, nullptr, kCardEdit, 120, 214, 24, 12, number);
    t.add(kStatic, L"Audio system", -1, 152, 216, 46, 10, label);
    t.add(kComboBox, nullptr, kAudioSystemCombo, 198, 214, 74, 60, list);

    t.add(kStatic, L"Now", -1, 7, 234, 110, 10, label);
    t.add(kEdit, nullptr, kStatusEdit, 7, 244, 266, 56,
          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER);

    t.add(kButton, L"Open log folder", kOpenLogButton, 7, 306, 70, 14, BS_PUSHBUTTON | WS_TABSTOP);
    t.add(kButton, L"OK", IDOK, 166, 306, 50, 14, BS_DEFPUSHBUTTON | WS_TABSTOP);
    t.add(kButton, L"Cancel", IDCANCEL, 222, 306, 50, 14, BS_PUSHBUTTON | WS_TABSTOP);

    // Which version, of when and whose; and where the project lives, as a
    // button rather than a link control, which would need version 6 of the
    // common controls, and that is the host's to have or not.
    t.add(kStatic, nullptr, kAboutText, 7, 327, 266, 10, label);
    t.add(kButton, nullptr, kHomepageButton, 7, 339, 170, 13, BS_PUSHBUTTON | WS_TABSTOP);
    return t;
}

void addItem(HWND dialog, int id, const wchar_t* text)
{
    SendDlgItemMessageW(dialog, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

void select(HWND dialog, int id, int index)
{
    SendDlgItemMessageW(dialog, id, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

int selected(HWND dialog, int id)
{
    return static_cast<int>(SendDlgItemMessageW(dialog, id, CB_GETCURSEL, 0, 0));
}

/// The input the combo shows, and which SDI input when it is one: Analog,
/// AES, SDI 1..kSdiInputs, HDMI, in the order fill() adds them. False when
/// nothing is chosen.
bool chosenInput(HWND dialog, InputChoice& input, std::uint32_t& sdiInput)
{
    const int index = selected(dialog, kInputCombo);
    if (index == 0)
        input = InputChoice::Analog;
    else if (index == 1)
        input = InputChoice::Aes;
    else if (index >= 2 && index < 2 + kSdiInputs)
    {
        input = InputChoice::Sdi;
        sdiInput = static_cast<std::uint32_t>(index - 1);
    }
    else if (index == 2 + kSdiInputs)
        input = InputChoice::Hdmi;
    else
        return false;
    return true;
}

/// The input was changed: the card's latency on the way out goes with it
/// while what the box holds is one of the card's own figures. A number that
/// is none of them was typed, or measured on this PC, and stays.
void followInput(HWND dialog)
{
    BOOL ok = FALSE;
    const UINT shown = GetDlgItemInt(dialog, kOutputLatencyEdit, &ok, FALSE);
    InputChoice input = InputChoice::Analog;
    std::uint32_t sdiInput = 1;
    if (!ok || !chosenInput(dialog, input, sdiInput))
        return;
    const InputChoice inputs[] = {InputChoice::Analog, InputChoice::Aes, InputChoice::Sdi, InputChoice::Hdmi};
    for (const InputChoice one : inputs)
    {
        if (shown == DriverSettings::cardLatencyFrames(one))
        {
            SetDlgItemInt(dialog, kOutputLatencyEdit, DriverSettings::cardLatencyFrames(input), FALSE);
            return;
        }
    }
}

/// The text beside the guard box: what it holds, in milliseconds, and what
/// it is.
void showGuard(HWND dialog)
{
    BOOL ok = FALSE;
    const UINT shown = GetDlgItemInt(dialog, kGuardEdit, &ok, FALSE);
    const std::wstring range = ok && shown <= DriverSettings::kMaxGuardFrames
                                   ? L"samples (" + wide(text::milliseconds(shown)) + L")"
                                   : std::wstring(L"samples, 0 to 1024,");
    SetDlgItemTextW(dialog, kGuardText, (range + L" kept ahead beyond a block").c_str());
}

void fill(HWND dialog, const DialogContent& content)
{
    const DriverSettings& s = content.settings;

    // Analog, AES, SDI 1..kSdiInputs, HDMI.
    addItem(dialog, kInputCombo, L"Analog: the Xpand's XLR in L/R");
    addItem(dialog, kInputCombo, L"AES: the Xpand's AES in 1-8");
    for (int i = 1; i <= kSdiInputs; ++i)
        addItem(dialog, kInputCombo, (L"SDI " + std::to_wstring(i) + L": its 16 embedded channels").c_str());
    addItem(dialog, kInputCombo, L"HDMI: its 8 embedded channels");
    int input = 0;
    switch (s.input)
    {
        case InputChoice::Analog: input = 0; break;
        case InputChoice::Aes:    input = 1; break;
        case InputChoice::Sdi:
            input = 1 + (s.sdiInput >= 1 && s.sdiInput <= static_cast<std::uint32_t>(kSdiInputs)
                             ? static_cast<int>(s.sdiInput)
                             : 1);
            break;
        case InputChoice::Hdmi:   input = 2 + kSdiInputs; break;
    }
    select(dialog, kInputCombo, input);
    for (int n = 0; n < kSdiOutputs; ++n)
        CheckDlgButton(dialog, kSdiOut1Check + n, (s.sdiOutputs & (1u << n)) != 0 ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kHdmiOutCheck, s.hdmiOutput ? BST_CHECKED : BST_UNCHECKED);

    int level = 0;
    for (int i = 0; i < kLevelChoices; ++i)
    {
        addItem(dialog, kAnalogLevelCombo,
                kLevels[i] == 0 ? L"As the card has it" : (L"+" + std::to_wstring(kLevels[i]) + L" dBu").c_str());
        if (kLevels[i] == s.analogLevelDbu)
            level = i;
    }
    select(dialog, kAnalogLevelCombo, level);

    int buffer = 0;
    for (int i = 0; i < kBufferChoices; ++i)
    {
        const unsigned size = DriverSettings::kMinBufferFrames << i;
        addItem(dialog, kBufferCombo,
                (std::to_wstring(size) + L" samples (" + wide(text::milliseconds(size)) + L")").c_str());
        if (size == s.bufferFrames)
            buffer = i;
    }
    select(dialog, kBufferCombo, buffer);

    SetDlgItemInt(dialog, kGuardEdit, s.guardFrames, FALSE);
    showGuard(dialog);
    CheckDlgButton(dialog, kAggressiveCheck, s.aggressive ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kKeepRoutingCheck, s.keepRouting ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kOemTasksCheck, s.oemTasks ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dialog, kInputLatencyEdit, s.inputLatencyFrames, FALSE);
    SetDlgItemInt(dialog, kOutputLatencyEdit, s.outputLatencyFrames, FALSE);
    if (content.latenciesFromCard)
    {
        EnableWindow(GetDlgItem(dialog, kInputLatencyEdit), FALSE);
        EnableWindow(GetDlgItem(dialog, kOutputLatencyEdit), FALSE);
        SetDlgItemTextW(dialog, kLatencyNote, L"The simulated card reports its own.");
    }
    else
    {
        SetDlgItemTextW(dialog, kLatencyNote,
                        L"What the card and its converters add, in samples (48 are 1 ms). The driver's own: 208 out "
                        L"with the analog input (4.3 ms), 130 with AES (2.7 ms), 0 with SDI or HDMI (not "
                        L"measured).");
    }

    SetDlgItemInt(dialog, kCardEdit, s.card, FALSE);
    addItem(dialog, kAudioSystemCombo, L"The last (2)");
    for (int a = 1; a <= kAudioSystems; ++a)
        addItem(dialog, kAudioSystemCombo, std::to_wstring(a).c_str());
    select(dialog, kAudioSystemCombo,
           s.audioSystem <= static_cast<std::uint32_t>(kAudioSystems) ? static_cast<int>(s.audioSystem) : 0);

    SetDlgItemTextW(dialog, kStatusEdit, content.status.c_str());
    EnableWindow(GetDlgItem(dialog, kOpenLogButton), content.logFolder.empty() ? FALSE : TRUE);

    SetDlgItemTextW(dialog, kAboutText, content.about.c_str());
    // The address as people read it, without its scheme; no address, no button.
    const std::wstring scheme = L"https://";
    const bool web = content.homepage.compare(0, scheme.size(), scheme) == 0;
    SetDlgItemTextW(dialog, kHomepageButton, web ? content.homepage.c_str() + scheme.size() : L"");
    ShowWindow(GetDlgItem(dialog, kHomepageButton), web ? SW_SHOW : SW_HIDE);
}

bool readNumber(HWND dialog, int id, std::uint32_t limit, std::uint32_t& value)
{
    BOOL ok = FALSE;
    const UINT read = GetDlgItemInt(dialog, id, &ok, FALSE);
    if (!ok || read > limit)
        return false;
    value = static_cast<std::uint32_t>(read);
    return true;
}

void finish(HWND dialog, DialogContent& content, bool saved)
{
    content.saved = saved;
    if (content.modeless)
        DestroyWindow(dialog);
    else
        EndDialog(dialog, saved ? IDOK : IDCANCEL);
}

INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG)
    {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        auto* content = reinterpret_cast<DialogContent*>(lParam);
        fill(dialog, *content);
        if (!content->modeless)
        {
            gModalDialog = dialog;
            SetForegroundWindow(dialog);
        }
        return TRUE;
    }
    if (message == WM_DESTROY)
    {
        HWND self = dialog;
        gModalDialog.compare_exchange_strong(self, nullptr);
        return FALSE;
    }
    auto* content = reinterpret_cast<DialogContent*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (content == nullptr || message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wParam))
    {
        case IDOK:
        {
            DriverSettings edited = content->settings;
            std::wstring error;
            if (!readSettingsDialog(dialog, edited, error))
            {
                content->error = error;
                if (!content->quiet)
                    MessageBoxW(dialog, error.c_str(), content->title.c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            content->error.clear();
            content->settings = edited;
            finish(dialog, *content, true);
            return TRUE;
        }
        case IDCANCEL:
            finish(dialog, *content, false);
            return TRUE;
        case kOpenLogButton:
            if (!content->logFolder.empty())
            {
                CreateDirectoryW(content->logFolder.c_str(), nullptr);
                ShellExecuteW(dialog, L"open", content->logFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return TRUE;
        case kHomepageButton:
            // Only what fill() showed as an address: the browser opens it.
            if (content->homepage.compare(0, 8, L"https://") == 0)
                ShellExecuteW(dialog, L"open", content->homepage.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case kInputCombo:
            if (HIWORD(wParam) != CBN_SELCHANGE)
                return FALSE;
            if (!content->latenciesFromCard)
                followInput(dialog);
            return TRUE;
        case kGuardEdit:
            if (HIWORD(wParam) != EN_CHANGE)
                return FALSE;
            showGuard(dialog);
            return TRUE;
        default:
            return FALSE;
    }
}

} // namespace

bool readSettingsDialog(HWND dialog, DriverSettings& settings, std::wstring& error)
{
    DriverSettings s = settings;
    chosenInput(dialog, s.input, s.sdiInput);   // nothing chosen: as it was
    // Outputs the dialog does not show (SDI 3 and up) stay as they were.
    for (int n = 0; n < kSdiOutputs; ++n)
    {
        const std::uint32_t bit = 1u << n;
        s.sdiOutputs = IsDlgButtonChecked(dialog, kSdiOut1Check + n) == BST_CHECKED ? (s.sdiOutputs | bit)
                                                                                     : (s.sdiOutputs & ~bit);
    }
    s.hdmiOutput = IsDlgButtonChecked(dialog, kHdmiOutCheck) == BST_CHECKED;
    const int level = selected(dialog, kAnalogLevelCombo);
    if (level >= 0 && level < kLevelChoices)
        s.analogLevelDbu = kLevels[level];
    const int buffer = selected(dialog, kBufferCombo);
    if (buffer >= 0 && buffer < kBufferChoices)
        s.bufferFrames = DriverSettings::kMinBufferFrames << buffer;
    if (!readNumber(dialog, kGuardEdit, DriverSettings::kMaxGuardFrames, s.guardFrames))
    {
        error = L"The guard is a number of samples from 0 to 1024.";
        return false;
    }
    s.aggressive = IsDlgButtonChecked(dialog, kAggressiveCheck) == BST_CHECKED;
    s.keepRouting = IsDlgButtonChecked(dialog, kKeepRoutingCheck) == BST_CHECKED;
    s.oemTasks = IsDlgButtonChecked(dialog, kOemTasksCheck) == BST_CHECKED;
    if (!readNumber(dialog, kInputLatencyEdit, DriverSettings::kMaxLatencyFrames, s.inputLatencyFrames) ||
        !readNumber(dialog, kOutputLatencyEdit, DriverSettings::kMaxLatencyFrames, s.outputLatencyFrames))
    {
        error = L"The card's latencies are numbers of samples from 0 to 48000 (a second).";
        return false;
    }
    if (!readNumber(dialog, kCardEdit, 15, s.card))
    {
        error = L"The card is a device index from 0 to 15 (duplex-probe list shows them).";
        return false;
    }
    const int system = selected(dialog, kAudioSystemCombo);
    s.audioSystem = system > 0 ? static_cast<std::uint32_t>(system) : 0;
    settings = s;
    return true;
}

DialogResult runSettingsDialog(HWND owner, DialogContent& content)
{
    content.saved = false;
    if (gModalRunning.exchange(true))
    {
        bringToFront(gModalDialog.load());
        return DialogResult::AlreadyOpen;
    }
    content.modeless = false;
    const DialogTemplate t = buildTemplate(content);
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.get(), owner, dialogProc,
                                                   reinterpret_cast<LPARAM>(&content));
    gModalRunning = false;
    return result == IDOK && content.saved ? DialogResult::Saved : DialogResult::Cancelled;
}

HWND chooseDialogOwner(HWND hint)
{
    const DWORD thread = GetCurrentThreadId();
    HWND foreground = GetForegroundWindow();
    if (foreground != nullptr && GetWindowThreadProcessId(foreground, nullptr) == thread)
        return foreground;
    if (hint != nullptr && IsWindow(hint) && GetWindowThreadProcessId(hint, nullptr) == thread)
        return hint;
    return nullptr;
}

HWND openSettingsDialog(HWND owner, DialogContent& content)
{
    content.modeless = true;
    content.saved = false;
    const DialogTemplate t = buildTemplate(content);
    return CreateDialogIndirectParamW(GetModuleHandleW(nullptr), t.get(), owner, dialogProc,
                                      reinterpret_cast<LPARAM>(&content));
}

} // namespace ajaduplex::ui
