// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SettingsDialog.h
 * @brief   The driver's settings dialog (Windows), the host's "ASIO settings".
 *
 * Built in code from an in-memory dialog template, so there is no resource
 * file to compile: the input, the SDI and HDMI outputs that play too, the
 * Xpand's analog level, the buffer size offered first, the guard,
 * aggressive mode, Keep my routing, OEM task mode, the card's latencies,
 * the card and the audio system, and a read-only box with what the driver
 * is doing. Sizes and latencies are in samples, as hosts and people say
 * (frames, in ASIO's words and the log's: one sample of every channel),
 * with what they are in milliseconds beside them. OK checks every field
 * and keeps the dialog open, saying which one is wrong, until they all are
 * right. Saving them, and telling the host, is the caller's part. Under
 * the buttons: which version this is, of when and whose, and the project's
 * address, which a click opens in the browser.
 */

#pragma once

#include "common/DriverSettings.h"

#include <windows.h>

#include <string>

namespace ajaduplex::ui
{

struct DialogContent
{
    std::wstring title = L"AJA Duplex ASIO";
    DriverSettings settings;        ///< in: shown; out: as edited, when saved
    bool latenciesFromCard = false; ///< the simulated card reports its own: the latency boxes are off
    std::wstring status;            ///< what the driver is doing, shown read-only
    std::wstring logFolder;         ///< "Open log folder" opens it
    std::wstring about;             ///< one line: the version, its date, whose it is
    std::wstring homepage;          ///< the project's address (https://...): a button that opens it

    // Out
    bool saved = false;
    std::wstring error;             ///< why OK did not close it (also shown in a message box unless quiet)

    // For tests
    bool quiet = false;             ///< no message boxes
    bool modeless = false;          ///< opened with openSettingsDialog: OK and Cancel destroy the window
};

/// How runSettingsDialog ended.
enum class DialogResult
{
    Saved,        ///< content.settings holds what was entered
    Cancelled,
    AlreadyOpen,  ///< one was open already: it was brought to the front instead
};

/// Modal, over `owner` (see chooseDialogOwner), brought to the front. One at
/// a time: while one is open, another call (a host whose window stays
/// clickable, or that calls from another thread) brings that one to the
/// front and returns at once.
DialogResult runSettingsDialog(HWND owner, DialogContent& content);

/// The window the dialog goes over: the one the user has just clicked in
/// (the foreground window), if this thread owns it, so the dialog is modal
/// to it, stays above it and needs no taskbar button to be found; else
/// `hint` (the window the host gave init) if this thread owns it; else none.
/// A window of another thread is never taken: owning a window ties the two
/// threads' input together, and a host thread waiting on this one would
/// hang.
HWND chooseDialogOwner(HWND hint);

/// Modeless, for tests: the window, or null if it could not be made (no
/// desktop, for instance).
HWND openSettingsDialog(HWND owner, DialogContent& content);

/// What the dialog's fields say, checked. False with `error` naming the
/// field that is wrong.
bool readSettingsDialog(HWND dialog, DriverSettings& settings, std::wstring& error);

/// Control IDs, for tests that fill the dialog in.
enum DialogControl : int
{
    kCardEdit = 1001,
    kAudioSystemCombo,
    kInputCombo,
    kBufferCombo,
    kGuardEdit,
    kAggressiveCheck,
    kInputLatencyEdit,
    kOutputLatencyEdit,
    kStatusEdit,
    kOpenLogButton,
    kLatencyNote,
    kKeepRoutingCheck,
    kAnalogLevelCombo,
    kOemTasksCheck,
    kSdiOut1Check,     ///< SDI out n is kSdiOut1Check + n - 1
    kSdiOut2Check,
    kHdmiOutCheck,
    kAboutText,        ///< DialogContent::about
    kHomepageButton,   ///< DialogContent::homepage, without its "https://"
    kGuardText,        ///< what the guard box holds, in milliseconds: follows the box
};

} // namespace ajaduplex::ui
