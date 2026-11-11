// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Report.h
 * @brief   Plain-text report built from titled sections.
 *
 * A section holds "key = value" lines and preformatted blocks, in the order
 * they were added. The rendering is meant to be read by a person and to be
 * compared with a diff: one fact per line, keys aligned within a section,
 * sections in a fixed order.
 *
 * Sections marked as detail (register dumps and the like) are left out of
 * the short rendering that goes to the console; the full rendering, written
 * to the report file, has them all.
 */

#pragma once

#include <deque>
#include <string>
#include <vector>

namespace ajaduplex
{

class Report
{
public:
    class Section
    {
    public:
        Section(std::string sectionTitle, bool isDetailSection);

        /// Adds a "key = value" line. A value with line breaks continues on
        /// the following lines, aligned with its first line.
        Section& add(std::string key, std::string value);

        /// Adds preformatted text, indented as a whole.
        Section& block(std::string text);

        const std::string& title() const { return title_; }
        bool isDetail() const { return detail_; }
        bool empty() const { return items_.empty(); }

        std::string render() const;

    private:
        struct Item
        {
            bool isBlock = false;
            std::string key;
            std::string value;
        };

        std::string title_;
        bool detail_ = false;
        std::vector<Item> items_;
    };

    /// Appends a section and returns it. The reference stays valid while the
    /// report exists (sections live in a deque).
    Section& addSection(std::string title, bool detail = false);

    /// All sections, or only those not marked as detail.
    std::string render(bool includeDetail) const;

    const std::deque<Section>& sections() const { return sections_; }

private:
    std::deque<Section> sections_;
};

} // namespace ajaduplex
