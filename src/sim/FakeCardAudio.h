// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FakeCardAudio.h
 * @brief   A simulated KONA X audio system, for tests and for --simulate.
 *
 * It keeps the two 4 MB rings in host memory and moves the heads with a
 * simulated 48 kHz audio clock that runs on a VirtualClock:
 *
 *  - The output side fetches the playback ring ahead of what it plays
 *    (outputPrefetchFrames), in chunks, and zeroes what it fetched when
 *    erase mode is on. What it fetched is kept, so a simulated loop cable
 *    can deliver it to the input after cableDelayFrames.
 *  - The input side writes the capture ring in chunks: silence, or what the
 *    cable carries. With the Xpand's select on analog, input 1-2 hear the
 *    analog path instead: whatever the analog output plays, delayed, scaled
 *    and with noise, if the XLR loop is cabled; converter noise if not.
 *  - Register reads and DMAs advance the clock by modelled costs.
 *
 * On a VirtualClock it runs as fast as the code driving it. On a real clock
 * (the driver's simulated card, so a host can be tried without the card)
 * it keeps real time: the heads follow the clock, and costs are whatever
 * the copies really take.
 *
 * The behaviour that phase 0 is meant to measure (head granularity, what
 * the head points at, what capture enable does, costs) is a parameter of
 * FakeCardModel, so the probe's analysis can be tested against each case.
 * The defaults are plausible guesses, not measurements.
 */

#pragma once

#include "device/CardAudio.h"
#include "sim/VirtualClock.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ajaduplex::sim
{

struct FakeCardModel
{
    // Clocks
    double sampleRate = 48000.0;
    double clockPpm = 20.0;              ///< audio clock against the host clock
    bool counterOnAudioClock = true;     ///< raw 48 kHz counter driven by the audio clock
    double counterPpm = 0.0;             ///< its rate otherwise

    // Heads
    std::uint32_t inputChunkFrames = 1;
    std::uint32_t outputChunkFrames = 16;
    std::uint32_t outputPrefetchFrames = 32;
    bool inputHeadAtLastByte = true;     ///< LastIn: last byte written (false: next byte to write)
    bool outputHeadAtLastByte = false;   ///< LastOut: last byte read (false: next byte to read)

    // Behaviour
    bool captureEnableGatesWrites = true;   ///< capture enable off: the head moves, memory stays
    bool eraseSupported = true;
    bool wrapEventsSupported = true;        ///< eAudioInWrap / eAudioOutWrap; eAudio is never simulated
    /// A DMA that touches a locked buffer fails unless it is all of it, from
    /// its start, as on the KONA X (0.1.13's first session).
    bool lockedDmasWholeOnly = true;
    /// Each ring has room past its end that the card never plays or fills,
    /// as on the KONA X (16 KB past L, up to 4 MB); false: the room ends at L.
    bool roomPastEnd = true;

    // Costs on the virtual clock
    std::int64_t registerReadNs = 2000;
    std::int64_t dmaSetupNs = 20000;
    std::int64_t dmaSetupLockedNs = 12000;
    double dmaNsPerByte = 0.25;
    std::int64_t jitterNs = 0;           ///< uniform extra cost in [0, jitterNs]
    std::uint32_t seed = 1;

    // Loop cable, from output channels to input channels (0-based)
    struct Patch
    {
        unsigned output;
        unsigned input;
    };
    std::vector<Patch> cable;            ///< empty: the input hears silence
    std::uint32_t cableDelayFrames = 12; ///< from when a frame plays to when it is captured
    /// The KONA X's AES inputs resample ("internal sample rate conversion";
    /// the words came back changed, 2026-11-10). On, AES in hears AES out
    /// later by a fraction of a frame more, interpolated, with the
    /// converter's noise: never the words sent. Off, bit exact, as through
    /// a plain cable.
    struct AesResampler
    {
        bool on = false;
        double delayFrames = 18.4;       ///< more than cableDelayFrames
        double noiseDbfs = -110.0;       ///< white, AES17 dBFS
    };
    AesResampler aesResampler;
    /// The audio system this one is, 1-based: the driver's default, the
    /// KONA X's last. A shared output routed to another audio system plays
    /// silence here.
    unsigned audioSystem = 2;
    /// The AES outputs already carry this audio system. False is the KONA X
    /// after power-up with audio system 2: they carry audio system 1 (silence
    /// here) until configure() is asked to route them.
    bool aesOutputsOnThisSystem = true;
    /// The monitor already plays channels 1-2 of this audio system.
    bool monitorOnThisSystem = false;
    /// The audio system's input source before configure(). The input hears
    /// the Xpand (AES, or analog where the select says) with AES or analog;
    /// with SDI or HDMI, what the video input carries (videoSignal).
    device::InputSource sourceAtStart = device::InputSource::Aes;
    /// The video connectors, the KONA X's.
    unsigned sdiInputs = 2;
    unsigned hdmiInputs = 1;
    unsigned sdiOutputs = 2;
    unsigned hdmiOutputs = 1;
    /// The KONA X's SDI connectors go both ways: SDI input n and output n
    /// are one connector, which sends or receives as the AJA Control Panel
    /// set it. An SDI input whose connector sends has no signal; an output
    /// whose connector receives sends nothing.
    bool bidirectionalSdi = true;
    unsigned sdiSending = 0;             ///< bits: the connectors that send (none: both are inputs)
    /// The SDI and HDMI outputs' audio already plays this audio system.
    /// False is the card after power-up: audio system 1 (silence here).
    bool videoOutputsOnThisSystem = false;
    /// An SDI cable from an SDI output to an SDI input (0-based): with the
    /// input as the source, it hears the output's channels if the output's
    /// embedder plays this audio system, silence if not.
    struct SdiLoop
    {
        bool looped = false;
        unsigned output = 1;             ///< SDI out 2
        unsigned input = 0;              ///< SDI in 1
        std::uint32_t delayFrames = 24;  ///< embedder, cable and de-embedder: a guess
    };
    SdiLoop sdiLoop;
    /// What an SDI or HDMI input carries as the source: with a signal,
    /// embedded channel c is a tone of 100 (c + 1) Hz at -30 dBFS (16
    /// channels in SDI, 8 in HDMI); without, nothing. Whether the audio
    /// clock stops with the signal (Q7) is stallAudioClock's.
    bool videoSignal = true;
    /// The card's task mode, which only reads tell.
    device::TaskMode taskMode = device::TaskMode::Standard;
    /// Another program holds the card (it reserved it) when this one opens
    /// it, as AJA Input does when it is up first; with taskMode Oem, that
    /// OEM mode is its own.
    bool heldAtStart = false;

    /// The Xpand's analog path: two channels each way. The values that phase
    /// 0 is meant to find (Q4, Q5, Q12) are parameters.
    struct AnalogPath
    {
        bool looped = false;               ///< XLR out L/R cabled to XLR in L/R
        std::uint32_t delayFrames = 40;    ///< DAC and ADC: from when a frame plays to when it is captured
        double gainDb = -0.1;
        double noiseDbfs = -100.0;         ///< white, at the input; AES17 dBFS (RMS against a full-scale sine)
        double dcOffset = 1e-4;            ///< of full scale, at the input
        bool inverted = false;             ///< pins 2 and 3 swapped somewhere on the way
        bool outFollowsMonitor = false;    ///< Q4: false = the analog out plays AES out 1-2
        bool replacesAllAes = false;       ///< Q5: false = the analog in replaces AES in 1-2 only
        /// Q14: what the select's 4-bit values do.
        enum class Select
        {
            Bit0,      ///< only bit 0 counts (value & 1 is analog)
            NonZero,   ///< any value but 0 is analog
            PerPair,   ///< bit k puts analog L/R on AES in pair k (AES and analog together)
        };
        Select select = Select::Bit0;
        bool selectedAtStart = false;      ///< the Xpand's analog/AES select before configure()
        int levelDbu = 24;                 ///< the Xpand's analog level before configure(): what 0 dBFS is at
        std::uint32_t seed = 3;
    };
    AnalogPath analog;
    bool hasBreakoutBoard = true;

    /// The phase 0 set-up: AES out 1-8 looped to AES in 1-8.
    static FakeCardModel withAesLoop();
    /// The analog set-up: XLR out L/R looped to XLR in L/R (and AES too).
    static FakeCardModel withAnalogLoop();
};

class FakeCardAudio final : public device::CardAudio
{
public:
    /// Simulated time: register reads and DMAs advance `clock` by their cost.
    explicit FakeCardAudio(VirtualClock& clock, FakeCardModel model = FakeCardModel{});
    /// Real time: nothing modelled is spent, `clock` just runs.
    FakeCardAudio(Clock& clock, FakeCardModel model);
    ~FakeCardAudio() override;

    std::string description() const override;
    device::CardCapabilities capabilities() const override
    {
        return device::CardCapabilities{model_.sdiInputs, model_.hdmiInputs, model_.sdiOutputs, model_.hdmiOutputs};
    }
    bool configure(const device::AudioConfig& config, std::string& error) override;
    device::RingLayout layout() const override { return layout_; }
    bool setCaptureEnable(bool enable) override;

    bool startOutput() override;
    bool startInput() override;
    bool stopOutput() override;
    bool stopInput() override;

    bool readInputHead(std::uint32_t& byteOffset) override;
    bool readOutputHead(std::uint32_t& byteOffset) override;
    bool readSampleCounter(std::uint32_t& ticks) override;

    bool dmaRead(device::Ring ring, std::uint32_t offset, void* host, std::uint32_t bytes) override;
    bool dmaWrite(device::Ring ring, std::uint32_t offset, const void* host, std::uint32_t bytes) override;
    std::uint32_t lastDmaSystemError() const override { return lastDmaSystemError_; }
    bool lockBuffer(const void* host, std::uint32_t bytes) override;
    bool unlockBuffer(const void* host, std::uint32_t bytes) override;

    bool subscribe(device::CardEvent event) override;
    bool unsubscribe(device::CardEvent event) override;
    bool waitForEvent(device::CardEvent event, unsigned timeoutMs) override;
    bool interruptCount(device::CardEvent event, std::uint32_t& count) override;
    bool readAnalogLevel(int& fullScaleDbu) override;
    bool readXpandSelect(unsigned& value) override;
    bool breakoutBoardPresent(bool& present) override
    {
        present = model_.hasBreakoutBoard;
        return true;
    }
    std::string clockReference() override { return "Free Run"; }
    bool sdiSends(unsigned connector, bool& sends) override;
    bool writeXpandSelect(unsigned value) override;
    bool readRouting(device::CardRouting& routing) override;
    bool writeRouting(const device::CardRouting& routing, unsigned parts) override;

    void restore() override;
    std::string restoreNote() const override { return restoreNote_; }

    // Other programs, for the driver's routing watch
    /// Another program sets these parts, as writeRouting() would (a source
    /// rewrites the select too), but they are its own: restore() leaves
    /// them. False if a part could not be set.
    bool anotherProgramSets(const device::CardRouting& routing, unsigned parts);
    /// Someone (the AJA Control Panel, say) sets the card's task mode.
    void anotherProgramSetsTaskMode(device::TaskMode mode) { routing_.taskMode = mode; }
    /// Another program reserves the card (as AJA Input does: "AJIN",
    /// process 4242) or lets it go.
    void anotherProgramHoldsTheCard(bool holds) { heldByAnother_ = holds; }

    // Faults, for the engine's recovery paths
    /// The audio clock (both heads, and the counter when it runs on that
    /// clock) stands still from `fromNs` for `durationNs`, as when an
    /// embedded source loses its signal.
    void stallAudioClock(std::int64_t fromNs, std::int64_t durationNs);
    /// The capture side alone falls `frames` behind: its head stands still
    /// that long while the playback head goes on, as when the audio
    /// system's input follows a source whose clock runs slow (an embedded
    /// input without a signal; on the KONA X, 2026-11-11, with the AJA
    /// Control Panel changing the input's source, the input lost some two
    /// seconds of frames in the 22 its source was not the card's own).
    /// From then on the two heads are out of step by `frames`.
    void holdInput(std::int64_t frames);
    /// The next `count` DMAs take `extraNs` longer, as when video DMA holds
    /// the engines.
    void delayNextDmas(unsigned count, std::int64_t extraNs);
    /// FakeCardModel::lockedDmasWholeOnly, from now on.
    void lockedDmasWholeOnly(bool on) { lockedWholeOnly_ = on; }
    /// The next `count` DMAs fail, the system saying `systemError`.
    void failNextDmas(unsigned count, std::uint32_t systemError = 0);
    /// From now on every write to the playback ring that starts at
    /// `offset` fails, the system saying `systemError`, but for the first
    /// `spare` of them (the engine's start zeroes the ring from 0).
    void failPlaybackWritesAt(std::uint32_t offset, std::uint32_t systemError, unsigned spare = 0)
    {
        failWritesAt_ = true;
        failWritesOffset_ = offset;
        failWritesError_ = systemError;
        failWritesSpare_ = spare;
    }
    /// A bad host buffer: the first DMA on `ring` from now on whose bytes
    /// take in `offset`, but for the first `spare` of them (the engine's
    /// start zeroes the playback ring), makes its host buffer bad (the same
    /// address): every DMA through it from then on, that one included,
    /// fails, the system saying `systemError`, whatever the ring offset;
    /// with `largestFailing`, only DMAs of that many bytes or fewer (on the
    /// KONA X one buffer failed reads of 512-2624 bytes and passed 8 KB
    /// ones: cost, 2026-11-10).
    void failDmasThroughBufferAt(device::Ring ring, std::uint32_t offset, std::uint32_t systemError,
                                 unsigned spare = 0, std::uint32_t largestFailing = 0xFFFFFFFFu)
    {
        badBufferArmed_ = true;
        badBufferRing_ = ring;
        badBufferOffset_ = offset;
        badBufferError_ = systemError;
        badBufferSpare_ = spare;
        badBufferLargest_ = largestFailing;
    }
    std::uint64_t failedDmas() const { return failedDmas_; }

    /// Every DMA asked of the card, once recordDmas(true) is called.
    struct DmaRecord
    {
        device::Ring ring;
        bool write;
        std::uint32_t offset;
        std::uint32_t bytes;
        const void* host;
        bool ok;
    };
    void recordDmas(bool on) { recordDmas_ = on; }
    const std::vector<DmaRecord>& dmaRecords() const { return dmaRecords_; }

    // For tests
    const FakeCardModel& model() const { return model_; }
    std::uint64_t dmaWrites() const { return dmaWrites_; }
    std::int64_t framesFetched();    ///< output frames fetched since startOutput
    std::int64_t framesWritten();    ///< input frames written since startInput
    std::int64_t audioTicksNow();    ///< audio clock ticks since the card was made
    std::size_t lockedBufferCount() const { return locked_.size(); }
    /// AES out 1-8 play channels 1-8 of this audio system.
    bool aesOutputsRouted() const;
    bool monitorRouted() const { return routing_.monitor.audioSystem == model_.audioSystem; }
    unsigned monitorPair() const { return (routing_.monitor.firstChannel - 1) / 2; }
    bool analogSelected() const { return analogPairs() != 0; }
    unsigned xpandSelect() const { return routing_.xpandSelect; }
    const device::CardRouting& routing() const { return routing_; }

private:
    /// Sets the parts, whoever asks; returns the parts written (a source
    /// adds the select).
    unsigned applyRouting(const device::CardRouting& routing, unsigned parts, bool& ok);
    std::int64_t audioTicksAt(std::int64_t ns) const;
    std::int64_t nsAtAudioTick(std::int64_t tick) const;
    void update();
    void fetchOutputTo(std::int64_t target);
    void writeInputTo(std::int64_t target);
    void spend(std::int64_t ns);
    std::int64_t jitter();
    double analogNoise();   ///< one Gaussian sample, unit RMS
    unsigned analogPairs() const;   ///< bit k: AES in pair k hears analog L/R
    unsigned silencedPairs() const; ///< bit k: AES in pair k hears nothing
    bool eventSupported(device::CardEvent event) const;

    struct Stall
    {
        std::int64_t fromNs;
        std::int64_t durationNs;
    };

    std::int64_t runningNsAt(std::int64_t ns) const;   ///< audio clock time: host time minus the stalls
    std::int64_t dmaExtra();
    /// False, counted, for a DMA the faults above make fail.
    bool dmaGoesThrough(device::Ring ring, bool write, std::uint32_t offset, const void* host, std::uint32_t bytes);

    Clock& clock_;
    VirtualClock* virtualClock_;   ///< null in real time
    FakeCardModel model_;
    std::int64_t originNs_;
    std::uint32_t rng_;
    std::vector<Stall> stalls_;
    unsigned delayedDmas_ = 0;
    std::int64_t dmaDelayNs_ = 0;
    std::uint64_t dmaWrites_ = 0;
    bool lockedWholeOnly_ = false;
    bool failWritesAt_ = false;
    std::uint32_t failWritesOffset_ = 0;
    std::uint32_t failWritesError_ = 0;
    unsigned failWritesSpare_ = 0;
    bool badBufferArmed_ = false;
    device::Ring badBufferRing_ = device::Ring::Playback;
    std::uint32_t badBufferOffset_ = 0;
    std::uint32_t badBufferError_ = 0;
    unsigned badBufferSpare_ = 0;
    std::uint32_t badBufferLargest_ = 0xFFFFFFFFu;
    struct BadBuffer
    {
        std::uint32_t systemError;
        std::uint32_t largestFailing;   ///< DMAs of more bytes go through
    };
    std::map<const void*, BadBuffer> badBuffers_;   ///< by host address
    bool recordDmas_ = false;
    std::vector<DmaRecord> dmaRecords_;
    unsigned failingDmas_ = 0;
    std::uint32_t failingSystemError_ = 0;
    std::uint32_t lastDmaSystemError_ = 0;
    std::uint64_t failedDmas_ = 0;

    bool configured_ = false;
    device::AudioConfig config_;
    device::RingLayout layout_;
    std::vector<std::uint8_t> playback_;
    std::vector<std::uint8_t> capture_;
    std::vector<std::int32_t> history_;   ///< historyFrames_ x channels: what the output fetched
    std::int64_t historyFrames_ = 0;

    bool outputRunning_ = false;
    bool inputRunning_ = false;
    std::int64_t outputStartTick_ = 0;
    std::int64_t inputStartTick_ = 0;
    std::int64_t fetched_ = 0;
    std::int64_t written_ = 0;
    bool captureEnable_ = true;
    /// The input source, the Xpand's select (0-15), the AES outputs, the
    /// monitor and the task mode, as the card holds them now; as they were
    /// when it was made; and as this program last set them (the parts it
    /// touched), for restore().
    device::CardRouting routing_;
    device::CardRouting saved_;
    device::CardRouting ours_;
    unsigned touched_ = 0;
    /// The SDI and HDMI output parts this program has set: only those are
    /// read back, as Ntv2CardAudio does.
    unsigned routedOutputs_ = 0;
    bool heldByAnother_ = false;
    bool heldAtStart_ = false;   ///< ... when this program opened the card
    std::string restoreNote_;
    std::uint32_t analogRng_ = 3;

    std::map<const void*, std::uint32_t> locked_;   ///< address, bytes
    std::set<device::CardEvent> subscribed_;
};

} // namespace ajaduplex::sim
