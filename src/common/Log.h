// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Log.h
 * @brief   Lines from any thread, the real-time one included, to a file.
 *
 * The design's rule: inside the audio cycle nothing allocates, takes a lock
 * or touches the disk, so the log goes through a lock-free queue to a
 * thread of its own. A line is a fixed-size record: the engine's thread
 * formats it on its stack and pushes it, and never waits; when the queue is
 * full the line is counted as lost instead, and the writer says how many.
 *
 * The writer stamps each line with the wall-clock time it was pushed (from
 * the steady clock, so a clock change on the PC does not reorder lines) and
 * hands it to a LogSink: a file that starts over past a size, keeping the
 * previous one, or anything a test wants.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(__GNUC__) || defined(__clang__)
#define AJADUPLEX_PRINTF(formatIndex, firstArgument) __attribute__((format(printf, formatIndex, firstArgument)))
#else
#define AJADUPLEX_PRINTF(formatIndex, firstArgument)
#endif

namespace ajaduplex
{

/// What code that logs needs: one line at a time, from any thread.
class LogTarget
{
public:
    virtual ~LogTarget() = default;

    /// Never blocks and never allocates: safe on the real-time thread.
    virtual void line(const char* text) = 0;

    /// printf into a buffer on the stack, then line(). Also real-time safe.
    void linef(const char* format, ...) AJADUPLEX_PRINTF(2, 3);
};

struct LogRecord
{
    static constexpr std::size_t kTextBytes = 400;
    std::int64_t steadyNs = 0;
    char text[kTextBytes] = {};
};

// MSVC warns that the counters' alignment pads the class (C4324): that is
// the point. The warning is given where the class ends, so it is off for
// all of it.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

/// Bounded multi-producer queue of LogRecords (Vyukov's): push and pop
/// never block; push fails when full.
class LogQueue
{
public:
    /// `capacity` is rounded up to a power of two.
    explicit LogQueue(std::size_t capacity);

    bool push(std::int64_t steadyNs, const char* text);
    bool pop(LogRecord& record);
    std::size_t capacity() const { return mask_ + 1; }

private:
    struct Cell
    {
        std::atomic<std::size_t> sequence{0};
        LogRecord record;
    };

    std::unique_ptr<Cell[]> cells_;
    std::size_t mask_ = 0;
    // Each counter on a cache line of its own, so writers and the reader do
    // not slow each other down.
    alignas(64) std::atomic<std::size_t> enqueue_{0};
    alignas(64) std::atomic<std::size_t> dequeue_{0};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

class LogSink
{
public:
    virtual ~LogSink() = default;
    virtual void write(const std::string& line) = 0;
    virtual void flush() {}
};

/// A text file, appended to. Past `maxBytes` it is renamed to <name>.old
/// (replacing the previous one) and a new one is started.
class FileLogSink final : public LogSink
{
public:
    FileLogSink(std::filesystem::path path, std::uintmax_t maxBytes);

    bool open() const { return stream_.is_open(); }
    const std::filesystem::path& path() const { return path_; }

    void write(const std::string& line) override;
    void flush() override;

private:
    void openFile();

    std::filesystem::path path_;
    std::uintmax_t maxBytes_;
    std::uintmax_t bytes_ = 0;
    std::ofstream stream_;
};

/// The log: a queue, a sink, and the thread that moves lines from one to
/// the other.
class Log final : public LogTarget
{
public:
    explicit Log(std::unique_ptr<LogSink> sink, std::size_t capacity = 1024);
    ~Log() override;
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void line(const char* text) override;

    /// Waits until every line pushed before the call is written. Not from
    /// the real-time thread.
    void flush();

    std::uint64_t lost() const { return lost_.load(std::memory_order_relaxed); }

    /// "2026-11-08 20:31:05.123" for a steady-clock time, in local time.
    std::string wallTime(std::int64_t steadyNs) const;

private:
    void writer();
    bool drain();

    LogQueue queue_;
    std::unique_ptr<LogSink> sink_;
    std::int64_t steadyOriginNs_ = 0;
    std::int64_t wallOriginMs_ = 0;
    std::atomic<std::uint64_t> lost_{0};
    std::uint64_t lostReported_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable drained_;
    bool stopping_ = false;
    std::uint64_t flushRequests_ = 0;
    std::uint64_t flushesDone_ = 0;
    std::thread thread_;
};

} // namespace ajaduplex
