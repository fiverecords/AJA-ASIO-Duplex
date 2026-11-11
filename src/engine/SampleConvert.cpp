// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "engine/SampleConvert.h"

namespace ajaduplex::engine
{

void deinterleave(const std::int32_t* interleaved, unsigned channels, std::size_t frames,
                  std::int32_t* const* perChannel)
{
    for (unsigned c = 0; c < channels; ++c)
    {
        std::int32_t* out = perChannel[c];
        if (out == nullptr)
            continue;
        const std::int32_t* in = interleaved + c;
        for (std::size_t f = 0; f < frames; ++f)
            out[f] = in[f * channels];
    }
}

void interleave(const std::int32_t* const* perChannel, unsigned channels, std::size_t frames,
                std::int32_t* interleaved)
{
    for (unsigned c = 0; c < channels; ++c)
    {
        const std::int32_t* in = perChannel[c];
        std::int32_t* out = interleaved + c;
        if (in == nullptr)
        {
            for (std::size_t f = 0; f < frames; ++f)
                out[f * channels] = 0;
            continue;
        }
        for (std::size_t f = 0; f < frames; ++f)
            out[f * channels] = in[f];
    }
}

} // namespace ajaduplex::engine
