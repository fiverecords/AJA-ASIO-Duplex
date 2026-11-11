// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "common/Log.h"

#include "common/TextFormat.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ajaduplex
{

namespace
{

std::int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t wallNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ---------------------------------------------------------------------------
// LogTarget
// ---------------------------------------------------------------------------

void LogTarget::linef(const char* format, ...)
{
    char buffer[LogRecord::kTextBytes];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    line(buffer);
}

// ---------------------------------------------------------------------------
// LogQueue
// ---------------------------------------------------------------------------

LogQueue::LogQueue(std::size_t capacity)
{
    std::size_t size = 2;
    while (size < capacity)
        size <<= 1;
    cells_ = std::make_unique<Cell[]>(size);
    mask_ = size - 1;
    for (std::size_t i = 0; i < size; ++i)
        cells_[i].sequence.store(i, std::memory_order_relaxed);
}

bool LogQueue::push(std::int64_t steadyNs, const char* text)
{
    std::size_t position = enqueue_.load(std::memory_order_relaxed);
    for (;;)
    {
        Cell& cell = cells_[position & mask_];
        const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::ptrdiff_t>(sequence - position);
        if (difference == 0)
        {
            if (enqueue_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
            {
                cell.record.steadyNs = steadyNs;
                std::size_t n = 0;
                if (text != nullptr)
                {
                    while (n < LogRecord::kTextBytes - 1 && text[n] != '\0')
                    {
                        cell.record.text[n] = text[n];
                        ++n;
                    }
                }
                cell.record.text[n] = '\0';
                cell.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        }
        else if (difference < 0)
        {
            return false;   // full
        }
        else
        {
            position = enqueue_.load(std::memory_order_relaxed);
        }
    }
}

bool LogQueue::pop(LogRecord& record)
{
    std::size_t position = dequeue_.load(std::memory_order_relaxed);
    for (;;)
    {
        Cell& cell = cells_[position & mask_];
        const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::ptrdiff_t>(sequence - (position + 1));
        if (difference == 0)
        {
            if (dequeue_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
            {
                record = cell.record;
                cell.sequence.store(position + mask_ + 1, std::memory_order_release);
                return true;
            }
        }
        else if (difference < 0)
        {
            return false;   // empty
        }
        else
        {
            position = dequeue_.load(std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// FileLogSink
// ---------------------------------------------------------------------------

FileLogSink::FileLogSink(std::filesystem::path path, std::uintmax_t maxBytes)
    : path_(std::move(path)), maxBytes_(maxBytes)
{
    openFile();
}

void FileLogSink::openFile()
{
    std::error_code error;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), error);
    bytes_ = std::filesystem::exists(path_, error) ? std::filesystem::file_size(path_, error) : 0;
    if (error)
        bytes_ = 0;
    stream_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
}

void FileLogSink::write(const std::string& line)
{
    if (!stream_.is_open())
        return;
    if (bytes_ > 0 && bytes_ + line.size() + 2 > maxBytes_)
    {
        // Start over, keeping the last file as <name>.old.
        stream_.close();
        std::filesystem::path old = path_;
        old += ".old";
        std::error_code error;
        std::filesystem::remove(old, error);
        std::filesystem::rename(path_, old, error);
        openFile();
        if (!stream_.is_open())
            return;
    }
    stream_ << line << "\r\n";
    bytes_ += line.size() + 2;
}

void FileLogSink::flush()
{
    if (stream_.is_open())
        stream_.flush();
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

Log::Log(std::unique_ptr<LogSink> sink, std::size_t capacity)
    : queue_(capacity), sink_(std::move(sink)), steadyOriginNs_(steadyNowNs()), wallOriginMs_(wallNowMs())
{
    thread_ = std::thread([this] { writer(); });
}

Log::~Log()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void Log::line(const char* text)
{
    if (!queue_.push(steadyNowNs(), text))
        lost_.fetch_add(1, std::memory_order_relaxed);
}

void Log::flush()
{
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t ticket = ++flushRequests_;
    wake_.notify_all();
    drained_.wait(lock, [&] { return flushesDone_ >= ticket || !thread_.joinable(); });
}

std::string Log::wallTime(std::int64_t steadyNs) const
{
    const std::int64_t wallMs = wallOriginMs_ + (steadyNs - steadyOriginNs_) / 1000000;
    const std::tm when = text::localTime(static_cast<std::time_t>(wallMs / 1000));
    char millis[8];
    std::snprintf(millis, sizeof(millis), ".%03d", static_cast<int>(((wallMs % 1000) + 1000) % 1000));
    return text::timestampForDisplay(when) + millis;
}

bool Log::drain()
{
    if (!sink_)
        return false;
    LogRecord record;
    bool wrote = false;
    while (queue_.pop(record))
    {
        sink_->write(wallTime(record.steadyNs) + "  " + record.text);
        wrote = true;
    }
    const std::uint64_t lost = lost_.load(std::memory_order_relaxed);
    if (lost != lostReported_)
    {
        sink_->write(wallTime(steadyNowNs()) + "  [" + std::to_string(lost - lostReported_) +
                     " lines lost: the log fell behind]");
        lostReported_ = lost;
        wrote = true;
    }
    return wrote;
}

void Log::writer()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        wake_.wait_for(lock, std::chrono::milliseconds(50),
                       [&] { return stopping_ || flushRequests_ > flushesDone_; });
        const bool stop = stopping_;
        const std::uint64_t requests = flushRequests_;
        lock.unlock();
        const bool wrote = drain();
        if (sink_ && (wrote || requests > flushesDone_))
            sink_->flush();
        lock.lock();
        flushesDone_ = requests;
        drained_.notify_all();
        if (stop)
            return;
    }
}

} // namespace ajaduplex
