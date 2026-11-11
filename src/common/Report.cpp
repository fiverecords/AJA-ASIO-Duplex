// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "common/Report.h"

#include <algorithm>
#include <utility>

namespace ajaduplex
{

namespace
{

/// Splits on '\n'; a trailing line break does not produce an empty last line.
std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
        {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    for (std::string& line : lines)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
    }
    return lines;
}

void appendTrimmed(std::string& out, const std::string& line)
{
    const std::size_t end = line.find_last_not_of(' ');
    if (end != std::string::npos)
        out.append(line, 0, end + 1);
    out.push_back('\n');
}

} // namespace

Report::Section::Section(std::string sectionTitle, bool isDetailSection)
    : title_(std::move(sectionTitle)), detail_(isDetailSection)
{
}

Report::Section& Report::Section::add(std::string key, std::string value)
{
    items_.push_back(Item{false, std::move(key), std::move(value)});
    return *this;
}

Report::Section& Report::Section::block(std::string text)
{
    items_.push_back(Item{true, {}, std::move(text)});
    return *this;
}

std::string Report::Section::render() const
{
    std::size_t keyWidth = 0;
    for (const Item& item : items_)
    {
        if (!item.isBlock)
            keyWidth = std::max(keyWidth, item.key.size());
    }

    std::string out = "[" + title_ + "]\n";
    for (const Item& item : items_)
    {
        const std::vector<std::string> lines = splitLines(item.value);
        if (item.isBlock)
        {
            for (const std::string& line : lines)
                appendTrimmed(out, line.empty() ? std::string() : "    " + line);
            continue;
        }

        std::string head = item.key;
        head.append(keyWidth - item.key.size(), ' ');
        head += " =";
        if (lines.empty())
        {
            out += head + "\n";
            continue;
        }
        appendTrimmed(out, head + " " + lines.front());
        const std::string indent(keyWidth + 3, ' ');
        for (std::size_t i = 1; i < lines.size(); ++i)
            appendTrimmed(out, lines[i].empty() ? std::string() : indent + lines[i]);
    }
    return out;
}

Report::Section& Report::addSection(std::string title, bool detail)
{
    sections_.emplace_back(std::move(title), detail);
    return sections_.back();
}

std::string Report::render(bool includeDetail) const
{
    std::string out;
    for (const Section& section : sections_)
    {
        if (section.isDetail() && !includeDetail)
            continue;
        if (!out.empty())
            out += "\n";
        out += section.render();
    }
    return out;
}

} // namespace ajaduplex
