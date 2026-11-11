// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "test_framework.h"

#include "common/Log.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

using namespace ajaduplex;

namespace
{

/// What a MemorySink wrote, kept apart from it: the log owns its sink and
/// destroys it, and the test still reads afterwards.
struct Written
{
    std::vector<std::string> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return lines;
    }

    std::mutex mutex;
    std::vector<std::string> lines;
};

class MemorySink final : public LogSink
{
public:
    explicit MemorySink(std::shared_ptr<Written> written) : written_(std::move(written)) {}

    void write(const std::string& line) override
    {
        std::lock_guard<std::mutex> lock(written_->mutex);
        written_->lines.push_back(line);
    }

private:
    std::shared_ptr<Written> written_;
};

std::filesystem::path tempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE(LogQueue, KeepsOrderAndRefusesWhenFull)
{
    LogQueue queue(4);
    EXPECT_EQ(queue.capacity(), std::size_t(4));
    EXPECT_TRUE(queue.push(1, "one"));
    EXPECT_TRUE(queue.push(2, "two"));
    EXPECT_TRUE(queue.push(3, "three"));
    EXPECT_TRUE(queue.push(4, "four"));
    EXPECT_FALSE(queue.push(5, "five"));   // full: refused, never waits
    LogRecord record;
    ASSERT_TRUE(queue.pop(record));
    EXPECT_EQ(std::string(record.text), std::string("one"));
    EXPECT_EQ(record.steadyNs, std::int64_t(1));
    EXPECT_TRUE(queue.push(6, "six"));
    std::vector<std::string> rest;
    while (queue.pop(record))
        rest.push_back(record.text);
    ASSERT_EQ(rest.size(), std::size_t(4));
    EXPECT_EQ(rest[0], std::string("two"));
    EXPECT_EQ(rest[3], std::string("six"));
    EXPECT_FALSE(queue.pop(record));
}

TEST_CASE(LogQueue, LongTextIsCut)
{
    LogQueue queue(2);
    const std::string longText(1000, 'x');
    ASSERT_TRUE(queue.push(0, longText.c_str()));
    LogRecord record;
    ASSERT_TRUE(queue.pop(record));
    EXPECT_EQ(std::string(record.text).size(), LogRecord::kTextBytes - 1);
}

TEST_CASE(LogQueue, ManyProducersLoseNothingThatFits)
{
    LogQueue queue(1 << 15);
    constexpr int kThreads = 4;
    constexpr int kEach = 5000;
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([&queue, t] {
            char text[32];
            for (int i = 0; i < kEach; ++i)
            {
                std::snprintf(text, sizeof(text), "%d:%d", t, i);
                while (!queue.push(i, text))
                    std::this_thread::yield();
            }
        });
    }
    std::set<std::string> seen;
    LogRecord record;
    int popped = 0;
    while (popped < kThreads * kEach)
    {
        if (queue.pop(record))
        {
            seen.insert(record.text);
            ++popped;
        }
    }
    for (std::thread& producer : producers)
        producer.join();
    EXPECT_EQ(seen.size(), std::size_t(kThreads * kEach));
}

TEST_CASE(Log, LinesReachTheSinkWithTheirTime)
{
    auto memory = std::make_shared<Written>();
    {
        Log log(std::make_unique<MemorySink>(memory));
        log.line("hello");
        log.linef("%d late outputs at %.1f s", 3, 2.5);
        log.flush();
        const std::vector<std::string> lines = memory->snapshot();
        ASSERT_EQ(lines.size(), std::size_t(2));
        // "YYYY-MM-DD HH:MM:SS.mmm  text"
        EXPECT_EQ(lines[0].size(), std::size_t(23 + 2 + 5));
        EXPECT_EQ(lines[0][4], '-');
        EXPECT_EQ(lines[0][19], '.');
        EXPECT_EQ(lines[0].substr(25), std::string("hello"));
        EXPECT_EQ(lines[1].substr(25), std::string("3 late outputs at 2.5 s"));
        log.line("last");
    }
    // Destroying the log writes what was left.
    const std::vector<std::string> lines = memory->snapshot();
    ASSERT_EQ(lines.size(), std::size_t(3));
    EXPECT_EQ(lines[2].substr(25), std::string("last"));
}

TEST_CASE(Log, SaysHowManyLinesItLost)
{
    auto memory = std::make_shared<Written>();
    Log log(std::make_unique<MemorySink>(memory), 4);
    for (int i = 0; i < 100; ++i)
        log.line("burst");   // far faster than the writer's 50 ms
    log.flush();
    EXPECT_TRUE(log.lost() > 0);
    bool said = false;
    for (const std::string& line : memory->snapshot())
        said = said || line.find("lines lost: the log fell behind") != std::string::npos;
    EXPECT_TRUE(said);
}

TEST_CASE(FileLogSink, AppendsAndStartsOverPastItsSize)
{
    const std::filesystem::path path = tempPath("ajaduplex-test-log.txt");
    std::filesystem::path old = path;
    old += ".old";
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(old, error);
    {
        FileLogSink sink(path, 100);
        ASSERT_TRUE(sink.open());
        for (int i = 0; i < 10; ++i)
            sink.write("0123456789012345678");   // 21 bytes a line with the CR LF
        sink.flush();
    }
    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_TRUE(std::filesystem::exists(old));
    EXPECT_TRUE(std::filesystem::file_size(path) <= 100);
    EXPECT_TRUE(std::filesystem::file_size(old) <= 100);
    {
        // Opened again, it appends.
        const std::uintmax_t before = std::filesystem::file_size(path);
        FileLogSink sink(path, 1000);
        sink.write("more");
        sink.flush();
        EXPECT_EQ(std::filesystem::file_size(path), before + 6);
    }
    std::filesystem::remove(path, error);
    std::filesystem::remove(old, error);
}

TEST_CASE(FileLogSink, MakesItsFolder)
{
    const std::filesystem::path folder = tempPath("ajaduplex-test-log-folder");
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    {
        FileLogSink sink(folder / "inner" / "driver.log", 1000);
        ASSERT_TRUE(sink.open());
        sink.write("x");
    }
    EXPECT_TRUE(std::filesystem::exists(folder / "inner" / "driver.log"));
    std::filesystem::remove_all(folder, error);
}
