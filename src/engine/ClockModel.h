// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    ClockModel.h
 * @brief   The card's sample clock against the host clock.
 *
 * The card's audio clock follows its video reference, not the PC. The
 * engine feeds this model with pairs (host time, frames the card has
 * captured) seen while polling, and asks it when a given frame will be
 * there, so it can sleep until shortly before and poll only the last
 * stretch.
 *
 * A least-squares line over the most recent observations; until there are
 * enough, the nominal rate. The observations may be old: the engine checks
 * each answer against the head it has just read, and resets a model that
 * disagrees with it (AudioEngine::waitForBlock).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ajaduplex::engine
{

class ClockModel
{
public:
    explicit ClockModel(double nominalRate = 48000.0, std::size_t window = 256);

    void reset();

    /// The card had captured `frames` frames (since start) at `hostNs`.
    void observe(std::int64_t hostNs, std::int64_t frames);

    std::size_t observations() const { return count_; }

    /// Enough observations, spread over enough time, to trust the fit.
    bool ready() const;

    /// Fitted rate in frames per second of the host clock (nominal until ready).
    double framesPerSecond() const;

    /// framesPerSecond against the nominal rate, in parts per million.
    double ppm() const;

    /// Host time at which the card will have captured `frames` frames.
    /// Before the first observation, returns 0.
    std::int64_t hostTimeOf(std::int64_t frames) const;

private:
    void refit() const;

    double nominalRate_;
    std::size_t window_;
    std::vector<std::int64_t> hostNs_;
    std::vector<std::int64_t> frames_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;

    mutable bool fitValid_ = false;
    mutable double rate_ = 0.0;        ///< frames per ns
    mutable double meanHost_ = 0.0;    ///< relative to baseHost_
    mutable double meanFrames_ = 0.0;  ///< relative to baseFrames_
    mutable std::int64_t baseHost_ = 0;
    mutable std::int64_t baseFrames_ = 0;
};

} // namespace ajaduplex::engine
