// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "OutputFile.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace ajaduplex::probe;
namespace fs = std::filesystem;

namespace
{

std::tm sampleTime()
{
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 8;
    t.tm_hour = 8;
    t.tm_min = 15;
    t.tm_sec = 30;
    return t;
}

/// A fresh folder under the system temp folder, removed on destruction.
struct TempFolder
{
    fs::path path;

    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("duplex-tests-" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~TempFolder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string readFile(const fs::path& path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE(OutputFile, NameWithAndWithoutLabel)
{
    const std::tm t = sampleTime();
    EXPECT_EQ(reportFileName("info", "idle", t), std::string("info-idle-20261108-081530.txt"));
    EXPECT_EQ(reportFileName("info", "", t), std::string("info-20261108-081530.txt"));
    EXPECT_EQ(reportFileName("info", "AJA ASIO", t), std::string("info-AJA-ASIO-20261108-081530.txt"));
    EXPECT_EQ(reportFileName("info", "***", t), std::string("info-20261108-081530.txt"));
    EXPECT_EQ(reportFileName("loop", "n128", t, "csv"), std::string("loop-n128-20261108-081530.csv"));
}

TEST_CASE(OutputFile, JoinPath)
{
    EXPECT_EQ(joinPath(".", "a.txt"), std::string("a.txt"));
    EXPECT_EQ(joinPath("", "a.txt"), std::string("a.txt"));
    EXPECT_EQ(joinPath("reports", "a.txt"), (fs::path("reports") / "a.txt").string());
}

TEST_CASE(OutputFile, WritesTextAndCreatesTheFolder)
{
    TempFolder temp;
    const fs::path target = temp.path / "sub" / "report.txt";
    std::string error;
    ASSERT_TRUE(writeTextFile(target.string(), "[s]\nk = v\n", error));
    EXPECT_EQ(error, std::string());
    EXPECT_EQ(readFile(target), std::string("[s]\nk = v\n"));
}

TEST_CASE(OutputFile, ReportsAFolderThatCannotBeCreated)
{
    TempFolder temp;
    const fs::path blocker = temp.path / "blocker";
    {
        std::ofstream file(blocker);
        file << "a file, not a folder";
    }
    std::string error;
    EXPECT_FALSE(writeTextFile((blocker / "report.txt").string(), "x", error));
    EXPECT_NE(error, std::string());
}
