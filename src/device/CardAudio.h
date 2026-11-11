// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CardAudio.h
 * @brief   One audio system of one card, as the engine and the probe see it.
 *
 * This is the boundary of the design: everything above it (the engine, the
 * probe's measurements, later the ASIO driver) talks to this interface, and
 * only its libajantv2 implementation (Ntv2CardAudio) talks to the SDK. The
 * simulated card (sim::FakeCardAudio) implements it too, so all of that
 * code runs and is tested without a card.
 *
 * The header uses no SDK types on purpose.
 *
 * Positions and offsets are bytes within a ring. Both rings of an audio
 * system (playback, then capture) wrap at the same address, ringBytes, a
 * little below the end of each 4 MB buffer. A frame is one sample of every
 * channel: 4 bytes per channel, little-endian signed 32-bit with the audio
 * in the upper bits (ASIOSTInt32LSB).
 */

#pragma once

#include <cstdint>
#include <string>

namespace ajaduplex::device
{

enum class InputSource
{
    Unchanged,   ///< leave the audio system's input source as it is
    Aes,         ///< Xpand AES inputs
    Analog,      ///< Xpand analog inputs
    Embedded,    ///< SDI embedded audio
    Hdmi,        ///< HDMI embedded audio
    Other,       ///< read back only: a source this code does not set (the card's microphone input, say)
};

/// Who sets the card up besides the programs that open it (NTV2TaskMode).
/// In Standard, AJA's services apply what the AJA Control Panel says; in
/// OEM, the programs do. The driver sets OEM while it plays only when told
/// to (whether it must is Q9).
enum class TaskMode
{
    Unknown,
    Disabled,
    Standard,
    Oem,
};

enum class Ring
{
    Playback,
    Capture,
};

enum class CardEvent
{
    Audio,          ///< eAudio
    AudioInWrap,    ///< eAudioInWrap
    AudioOutWrap,   ///< eAudioOutWrap
};

const char* toString(InputSource source);
const char* toString(CardEvent event);
const char* toString(TaskMode mode);

/// Where a group of the card's outputs takes its audio from.
struct OutputSource
{
    unsigned audioSystem = 0;    ///< 1-based
    unsigned firstChannel = 0;   ///< 1-based, of that audio system: AES out 5-8 playing its channels 5-8 is 5
};

inline bool operator==(OutputSource a, OutputSource b)
{
    return a.audioSystem == b.audioSystem && a.firstChannel == b.firstChannel;
}

inline bool operator!=(OutputSource a, OutputSource b)
{
    return !(a == b);
}

/// What an SDI output's audio embedder plays: all the channels of one audio
/// system, embedded in whatever video the output carries.
struct SdiOutputAudio
{
    unsigned audioSystem = 0;   ///< 1-based
    bool enabled = false;       ///< false: the output carries no audio at all
};

inline bool operator==(SdiOutputAudio a, SdiOutputAudio b)
{
    return a.audioSystem == b.audioSystem && a.enabled == b.enabled;
}

inline bool operator!=(SdiOutputAudio a, SdiOutputAudio b)
{
    return !(a == b);
}

/// What the HDMI output's audio plays: 8 channels of an audio system, from
/// channel 1 or 9, or one pair of them.
struct HdmiOutputAudio
{
    unsigned audioSystem = 0;    ///< 1-based
    unsigned channels = 0;       ///< 8 or 2
    unsigned firstChannel = 0;   ///< 1-based: 1 or 9 with 8 channels; the pair's first with 2
};

inline bool operator==(HdmiOutputAudio a, HdmiOutputAudio b)
{
    return a.audioSystem == b.audioSystem && a.channels == b.channels && a.firstChannel == b.firstChannel;
}

inline bool operator!=(HdmiOutputAudio a, HdmiOutputAudio b)
{
    return !(a == b);
}

/// What configure() sets up that other programs can change while it runs:
/// the audio system's input source, and settings the whole card shares
/// (the Xpand's analog/AES input select and analog level, the AES outputs,
/// the monitor, the SDI and HDMI outputs' audio), for which whoever wrote
/// last wins. The driver reads it back once a second (the design's
/// coexistence rules, engine/RoutingWatch.h). Each part is there only if
/// its `have` flag says so: the card has it and it was read. The SDI and
/// HDMI outputs are read only once this program has set them: video
/// programs change them all the time, and they are nobody else's business.
struct CardRouting
{
    static constexpr unsigned kMaxAesGroups = 4;
    static constexpr unsigned kMaxSdiOutputs = 8;

    bool haveSource = false;
    InputSource source = InputSource::Unchanged;   ///< this audio system's input
    unsigned sourceCode = 0;                       ///< for Other: the card's own code for it; 0 otherwise
    unsigned embeddedInput = 0;                    ///< for Embedded: 0-based video input; 0 otherwise

    bool haveXpandSelect = false;
    unsigned xpandSelect = 0;                      ///< raw, 4 bits: 0 AES, 1 analog (the rest is Q14)

    bool haveAnalogLevel = false;
    int analogLevelDbu = 0;                        ///< what 0 dBFS is: +24, +18, +15 or +12 dBu; 0: a value without a name
    unsigned analogLevelCode = 0;                  ///< for that one, the card's own value; 0 otherwise

    unsigned aesGroups = 0;                        ///< AES outputs read, in groups of four: 2 on the KONA X
    OutputSource aes[kMaxAesGroups];               ///< AES out 1-4, 5-8, ...

    bool haveMonitor = false;
    OutputSource monitor;                          ///< the pair the monitor plays (the analog out too, if Q4 says so)

    unsigned sdiOutputs = 0;                       ///< bits: the SDI outputs read; bit n is SDI out n + 1
    SdiOutputAudio sdiOut[kMaxSdiOutputs];

    bool haveHdmiOut = false;
    HdmiOutputAudio hdmiOut;

    bool haveTaskMode = false;
    TaskMode taskMode = TaskMode::Unknown;         ///< written only when the driver is told to set it
};

/// The parts of a CardRouting, as bits, in the order they are described.
namespace routing
{
constexpr unsigned kSource = 1u << 0;
constexpr unsigned kXpandSelect = 1u << 1;
constexpr unsigned kAnalogLevel = 1u << 2;
constexpr unsigned kAesGroup = 1u << 3;            ///< AES out 1-4; group g is kAesGroup << g
constexpr unsigned kAesGroups = 0xFu << 3;
constexpr unsigned kMonitor = 1u << 7;
constexpr unsigned kSdiOut = 1u << 8;              ///< SDI out 1; SDI out n + 1 is kSdiOut << n
constexpr unsigned kSdiOuts = 0xFFu << 8;
constexpr unsigned kHdmiOut = 1u << 16;
constexpr unsigned kTaskMode = 1u << 17;
constexpr unsigned kLast = kTaskMode;              ///< for loops over the parts, in order
/// What the driver sets up as a rule: the parts it owns unless told otherwise.
constexpr unsigned kRouting = kSource | kXpandSelect | kAnalogLevel | kAesGroups | kMonitor | kSdiOuts | kHdmiOut;
constexpr unsigned kWritable = kRouting | kTaskMode;
constexpr unsigned kAll = kWritable;
} // namespace routing

/// The parts `routing` has.
inline unsigned routingParts(const CardRouting& r)
{
    unsigned parts = 0;
    if (r.haveSource)
        parts |= routing::kSource;
    if (r.haveXpandSelect)
        parts |= routing::kXpandSelect;
    if (r.haveAnalogLevel)
        parts |= routing::kAnalogLevel;
    for (unsigned g = 0; g < r.aesGroups && g < CardRouting::kMaxAesGroups; ++g)
        parts |= routing::kAesGroup << g;
    if (r.haveMonitor)
        parts |= routing::kMonitor;
    for (unsigned n = 0; n < CardRouting::kMaxSdiOutputs; ++n)
    {
        if ((r.sdiOutputs & (1u << n)) != 0)
            parts |= routing::kSdiOut << n;
    }
    if (r.haveHdmiOut)
        parts |= routing::kHdmiOut;
    if (r.haveTaskMode)
        parts |= routing::kTaskMode;
    return parts;
}

/// The parts `a` and `b` disagree on; a part only one of them has counts.
inline unsigned routingDifferences(const CardRouting& a, const CardRouting& b)
{
    const unsigned inA = routingParts(a);
    const unsigned inB = routingParts(b);
    const unsigned both = inA & inB;
    unsigned differ = inA ^ inB;
    if ((both & routing::kSource) != 0 &&
        (a.source != b.source || a.sourceCode != b.sourceCode || a.embeddedInput != b.embeddedInput))
        differ |= routing::kSource;
    if ((both & routing::kXpandSelect) != 0 && a.xpandSelect != b.xpandSelect)
        differ |= routing::kXpandSelect;
    if ((both & routing::kAnalogLevel) != 0 &&
        (a.analogLevelDbu != b.analogLevelDbu || a.analogLevelCode != b.analogLevelCode))
        differ |= routing::kAnalogLevel;
    for (unsigned g = 0; g < CardRouting::kMaxAesGroups; ++g)
    {
        const unsigned bit = routing::kAesGroup << g;
        if ((both & bit) != 0 && a.aes[g] != b.aes[g])
            differ |= bit;
    }
    if ((both & routing::kMonitor) != 0 && a.monitor != b.monitor)
        differ |= routing::kMonitor;
    for (unsigned n = 0; n < CardRouting::kMaxSdiOutputs; ++n)
    {
        const unsigned bit = routing::kSdiOut << n;
        if ((both & bit) != 0 && a.sdiOut[n] != b.sdiOut[n])
            differ |= bit;
    }
    if ((both & routing::kHdmiOut) != 0 && a.hdmiOut != b.hdmiOut)
        differ |= routing::kHdmiOut;
    if ((both & routing::kTaskMode) != 0 && a.taskMode != b.taskMode)
        differ |= routing::kTaskMode;
    return differ;
}

/// Copies the `parts` of `from` into `to`, whether `from` has them or not.
inline void copyRoutingParts(CardRouting& to, const CardRouting& from, unsigned parts)
{
    if ((parts & routing::kSource) != 0)
    {
        to.haveSource = from.haveSource;
        to.source = from.source;
        to.sourceCode = from.sourceCode;
        to.embeddedInput = from.embeddedInput;
    }
    if ((parts & routing::kXpandSelect) != 0)
    {
        to.haveXpandSelect = from.haveXpandSelect;
        to.xpandSelect = from.xpandSelect;
    }
    if ((parts & routing::kAnalogLevel) != 0)
    {
        to.haveAnalogLevel = from.haveAnalogLevel;
        to.analogLevelDbu = from.analogLevelDbu;
        to.analogLevelCode = from.analogLevelCode;
    }
    if ((parts & routing::kAesGroups) != 0)
    {
        to.aesGroups = from.aesGroups;
        for (unsigned g = 0; g < CardRouting::kMaxAesGroups; ++g)
        {
            if ((parts & (routing::kAesGroup << g)) != 0)
                to.aes[g] = from.aes[g];
        }
    }
    if ((parts & routing::kMonitor) != 0)
    {
        to.haveMonitor = from.haveMonitor;
        to.monitor = from.monitor;
    }
    for (unsigned n = 0; n < CardRouting::kMaxSdiOutputs; ++n)
    {
        if ((parts & (routing::kSdiOut << n)) == 0)
            continue;
        to.sdiOutputs = (to.sdiOutputs & ~(1u << n)) | (from.sdiOutputs & (1u << n));
        to.sdiOut[n] = from.sdiOut[n];
    }
    if ((parts & routing::kHdmiOut) != 0)
    {
        to.haveHdmiOut = from.haveHdmiOut;
        to.hdmiOut = from.hdmiOut;
    }
    if ((parts & routing::kTaskMode) != 0)
    {
        to.haveTaskMode = from.haveTaskMode;
        to.taskMode = from.taskMode;
    }
}

struct AudioConfig
{
    unsigned channels = 16;
    InputSource source = InputSource::Aes;
    unsigned embeddedInput = 0;          ///< for InputSource::Embedded: 0 = SDI 1
    bool captureEnable = true;           ///< SetAudioCaptureEnable
    bool eraseMode = true;               ///< card zeroes what it has played
    /// Setting any input source rewrites the Xpand's analog/AES select, which
    /// is shared by every audio system. When true, the select is put back the
    /// way it was right after, so other programs' inputs do not change; the
    /// input then hears whatever the select says. When false, the select
    /// follows `source`, as anything that records the input needs.
    bool keepXpandSelect = true;
    /// Point AES out 1-8 at channels 1-8 of this audio system
    /// (SetAESOutputSource). After power-up they follow audio system 1. The
    /// AES outputs are shared by every audio system: restore() puts the old
    /// map back.
    bool routeAesOutputs = false;
    /// Point the monitor output at a pair of this audio system's channels
    /// (SetAudioOutputMonitorSource). The Xpand's analog output follows
    /// either AES 1-2 or the monitor (Q4): with both on channels 1-2 it
    /// plays them either way. Shared too: restore() puts it back.
    bool routeMonitor = false;
    unsigned monitorPair = 0;            ///< 0: channels 1-2, ..., 7: channels 15-16
    /// The Xpand's analog reference level, what 0 dBFS is in dBu: +24, +18,
    /// +15 or +12 (isAnalogLevel); 0 leaves it as it is. One setting for the
    /// whole card: restore() puts the old one back.
    int analogLevelDbu = 0;
    /// The card's task mode; Unknown leaves it as it is. One for the whole
    /// card: restore() puts the old one back, unless another program holds
    /// the card by then (it reserved it, as AJA Input does). An OEM mode
    /// found while another program held the card was that program's: with
    /// the card held no longer, restore() puts standard mode instead.
    /// configure() sets it before anything else, and when that takes the
    /// card out of standard mode, the rest waits until a pass AJA's
    /// services were in the middle of is over.
    TaskMode taskMode = TaskMode::Unknown;
    /// The SDI outputs whose embedder plays this audio system, all its
    /// channels (SetSDIOutputAudioSystem, SetSDIOutputAudioEnabled), as
    /// bits: 1 is SDI out 1, 2 is SDI out 2. The audio goes into whatever
    /// video the output carries: the video is never set here. Shared:
    /// restore() puts them back.
    unsigned sdiOutputs = 0;
    /// The HDMI output plays channels 1-8 of this audio system
    /// (SetHDMIOutAudioSource8Channel). Shared: restore() puts it back.
    bool hdmiOutput = false;
};

/// +24, +18, +15 or +12: the Xpand's analog reference levels.
inline bool isAnalogLevel(int dbu)
{
    return dbu == 24 || dbu == 18 || dbu == 15 || dbu == 12;
}

struct RingLayout
{
    std::uint32_t ringBytes = 0;       ///< wrap address: both rings wrap here
    std::uint32_t captureOffset = 0;   ///< capture ring start, from the audio system's base
    std::uint32_t frameBytes = 0;      ///< 4 x channels
    /// Each ring's room, from its start: the ring, then bytes past the wrap
    /// that the card never plays or fills (on the KONA X the 4 MB from one
    /// ring's start to the next: 16 KB past L). Equal to ringBytes when
    /// there are none.
    std::uint32_t roomBytes = 0;

    std::uint32_t ringFrames() const { return frameBytes ? ringBytes / frameBytes : 0; }
};

/// What the card has besides the audio system: video inputs and outputs,
/// whose embedded audio an audio system can take or feed.
struct CardCapabilities
{
    unsigned sdiInputs = 0;
    unsigned hdmiInputs = 0;
    unsigned sdiOutputs = 0;
    unsigned hdmiOutputs = 0;
};

class CardAudio
{
public:
    virtual ~CardAudio() = default;

    /// The card's video connectors, from its feature tables.
    virtual CardCapabilities capabilities() const { return CardCapabilities{}; }

    /// One or more lines: device, audio system and anything worth knowing
    /// about the state found at open (task mode, for instance).
    virtual std::string description() const = 0;

    /// Applies `config` to the stopped audio system: channels, 48 kHz, 4 MB
    /// buffers, loopback off, input source, capture enable, erase mode, card
    /// delays 0. The layout is valid after this returns true.
    virtual bool configure(const AudioConfig& config, std::string& error) = 0;
    virtual RingLayout layout() const = 0;

    virtual bool setCaptureEnable(bool enable) = 0;

    virtual bool startOutput() = 0;
    virtual bool startInput() = 0;
    virtual bool stopOutput() = 0;
    virtual bool stopInput() = 0;

    /// ReadAudioLastIn: where the card is writing the capture ring.
    virtual bool readInputHead(std::uint32_t& byteOffset) = 0;
    /// ReadAudioLastOut: where the card is reading the playback ring.
    virtual bool readOutputHead(std::uint32_t& byteOffset) = 0;
    /// GetRawAudioTimer: the card's 48 kHz counter.
    virtual bool readSampleCounter(std::uint32_t& ticks) = 0;

    /// DMA between host memory and a ring. [offset, offset + bytes) must lie
    /// inside the ring's room (RingLayout::roomBytes): what goes past the
    /// wrap is neither played nor filled there, and splitting at the wrap is
    /// the caller's job.
    virtual bool dmaRead(Ring ring, std::uint32_t offset, void* host, std::uint32_t bytes) = 0;
    virtual bool dmaWrite(Ring ring, std::uint32_t offset, const void* host, std::uint32_t bytes) = 0;
    /// What the system said when the last DMA failed (GetLastError on
    /// Windows, errno elsewhere), 0 if it said nothing: the card's driver
    /// refused it, or the SDK did before asking.
    virtual std::uint32_t lastDmaSystemError() const { return 0; }

    /// DMABufferLock / DMABufferUnlock of a host buffer, for the probe's
    /// cost: the engine locks nothing (on the KONA X a locked buffer was no
    /// faster, and a DMA of part of one failed in 0.1.13's first session).
    virtual bool lockBuffer(const void* host, std::uint32_t bytes) = 0;
    virtual bool unlockBuffer(const void* host, std::uint32_t bytes) = 0;

    virtual bool subscribe(CardEvent event) = 0;
    virtual bool unsubscribe(CardEvent event) = 0;
    /// True if the event arrived within the timeout.
    virtual bool waitForEvent(CardEvent event, unsigned timeoutMs) = 0;
    /// The driver's count of interrupts of that type.
    virtual bool interruptCount(CardEvent event, std::uint32_t& count) = 0;

    /// The Xpand's analog reference level: the dBu that 0 dBFS comes out at
    /// (+24, +18, +15 or +12). False without a breakout board.
    virtual bool readAnalogLevel(int& fullScaleDbu) = 0;

    /// The Xpand's 4-bit analog/AES input select, raw. The SDK only ever
    /// writes 0 (AES) or 1 (analog); what the other values do is Q14. It is
    /// shared by every audio system: restore() puts the old value back.
    /// False without a breakout board.
    virtual bool readXpandSelect(unsigned& value) = 0;
    virtual bool writeXpandSelect(unsigned value) = 0;

    /// Whether the KONA X's breakout board (the Xpand) is connected, as the
    /// card says now. False if the card cannot tell.
    virtual bool breakoutBoardPresent(bool& present)
    {
        (void)present;
        return false;
    }

    /// The card's video reference by name ("Free Run", "Reference In",
    /// "SDI In 1", ...): the clock its audio runs on. Empty if unknown.
    virtual std::string clockReference() { return std::string(); }

    /// Whether SDI connector `connector` (0-based) sends. On cards whose
    /// SDI connectors go both ways, as the KONA X's do, SDI input n and SDI
    /// output n are the same connector, and the AJA Control Panel or a video
    /// program sets which way it goes; this code only reads it. False when
    /// the card's connectors go one way only (each input receives, each
    /// output sends), or when it cannot be read.
    virtual bool sdiSends(unsigned connector, bool& sends)
    {
        (void)connector;
        (void)sends;
        return false;
    }

    /// The routing as the card holds it now. False if none of it could be
    /// read. A few register reads, no allocation: the driver calls it on its
    /// real-time thread, once a second.
    virtual bool readRouting(CardRouting& routing)
    {
        routing = CardRouting{};
        return false;
    }

    /// Sets the `parts` of `routing` it has (routing::kWritable, the task
    /// mode included). The input source is set as the SDK sets it,
    /// which rewrites the Xpand's select for every audio system; the select,
    /// if asked for too, is written after it. What is written here counts as
    /// this program's own, like what configure() sets. False if any part
    /// could not be written.
    virtual bool writeRouting(const CardRouting& routing, unsigned parts)
    {
        (void)routing;
        (void)parts;
        return false;
    }

    /// Stops the audio system and puts back every setting configure() or
    /// setCaptureEnable() changed. The settings the whole card shares (the
    /// Xpand's select and level, the AES outputs, the monitor, the SDI and
    /// HDMI outputs' audio, the task mode) go back only
    /// where the card still holds what this program set: what another
    /// program set since is its own, and stays. Called by the destructor of
    /// the implementations too; calling it twice is harmless.
    virtual void restore() = 0;

    /// What restore() left as it was, and why ("AES out 5-8 left as another
    /// program set them", "task mode left at OEM: AJIN, process 4242, holds
    /// the card"), or put to something other than what it found ("the task
    /// mode put to standard, not back to OEM: ..."), for the log; empty when
    /// it put everything back.
    virtual std::string restoreNote() const { return std::string(); }
};

inline const char* toString(InputSource source)
{
    switch (source)
    {
        case InputSource::Unchanged: return "unchanged";
        case InputSource::Aes:       return "AES";
        case InputSource::Analog:    return "analog";
        case InputSource::Embedded:  return "SDI embedded";
        case InputSource::Hdmi:      return "HDMI";
        case InputSource::Other:     return "another source";
    }
    return "?";
}

inline const char* toString(TaskMode mode)
{
    switch (mode)
    {
        case TaskMode::Unknown:  return "unknown";
        case TaskMode::Disabled: return "disabled";
        case TaskMode::Standard: return "standard";
        case TaskMode::Oem:      return "OEM";
    }
    return "?";
}

inline const char* toString(CardEvent event)
{
    switch (event)
    {
        case CardEvent::Audio:        return "eAudio";
        case CardEvent::AudioInWrap:  return "eAudioInWrap";
        case CardEvent::AudioOutWrap: return "eAudioOutWrap";
    }
    return "?";
}

} // namespace ajaduplex::device
