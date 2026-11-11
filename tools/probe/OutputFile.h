// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    OutputFile.h
 * @brief   Names and writes the files duplex-probe leaves behind.
 */

#pragma once

#include <ctime>
#include <string>

namespace ajaduplex::probe
{

/// "<command>-<label>-<yyyymmdd>-<hhmmss>.<extension>", e.g.
/// "info-idle-20261108-081530.txt". The label is passed through
/// text::sanitizeLabel; when nothing is left of it, that part is dropped.
std::string reportFileName(const std::string& command, const std::string& label,
                           const std::tm& when, const std::string& extension = "txt");

/// `folder` joined with `fileName`; an empty folder or "." gives the bare name.
std::string joinPath(const std::string& folder, const std::string& fileName);

/// Writes `text` to `path` (text mode: CRLF line ends on Windows), creating
/// the folder if needed. Returns false with `error` set on failure.
bool writeTextFile(const std::string& path, const std::string& text, std::string& error);

} // namespace ajaduplex::probe
