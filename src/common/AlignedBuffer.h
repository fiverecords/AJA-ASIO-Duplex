// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AlignedBuffer.h
 * @brief   Zero-filled, page-aligned host memory for DMA.
 *
 * AJA's driver locks and maps host buffers page by page, and its demos use
 * page-aligned buffers; every DMA buffer here is one of these.
 */

#pragma once

#include <cstddef>
#include <cstring>
#include <new>

namespace ajaduplex
{

class AlignedBuffer
{
public:
    static constexpr std::size_t kPage = 4096;

    explicit AlignedBuffer(std::size_t bytes, std::size_t alignment = kPage)
        : size_(bytes), alignment_(alignment)
    {
        data_ = ::operator new(bytes == 0 ? 1 : bytes, std::align_val_t(alignment));
        std::memset(data_, 0, bytes);
    }

    ~AlignedBuffer() { ::operator delete(data_, std::align_val_t(alignment_)); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    void* data() { return data_; }
    const void* data() const { return data_; }
    std::size_t size() const { return size_; }

    template <typename T>
    T* as() { return static_cast<T*>(data_); }
    template <typename T>
    const T* as() const { return static_cast<const T*>(data_); }

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = kPage;
};

} // namespace ajaduplex
