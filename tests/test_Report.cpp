// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "common/Report.h"

using ajaduplex::Report;

TEST_CASE(Report, KeysAlignWithinASection)
{
    Report report;
    report.addSection("device").add("model", "KONA X").add("serial number", "1X0001");
    EXPECT_EQ(report.render(true), std::string("[device]\n"
                                               "model         = KONA X\n"
                                               "serial number = 1X0001\n"));
}

TEST_CASE(Report, MultiLineValueContinuesUnderItsFirstLine)
{
    Report report;
    report.addSection("s").add("key", "first\nsecond");
    EXPECT_EQ(report.render(true), std::string("[s]\n"
                                               "key = first\n"
                                               "      second\n"));
}

TEST_CASE(Report, EmptyValueAndTrailingSpaces)
{
    Report report;
    report.addSection("s").add("empty", "").add("k", "v   ");
    EXPECT_EQ(report.render(true), std::string("[s]\n"
                                               "empty =\n"
                                               "k     = v\n"));
}

TEST_CASE(Report, BlocksAreIndentedAndKeepEmptyLines)
{
    Report report;
    report.addSection("s").block("line one\n\nline two\n");
    EXPECT_EQ(report.render(true), std::string("[s]\n"
                                               "    line one\n"
                                               "\n"
                                               "    line two\n"));
}

TEST_CASE(Report, CarriageReturnsAreDropped)
{
    Report report;
    report.addSection("s").block("a\r\nb\r\n");
    EXPECT_EQ(report.render(true), std::string("[s]\n    a\n    b\n"));
}

TEST_CASE(Report, DetailSectionsOnlyInFullRendering)
{
    Report report;
    report.addSection("summary").add("a", "1");
    report.addSection("registers", true).add("b", "2");
    report.addSection("tail").add("c", "3");

    EXPECT_EQ(report.render(false), std::string("[summary]\na = 1\n\n[tail]\nc = 3\n"));
    EXPECT_EQ(report.render(true), std::string("[summary]\na = 1\n\n[registers]\nb = 2\n\n[tail]\nc = 3\n"));
}

TEST_CASE(Report, SectionReferencesSurviveMoreSections)
{
    Report report;
    Report::Section& first = report.addSection("first");
    for (int i = 0; i < 200; ++i)
        report.addSection("filler " + std::to_string(i));
    first.add("late", "still here");
    EXPECT_EQ(report.sections().front().render(), std::string("[first]\nlate = still here\n"));
}

TEST_CASE(Report, EmptyReportRendersNothing)
{
    Report report;
    EXPECT_EQ(report.render(true), std::string());
}
