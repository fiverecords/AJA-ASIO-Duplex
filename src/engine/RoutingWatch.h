// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    RoutingWatch.h
 * @brief   The design's coexistence rule: the routing set up is read back.
 *
 * Whoever writes last wins the card's shared settings (the AES outputs, the
 * monitor, the Xpand's analog/AES input select and analog level), and
 * anyone who sets up our audio system can change its input source: the AJA
 * Control Panel, AJA's services in standard task mode, AJA's own audio
 * driver, any program on the SDK. RoutingWatch takes the routing as it
 * stands right after the engine started as ours, for the parts it is told
 * are the driver's; each check() reads it again and says what changed since
 * the last check and what is not ours now, and, when asked to keep it,
 * writes ours back. The other parts are only reported: the analog level
 * when the driver was told to leave it as the card has it, the task mode
 * unless the driver holds the card in OEM mode. A task mode the driver
 * holds is always put back, and with it whatever else is not ours then:
 * with the card in standard mode, even for a moment, AJA's services move
 * the shared routing back to their own (Q9: the AES outputs and the monitor
 * to the mixer, within a second), and the mode alone would leave the
 * driver's outputs there. The same goes for the first looks after a start
 * at which the driver itself took the card out of standard mode.
 *
 * It only uses CardAudio, so it runs on the engine's thread: the driver
 * calls check() once a second, after a block's output went out, and the
 * check allocates nothing. The text helpers write into the caller's buffer
 * for the same reason, except describeRouting(), for the log at start.
 */

#pragma once

#include "device/CardAudio.h"

#include <cstddef>
#include <string>

namespace ajaduplex::engine
{

class RoutingWatch
{
public:
    explicit RoutingWatch(device::CardAudio& card) : card_(card) {}

    /// Reads the routing as it stands now and takes the `owned` parts of it
    /// as ours (only writable parts can be); of those, the `alwaysKept` are
    /// put back whether check() is asked to keep or not (the task mode the
    /// driver sets: the design reasserts it). False if none of it can be
    /// read: check() then reads nothing.
    ///
    /// `tookFromServices`: the task mode always kept is one the driver has
    /// just set over standard mode. AJA's services, in the middle of a pass
    /// when the mode changed, may still write the shared routing after the
    /// driver did: the first looks put back everything that is not ours,
    /// as after any change of the task mode.
    bool takeBaseline(unsigned owned = device::routing::kRouting, unsigned alwaysKept = 0,
                      bool tookFromServices = false);
    bool active() const { return active_; }
    const device::CardRouting& baseline() const { return baseline_; }
    /// The parts that are ours: kept with keep, and "changed by another
    /// program" when they change; the rest are only reported.
    unsigned owned() const { return owned_; }
    /// The last check's reading, after what it put back.
    const device::CardRouting& last() const { return last_; }

    /// The driver itself sets `parts` to what `wanted` says (a setting
    /// changed while it plays): they are written, read back, and ours from
    /// now on. True if the card took them.
    bool change(const device::CardRouting& wanted, unsigned parts);

    /// `parts` are no longer ours: they stay as they are, only reported.
    void release(unsigned parts) { owned_ &= ~parts; }

    struct Check
    {
        bool read = false;            ///< the card could be read
        unsigned changed = 0;         ///< parts that changed since the last check, the task mode included
        unsigned notOurs = 0;         ///< writable parts that were not ours when read
        unsigned putBack = 0;         ///< those written back (all of them, when keeping)
        unsigned putBackFailed = 0;   ///< of those, the ones still not ours after
        device::CardRouting before;   ///< the last check's reading, after what it put back
        device::CardRouting now;      ///< this check's, before anything was put back
    };

    /// One look: reads the routing and compares it with ours and with the
    /// last look. With `keep` (and for the parts always kept), writes ours
    /// back over what is not ours, then reads again to see that it took.
    /// When the task mode is one of the parts always kept and is found
    /// changed, everything of ours that is not ours at this look and at the
    /// next goes back too, with or without `keep`: AJA's services had the
    /// card meanwhile.
    Check check(bool keep);

    /// Looks, this one included, at which everything goes back after the
    /// task mode was found changed: the next one too, for what the services
    /// wrote while this one was putting the mode back. As many after a
    /// start that took the card from the services.
    static constexpr unsigned kChecksAfterModeChange = 2;

private:
    device::CardAudio& card_;
    device::CardRouting baseline_;
    device::CardRouting last_;
    unsigned owned_ = 0;
    unsigned alwaysKept_ = 0;
    unsigned afterModeChange_ = 0;   ///< looks left at which everything goes back
    bool active_ = false;
};

/// The name of one part (one bit of device::routing): "input source",
/// "Xpand input select", "Xpand analog level", "AES out 1-4", "monitor",
/// "task mode".
const char* routingPartName(unsigned part);

/// One part's value as `routing` has it, into `buffer` (always ended):
/// "analog", "1 (analog)", "+24 dBu", "audio system 2, channels 1-4",
/// "standard", or "unreadable" when `routing` does not have it. Returns
/// `buffer`.
const char* describeRoutingPart(const device::CardRouting& routing, unsigned part, char* buffer, std::size_t size);

/// Every part `routing` has, "input source: analog; Xpand input select: 1
/// (analog); AES out 1-4: audio system 2, channels 1-4; ...".
std::string describeRouting(const device::CardRouting& routing);

/// The names of the parts in `parts`: "AES out 1-4, monitor".
std::string routingPartNames(unsigned parts);

/// What a check of `watch` found, as lines for a log, one per part that
/// changed or was put back: "routing: another program changed AES out 1-4
/// to audio system 1, channels 1-4; the driver's is audio system 2,
/// channels 1-4; put back", "... is the driver's again", and for a part
/// that is not the driver's "routing: Xpand analog level is now +18 dBu (it
/// was +24 dBu)" ("the card's task mode is now OEM ..."). Each goes to
/// `emit` with `context`. `keepName` names what puts the routing back, for
/// "left as it is (<keepName> is off)". Returns how many of the driver's
/// parts another program changed. Formats on the stack: it allocates
/// nothing, for the engine's thread.
unsigned reportCheck(const RoutingWatch& watch, const RoutingWatch::Check& check, const char* keepName,
                     void (*emit)(void* context, const char* line), void* context);

} // namespace ajaduplex::engine
