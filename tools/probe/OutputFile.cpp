// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "OutputFile.h"

#include "common/TextFormat.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace ajaduplex::probe
{

std::string reportFileName(const std::string& command, const std::string& label,
                           const std::tm& when, const std::string& extension)
{
    std::string name = command;
    const std::string safeLabel = text::sanitizeLabel(label);
    if (!safeLabel.empty())
        name += "-" + safeLabel;
    name += "-" + text::timestampForFileName(when);
    if (!extension.empty())
        name += "." + extension;
    return name;
}

std::string joinPath(const std::string& folder, const std::string& fileName)
{
    if (folder.empty() || folder == ".")
        return fileName;
    return (std::filesystem::path(folder) / fileName).string();
}

bool writeTextFile(const std::string& path, const std::string& text, std::string& error)
{
    const std::filesystem::path target(path);
    if (target.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec)
        {
            error = "cannot create folder '" + target.parent_path().string() + "': " + ec.message();
            return false;
        }
    }

    std::ofstream out(target);
    if (!out)
    {
        error = "cannot open '" + path + "' for writing";
        return false;
    }
    out << text;
    out.close();
    if (!out)
    {
        error = "write to '" + path + "' failed";
        return false;
    }
    return true;
}

} // namespace ajaduplex::probe
