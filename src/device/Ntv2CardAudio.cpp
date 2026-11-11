// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "device/Ntv2CardAudio.h"

#include "platform/OwnerMark.h"

#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <set>
#include <thread>

namespace ajaduplex::device
{

namespace
{

/// The system's last error, cleared before a call and read after it fails:
/// still 0 then if the SDK refused the call itself.
void clearSystemError()
{
#ifdef _WIN32
    ::SetLastError(0);
#else
    errno = 0;
#endif
}

std::uint32_t systemError()
{
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetLastError());
#else
    return static_cast<std::uint32_t>(errno);
#endif
}

INTERRUPT_ENUMS toInterrupt(CardEvent event)
{
    switch (event)
    {
        case CardEvent::Audio:        return eAudio;
        case CardEvent::AudioInWrap:  return eAudioInWrap;
        case CardEvent::AudioOutWrap: return eAudioOutWrap;
    }
    return eAudio;
}

/// Each audio system's source select register, as in ntv2audio.cpp. It
/// holds the input source, the embedded input, the embedded audio clock and
/// erase mode, among other set-up bits.
const ULWord kSourceSelectRegister[] = {kRegAud1SourceSelect, kRegAud2SourceSelect, kRegAud3SourceSelect,
                                        kRegAud4SourceSelect, kRegAud5SourceSelect, kRegAud6SourceSelect,
                                        kRegAud7SourceSelect, kRegAud8SourceSelect};

/// How long configure() waits after it takes the card out of standard task
/// mode before it writes anything else. In standard mode AJA's services
/// keep writing the card's set-up from the Control Panel's settings (the
/// AES outputs and the monitor back to their mixer within a second, Q9);
/// out of it they stop, but a pass they are in the middle of still
/// finishes. Several video frames, for services that run once a frame.
constexpr std::chrono::milliseconds kServicesSettle{100};

/// The audio system's settings that configure() may change.
struct SavedSettings
{
    ULWord channels = 0;
    NTV2AudioRate rate = NTV2_AUDIO_48K;
    NTV2AudioBufferSize bufferSize = NTV2_AUDIO_BUFFER_BIG;
    NTV2AudioLoopBack loopBack = NTV2_AUDIO_LOOPBACK_OFF;
    bool captureEnable = false;
    bool haveDelays = false;
    ULWord inputDelay = 0;
    ULWord outputDelay = 0;
    /// Saved raw, so a source the SDK cannot name comes back as it was.
    ULWord sourceSelect = 0;
    bool haveXpand = false;
    ULWord xpandSelect = 0;   ///< kRegBOBAudioControl analog input select field
    bool haveLevel = false;
    ULWord analogLevel = 0;   ///< kRegBOBAudioControl analog level field (NTV2AudioLevel)
    bool haveAesMap = false;
    ULWord aesOutputMap = 0;  ///< kRegAudioOutputSourceMap, AES quads only (bits 0-15)
    bool haveMonitor = false;
    ULWord monitorSource = 0; ///< kRegAudioOutputSourceMap, monitor field (bits 16-21)
    bool haveTaskMode = false;
    NTV2TaskMode taskMode = NTV2_TASK_MODE_INVALID;
    /// Whether a program held the card then (it had reserved it), and
    /// which: an OEM mode found so is that program's, not the card's own.
    bool taskModeHeld = false;
    ULWord taskModeHolder = 0;
    int32_t taskModeHolderProcess = 0;
    /// Each SDI output's embedder: the audio system it plays, and whether it
    /// embeds audio at all.
    bool haveSdi[CardRouting::kMaxSdiOutputs] = {};
    NTV2AudioSystem sdiAudioSystem[CardRouting::kMaxSdiOutputs] = {};
    bool sdiEnabled[CardRouting::kMaxSdiOutputs] = {};
    /// The HDMI output's audio: its channel count, and both of its sources
    /// (8 channels, or a pair), whichever the count says is in use.
    bool haveHdmi = false;
    NTV2HDMIAudioChannels hdmiChannels = NTV2_HDMIAudio8Channels;
    NTV2Audio8ChannelSelect hdmiOctet = NTV2_AudioChannel1_8;
    NTV2AudioSystem hdmiOctetSystem = NTV2_AUDIOSYSTEM_1;
    NTV2AudioChannelPair hdmiPair = NTV2_AudioChannel1_2;
    NTV2AudioSystem hdmiPairSystem = NTV2_AUDIOSYSTEM_1;
};

/// The AES output quads' nibbles of kRegAudioOutputSourceMap (ntv2audio.cpp).
/// The register's upper bits hold the monitor and HDMI sources: not ours.
constexpr ULWord kAesOutputMapMask = 0x0000FFFFu;

/// The source field's codes, as GetAudioSystemInputSource reads them (the
/// low nibble of kRegMaskAudioSource).
constexpr ULWord kSourceCodeAes = 0x0;
constexpr ULWord kSourceCodeEmbedded = 0x1;
constexpr ULWord kSourceCodeAnalog = 0x9;
constexpr ULWord kSourceCodeHdmi = 0xA;

/// The SDK's names for a source this code sets. False for Unchanged and Other.
bool toNtv2Source(InputSource in, unsigned embeddedInput, NTV2AudioSource& source, NTV2EmbeddedAudioInput& embedded)
{
    embedded = NTV2_EMBEDDED_AUDIO_INPUT_VIDEO_1;
    switch (in)
    {
        case InputSource::Aes:    source = NTV2_AUDIO_AES; return true;
        case InputSource::Analog: source = NTV2_AUDIO_ANALOG; return true;
        case InputSource::Hdmi:   source = NTV2_AUDIO_HDMI; return true;
        case InputSource::Embedded:
            source = NTV2_AUDIO_EMBEDDED;
            embedded = NTV2EmbeddedAudioInput(NTV2_EMBEDDED_AUDIO_INPUT_VIDEO_1 + embeddedInput);
            return true;
        case InputSource::Unchanged:
        case InputSource::Other:
            break;
    }
    return false;
}

NTV2TaskMode toNtv2(TaskMode mode)
{
    switch (mode)
    {
        case TaskMode::Disabled: return NTV2_DISABLE_TASKS;
        case TaskMode::Standard: return NTV2_STANDARD_TASKS;
        case TaskMode::Oem:      return NTV2_OEM_TASKS;
        case TaskMode::Unknown:  break;
    }
    return NTV2_TASK_MODE_INVALID;
}

/// A four-CC as text: the first character is the most significant byte.
std::string fourcc(ULWord code)
{
    std::string text;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const char c = static_cast<char>((code >> shift) & 0xFFu);
        text += c >= 32 && c < 127 ? c : '?';
    }
    return text;
}

/// A task mode as an owner's mark keeps it, and back.
std::string taskModeWord(NTV2TaskMode mode)
{
    return mode == NTV2_STANDARD_TASKS ? "standard"
           : mode == NTV2_OEM_TASKS    ? "oem"
           : mode == NTV2_DISABLE_TASKS ? "disabled"
                                        : "";
}

bool taskModeFromWord(const std::string& word, NTV2TaskMode& mode)
{
    for (const NTV2TaskMode candidate : {NTV2_STANDARD_TASKS, NTV2_OEM_TASKS, NTV2_DISABLE_TASKS})
    {
        if (!word.empty() && word == taskModeWord(candidate))
        {
            mode = candidate;
            return true;
        }
    }
    return false;
}

/// The audio system's own settings as an owner's mark keeps them, and
/// back. False for numbers the SDK has no name for: not to be written.
platform::FoundSettings toFound(const SavedSettings& s)
{
    platform::FoundSettings found;
    found.channels = s.channels;
    found.rate = static_cast<std::uint32_t>(s.rate);
    found.bufferSize = static_cast<std::uint32_t>(s.bufferSize);
    found.loopBack = static_cast<std::uint32_t>(s.loopBack);
    found.captureEnable = s.captureEnable;
    found.inputDelay = s.haveDelays ? s.inputDelay : 0;
    found.outputDelay = s.haveDelays ? s.outputDelay : 0;
    found.sourceSelect = s.sourceSelect;
    return found;
}

bool fromFound(const platform::FoundSettings& found, bool haveDelays, SavedSettings& s)
{
    const NTV2AudioRate rate = static_cast<NTV2AudioRate>(found.rate);
    const NTV2AudioBufferSize bufferSize = static_cast<NTV2AudioBufferSize>(found.bufferSize);
    const NTV2AudioLoopBack loopBack = static_cast<NTV2AudioLoopBack>(found.loopBack);
    if (found.channels == 0 || !NTV2_IS_VALID_AUDIO_RATE(rate) || !NTV2_IS_VALID_AUDIO_BUFFER_SIZE(bufferSize) ||
        !NTV2_IS_VALID_AUDIO_LOOPBACK(loopBack))
        return false;
    s.channels = found.channels;
    s.rate = rate;
    s.bufferSize = bufferSize;
    s.loopBack = loopBack;
    s.captureEnable = found.captureEnable;
    s.haveDelays = haveDelays;
    s.inputDelay = found.inputDelay;
    s.outputDelay = found.outputDelay;
    s.sourceSelect = found.sourceSelect;
    return true;
}

/// True while the program that reserved the card (GetStreamingApplication)
/// is there to hold it: a reservation outlives a program that ended
/// without letting go, and holds nothing then. AJA's own agent holds the
/// card with process 0, which is nobody.
bool heldByAProgram(int32_t process)
{
    return process != 0 && platform::processRuns(static_cast<std::uint32_t>(process));
}

TaskMode fromNtv2(NTV2TaskMode mode)
{
    switch (mode)
    {
        case NTV2_DISABLE_TASKS:  return TaskMode::Disabled;
        case NTV2_STANDARD_TASKS: return TaskMode::Standard;
        case NTV2_OEM_TASKS:      return TaskMode::Oem;
        default:                  return TaskMode::Unknown;
    }
}

/// AES out group `group` plays a group of four channels of an audio system:
/// the nibble is (audio system - 1) * 4 + (first channel - 1) / 4, for audio
/// systems 1-4 (the comment above GetAESOutputSource in ntv2audio.cpp).
bool aesNibble(OutputSource source, ULWord& nibble)
{
    if (source.audioSystem < 1 || source.audioSystem > 4 || source.firstChannel < 1 || source.firstChannel > 13 ||
        (source.firstChannel - 1) % 4 != 0)
        return false;
    nibble = ULWord((source.audioSystem - 1) * 4 + (source.firstChannel - 1) / 4);
    return true;
}

/// The Xpand's analog level field holds an NTV2AudioLevel: 0 is +24 dBu, 1
/// +18, 2 +12 and 3 +15 (ntv2enums.h; SetAudioAnalogLevel writes it).
bool levelCode(int dbu, ULWord& code)
{
    switch (dbu)
    {
        case 24: code = NTV2_AUDIOLEVEL_24dBu; return true;
        case 18: code = NTV2_AUDIOLEVEL_18dBu; return true;
        case 15: code = NTV2_AUDIOLEVEL_15dBu; return true;
        case 12: code = NTV2_AUDIOLEVEL_12dBu; return true;
        default: return false;
    }
}

/// 0 for a value without a name.
int levelDbu(ULWord code)
{
    switch (code)
    {
        case NTV2_AUDIOLEVEL_24dBu: return 24;
        case NTV2_AUDIOLEVEL_18dBu: return 18;
        case NTV2_AUDIOLEVEL_15dBu: return 15;
        case NTV2_AUDIOLEVEL_12dBu: return 12;
        default:                    return 0;
    }
}

/// The monitor field: (audio system - 1) << 4 | the pair, as
/// SetAudioOutputMonitorSource writes it.
bool monitorEncoding(OutputSource source, ULWord& encoding)
{
    if (source.audioSystem < 1 || source.audioSystem > 4 || source.firstChannel < 1 || source.firstChannel > 31 ||
        (source.firstChannel - 1) % 2 != 0)
        return false;
    encoding = ULWord(((source.audioSystem - 1) << 4) | ((source.firstChannel - 1) / 2));
    return true;
}

class Ntv2CardAudio final : public CardAudio
{
public:
    ~Ntv2CardAudio() override { restore(); }

    bool open(const std::string& selector, unsigned audioSystem, std::string& error, const std::string& pickAnother);

    std::string description() const override { return description_; }
    CardCapabilities capabilities() const override { return capabilities_; }
    std::string restoreNote() const override { return restoreNote_; }
    bool configure(const AudioConfig& config, std::string& error) override;
    RingLayout layout() const override { return layout_; }
    bool setCaptureEnable(bool enable) override;

    bool startOutput() override;
    bool startInput() override;
    bool stopOutput() override;
    bool stopInput() override;

    bool readInputHead(std::uint32_t& byteOffset) override
    {
        ULWord value = 0;
        const bool ok = card_.ReadAudioLastIn(value, system_);
        byteOffset = value;
        return ok;
    }
    bool readOutputHead(std::uint32_t& byteOffset) override
    {
        ULWord value = 0;
        const bool ok = card_.ReadAudioLastOut(value, system_);
        byteOffset = value;
        return ok;
    }
    bool readSampleCounter(std::uint32_t& ticks) override
    {
        ULWord value = 0;
        const bool ok = card_.GetRawAudioTimer(value, system_);
        ticks = value;
        return ok;
    }

    bool dmaRead(Ring ring, std::uint32_t offset, void* host, std::uint32_t bytes) override;
    bool dmaWrite(Ring ring, std::uint32_t offset, const void* host, std::uint32_t bytes) override;
    std::uint32_t lastDmaSystemError() const override { return lastDmaSystemError_; }
    bool lockBuffer(const void* host, std::uint32_t bytes) override
    {
        const bool ok = host != nullptr && card_.DMABufferLock(static_cast<const ULWord*>(host), bytes, /*inMap*/ true);
        lockedAny_ = lockedAny_ || ok;
        return ok;
    }
    bool unlockBuffer(const void* host, std::uint32_t bytes) override
    {
        return host != nullptr && card_.DMABufferUnlock(static_cast<const ULWord*>(host), bytes);
    }

    bool subscribe(CardEvent event) override;
    bool unsubscribe(CardEvent event) override;
    bool waitForEvent(CardEvent event, unsigned timeoutMs) override
    {
        return card_.WaitForInterrupt(toInterrupt(event), timeoutMs);
    }
    bool readAnalogLevel(int& fullScaleDbu) override;
    bool readXpandSelect(unsigned& value) override;
    bool breakoutBoardPresent(bool& present) override;
    std::string clockReference() override;
    bool sdiSends(unsigned connector, bool& sends) override;
    bool writeXpandSelect(unsigned value) override;
    bool readRouting(CardRouting& routing) override;
    bool writeRouting(const CardRouting& routing, unsigned parts) override;
    bool interruptCount(CardEvent event, std::uint32_t& count) override
    {
        ULWord value = 0;
        const bool ok = card_.GetInterruptCount(toInterrupt(event), value);
        count = value;
        return ok;
    }

    void restore() override;

private:
    bool readSettings(SavedSettings& out);
    bool writeSettings(const SavedSettings& settings);
    /// The part of the settings that is the audio system's own, which no
    /// other audio system shares.
    bool writeOwnSettings(const SavedSettings& settings);
    bool readSelectField(ULWord& value);
    bool writeSelectField(ULWord value);
    bool writeLevelField(ULWord code);
    bool routeAesOutputs(std::string& error);
    bool routeMonitor(unsigned pair, std::string& error);
    bool readHdmiOut(HdmiOutputAudio& out);
    bool writeHdmiOut(const HdmiOutputAudio& out);
    bool ringRange(Ring ring, std::uint32_t offset, std::uint32_t bytes, ULWord& absolute) const;
    /// What the card holds now of `parts` becomes this program's own.
    void takeAsOurs(unsigned parts);
    /// From here on this program has the audio system in hand: says so
    /// where it outlives the program, before the first change to the card.
    void markAsOurs();
    /// What a program of this driver's that is gone left behind: its audio
    /// system stopped if it runs, and the audio system's own settings and
    /// the task mode put back as it had found them.
    bool clearWhatWasLeft(const platform::OwnerRecord& left, bool running, std::string& note, std::string& error);

    CNTV2Card card_;
    NTV2AudioSystem system_ = NTV2_AUDIOSYSTEM_INVALID;
    SavedSettings saved_;
    unsigned aesGroups_ = 0;   ///< AES outputs in groups of four (2 on the KONA X)
    bool changed_ = false;
    std::string markName_;     ///< this card's audio system among the owners' marks
    bool marked_ = false;      ///< this program's mark is there
    bool startedInput_ = false;
    bool startedOutput_ = false;
    bool restored_ = false;
    bool lockedAny_ = false;
    std::uint32_t lastDmaSystemError_ = 0;
    bool routedSelect_ = false;   ///< this program wrote the Xpand's select (or a source, which rewrites it)
    bool routedLevel_ = false;    ///< this program wrote the Xpand's analog level
    bool routedTaskMode_ = false; ///< this program set the card's task mode
    std::string restoreNote_;
    bool routedAes_ = false;
    bool routedMonitor_ = false;
    /// The SDI outputs (bits) and the HDMI output this program has set:
    /// only those are read back, and put back.
    unsigned routedSdi_ = 0;
    bool routedHdmi_ = false;
    /// The shared settings as this program last set them, read back: at
    /// restore, only what still holds them is put back.
    CardRouting ours_;
    bool haveOurs_ = false;
    RingLayout layout_;
    std::set<CardEvent> subscribed_;
    std::string description_;
    CardCapabilities capabilities_;
};

bool Ntv2CardAudio::open(const std::string& selector, unsigned audioSystem, std::string& error,
                         const std::string& pickAnother)
{
    if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(selector, card_) || !card_.IsOpen())
    {
        error = "no AJA device matches '" + selector + "' (try: duplex-probe list)";
        return false;
    }

    const unsigned count = card_.features().GetNumAudioSystems();
    const unsigned chosen = audioSystem == 0 ? count : audioSystem;
    if (chosen == 0 || chosen > count)
    {
        error = "audio system " + std::to_string(chosen) + " does not exist; the card has " + std::to_string(count);
        return false;
    }
    system_ = NTV2AudioSystem(chosen - 1);

    bool inputRunning = false, outputRunning = false;
    if (!card_.IsAudioInputRunning(system_, inputRunning) || !card_.IsAudioOutputRunning(system_, outputRunning))
    {
        error = "cannot read the state of audio system " + std::to_string(chosen);
        return false;
    }
    std::string serial;
    if (!card_.GetSerialNumberString(serial) || serial.empty())
        serial = "n/a";

    // AJA's driver stops nothing when a program ends: one closed by force
    // (or that crashed) while it played leaves its audio system running,
    // the card in the task mode it had set, and it all looks in use to the
    // next. What a program of this driver's left so is this one's to
    // clear: its mark says which program it was, and that program is gone.
    markName_ = (serial == "n/a" ? "card" + std::to_string(card_.GetIndexNumber()) : serial) + "-as" +
                std::to_string(chosen);
    platform::OwnerRecord left;
    const bool wasMarked = platform::ownermarks::read(markName_, left);
    const platform::ProcessState owner =
        wasMarked ? platform::stateOf(left.process) : platform::ProcessState::Unknown;
    const bool running = inputRunning || outputRunning;
    platform::OpenAction action = platform::onOpen(running, wasMarked, owner);
    if (action == platform::OpenAction::TakeOver)
    {
        // Running as this driver runs it, with the playback erased behind
        // the head: if not, another program has started it since, for
        // itself, and the mark is stale.
        bool erase = false;
        if (!card_.GetAudioOutputEraseMode(system_, erase) || !erase)
            action = platform::OpenAction::Refuse;
    }
    std::string leftNote;
    switch (action)
    {
        case platform::OpenAction::Refuse:
        {
            const std::string sides =
                inputRunning && outputRunning ? "input and output are" : inputRunning ? "input is" : "output is";
            error = "audio system " + std::to_string(chosen) + " is in use (its " + sides + " running)";
            if (wasMarked && owner != platform::ProcessState::Gone)
                error += " by " + (left.program.empty() ? std::string("a program") : left.program) + " (process " +
                         std::to_string(left.process.id) + "), through AJA Duplex ASIO or duplex-probe: close it";
            else
                error += ": close the program using it, or pick another " + pickAnother +
                         ". If no program is using it, one that was closed by force left it running: restarting "
                         "the PC stops it";
            return false;
        }
        case platform::OpenAction::TakeOver:
        case platform::OpenAction::PutBack:
            if (!clearWhatWasLeft(left, running, leftNote, error))
                return false;
            platform::ownermarks::remove(markName_);
            break;
        case platform::OpenAction::Proceed:
            break;
    }
    if (!readSettings(saved_))
    {
        error = "cannot read the settings of audio system " + std::to_string(chosen);
        return false;
    }
    aesGroups_ = saved_.haveAesMap ? std::min<unsigned>(unsigned(card_.features().GetNumAESAudioOutputChannels()) / 4,
                                                        CardRouting::kMaxAesGroups)
                                   : 0;
    capabilities_.sdiInputs = card_.features().GetNumVideoInputs();
    capabilities_.hdmiInputs = card_.features().GetNumHDMIVideoInputs();
    capabilities_.sdiOutputs = card_.features().GetNumVideoOutputs();
    capabilities_.hdmiOutputs = card_.features().GetNumHDMIVideoOutputs();

    NTV2TaskMode mode = NTV2_TASK_MODE_INVALID;
    const std::string taskMode = card_.GetTaskMode(mode) ? ::NTV2TaskModeToString(mode) : std::string("n/a");
    // The versions, so that a log says what ran against what.
    UWord major = 0, minor = 0, point = 0, build = 0;
    const std::string driver = card_.GetDriverVersionComponents(major, minor, point, build)
                                   ? std::to_string(major) + "." + std::to_string(minor) + "." +
                                         std::to_string(point) + "." + std::to_string(build)
                                   : std::string("n/a");
    UWord firmware = 0;
    const std::string firmwareText =
        card_.GetRunningFirmwareRevision(firmware) ? std::to_string(firmware) : std::string("n/a");
    description_ = card_.GetModelName() + " (serial " + serial + "), audio system " + std::to_string(chosen) +
                   ", task mode " + taskMode + "; AJA driver " + driver + ", firmware revision " + firmwareText +
                   ", SDK " + ::NTV2GetVersionString(true);
    // A line of its own, in the driver's log and in every report.
    if (!leftNote.empty())
        description_ += "\n" + leftNote;
    return true;
}

bool Ntv2CardAudio::clearWhatWasLeft(const platform::OwnerRecord& left, bool running, std::string& note,
                                     std::string& error)
{
    const std::string who = (left.program.empty() ? std::string("a program") : left.program) + " (process " +
                            std::to_string(left.process.id) + ")";
    const std::string system = "audio system " + std::to_string(unsigned(system_) + 1);
    if (running)
    {
        bool input = true, output = true;
        const bool stopped = card_.StopAudioInput(system_) && card_.StopAudioOutput(system_) &&
                             card_.IsAudioInputRunning(system_, input) &&
                             card_.IsAudioOutputRunning(system_, output) && !input && !output;
        if (!stopped)
        {
            error = system + " was left running by " + who + ", which is gone, and could not be stopped: " +
                    "restarting the PC stops it";
            return false;
        }
        note = "taken over: " + who + " left " + system + " running and is gone (closed by force, or it crashed); "
               "stopped";
    }
    else
        note = "cleared: " + who + " had " + system + " in hand and is gone (closed by force, or it crashed)";

    // The audio system's own settings as that program found them, which it
    // would have put back at its close: nothing else puts back the capture
    // enable and the erase mode it set (KONA X, 2026-11-11: both were left
    // on after a takeover that stopped the audio system and no more). Only
    // while the audio system is still as this driver sets it, with the
    // playback erased behind the head: if not, something else has set it
    // since, and it is not this program's to write over.
    bool erase = false;
    SavedSettings asFound;
    if (!left.haveSettings)
        note += "; the audio system's own settings left as they are: the mark, of 0.1.21 or 0.1.22, does not say "
                "how it had found them";
    else if (unsigned(system_) >= sizeof(kSourceSelectRegister) / sizeof(kSourceSelectRegister[0]) ||
             !card_.GetAudioOutputEraseMode(system_, erase) || !erase)
        note += "; the audio system's own settings left as they are: no longer as it had set them (the erase mode "
                "is off)";
    else if (fromFound(left.settings, card_.features().CanDoAudioDelay(), asFound) && writeOwnSettings(asFound))
        note += "; the audio system's own settings back as it had found them";
    else
        note += "; the audio system's own settings could not all be put back as it had found them";

    // The task mode it found, if the card is still in the one it set and
    // nobody is there to need it: the OEM mode it left is nobody's now.
    NTV2TaskMode found = NTV2_TASK_MODE_INVALID, now = NTV2_TASK_MODE_INVALID;
    if (taskModeFromWord(left.taskModeFound, found) && card_.GetTaskMode(now) && now != found &&
        now == NTV2_OEM_TASKS)
    {
        ULWord app = 0;
        int32_t process = 0;
        const bool held =
            card_.CNTV2DriverInterface::GetStreamingApplication(app, process) && heldByAProgram(process);
        if (held)
            note += "; the task mode left as it is: " + fourcc(app) + ", process " + std::to_string(process) +
                    ", holds the card";
        else if (card_.SetTaskMode(found))
            note += "; the task mode back to " + left.taskModeFound + ", as it had found it";
        else
            note += "; the task mode could not be put back to " + left.taskModeFound;
    }
    return true;
}

void Ntv2CardAudio::markAsOurs()
{
    if (marked_ || markName_.empty())
        return;
    platform::OwnerRecord mine;
    mine.process = platform::thisProcess();
    mine.program = platform::thisProgramName();
    if (saved_.haveTaskMode)
        mine.taskModeFound = taskModeWord(saved_.taskMode);
    mine.settings = toFound(saved_);
    mine.haveSettings = true;
    // Where marks cannot be kept there is none, and what this program
    // leaves behind if it is ended by force is refused as another's.
    marked_ = platform::ownermarks::write(markName_, mine);
}

bool Ntv2CardAudio::readSelectField(ULWord& value)
{
    return card_.ReadRegister(kRegBOBAudioControl, value, ULWord(kRegMaskBOBAnalogInputSelect),
                              ULWord(kRegShiftBOBAnalogInputSelect));
}

bool Ntv2CardAudio::writeSelectField(ULWord value)
{
    return card_.WriteRegister(kRegBOBAudioControl, value, ULWord(kRegMaskBOBAnalogInputSelect),
                               ULWord(kRegShiftBOBAnalogInputSelect));
}

bool Ntv2CardAudio::writeLevelField(ULWord code)
{
    return card_.WriteRegister(kRegBOBAudioControl, code, ULWord(kRegMaskBOBAnalogLevelControl),
                               ULWord(kRegShiftBOBAnalogLevelControl));
}

bool Ntv2CardAudio::readSettings(SavedSettings& out)
{
    if (unsigned(system_) >= sizeof(kSourceSelectRegister) / sizeof(kSourceSelectRegister[0]))
        return false;
    bool ok = card_.GetNumberAudioChannels(out.channels, system_) && card_.GetAudioRate(out.rate, system_) &&
              card_.GetAudioBufferSize(out.bufferSize, system_) && card_.GetAudioLoopBack(out.loopBack, system_) &&
              card_.GetAudioCaptureEnable(system_, out.captureEnable) &&
              card_.ReadRegister(kSourceSelectRegister[system_], out.sourceSelect);
    out.haveDelays = card_.features().CanDoAudioDelay();
    if (ok && out.haveDelays)
        ok = card_.GetAudioInputDelay(system_, out.inputDelay) && card_.GetAudioOutputDelay(system_, out.outputDelay);
    out.haveXpand = card_.features().CanDoBreakoutBoard() && readSelectField(out.xpandSelect);
    out.haveLevel = out.haveXpand && card_.ReadRegister(kRegBOBAudioControl, out.analogLevel,
                                                        ULWord(kRegMaskBOBAnalogLevelControl),
                                                        ULWord(kRegShiftBOBAnalogLevelControl));
    out.haveAesMap = card_.features().GetNumAESAudioOutputChannels() >= 4 &&
                     card_.ReadRegister(kRegAudioOutputSourceMap, out.aesOutputMap, kAesOutputMapMask, 0);
    out.haveMonitor = card_.ReadRegister(kRegAudioOutputSourceMap, out.monitorSource, ULWord(kRegMaskMonitorSource),
                                         ULWord(kRegShiftMonitorSource));
    out.haveTaskMode = card_.GetTaskMode(out.taskMode) && NTV2_IS_VALID_TASK_MODE(out.taskMode);
    out.taskModeHeld = out.haveTaskMode &&
                       card_.CNTV2DriverInterface::GetStreamingApplication(out.taskModeHolder,
                                                                           out.taskModeHolderProcess) &&
                       out.taskModeHolderProcess != 0;   // gone or not: an OEM mode found with it was its own
    const unsigned sdiOutputs =
        std::min<unsigned>(unsigned(card_.features().GetNumVideoOutputs()), CardRouting::kMaxSdiOutputs);
    for (unsigned n = 0; n < sdiOutputs; ++n)
        out.haveSdi[n] = card_.GetSDIOutputAudioSystem(NTV2Channel(n), out.sdiAudioSystem[n]) &&
                         card_.GetSDIOutputAudioEnabled(NTV2Channel(n), out.sdiEnabled[n]);
    out.haveHdmi = card_.features().GetNumHDMIVideoOutputs() > 0 && card_.GetHDMIOutAudioChannels(out.hdmiChannels) &&
                   card_.GetHDMIOutAudioSource8Channel(out.hdmiOctet, out.hdmiOctetSystem) &&
                   card_.GetHDMIOutAudioSource2Channel(out.hdmiPair, out.hdmiPairSystem);
    return ok;
}

bool Ntv2CardAudio::routeMonitor(unsigned pair, std::string& error)
{
    if (!saved_.haveMonitor)
    {
        error = "the monitor source could not be read, so it would not be put back";
        return false;
    }
    if (pair > 7)
    {
        error = "the monitor plays one of 8 channel pairs, not pair " + std::to_string(pair + 1);
        return false;
    }
    routedMonitor_ = true;
    if (!card_.SetAudioOutputMonitorSource(NTV2AudioChannelPair(pair), system_))
    {
        error = "SetAudioOutputMonitorSource(" + std::to_string(2 * pair + 1) + "-" + std::to_string(2 * pair + 2) +
                ") failed";
        return false;
    }
    return true;
}

bool Ntv2CardAudio::readHdmiOut(HdmiOutputAudio& out)
{
    NTV2HDMIAudioChannels channels = NTV2_HDMIAudio8Channels;
    NTV2AudioSystem system = NTV2_AUDIOSYSTEM_INVALID;
    if (!card_.GetHDMIOutAudioChannels(channels))
        return false;
    if (channels == NTV2_HDMIAudio2Channels)
    {
        NTV2AudioChannelPair pair = NTV2_AudioChannel1_2;
        if (!card_.GetHDMIOutAudioSource2Channel(pair, system))
            return false;
        out = HdmiOutputAudio{unsigned(system) + 1, 2, 2 * unsigned(pair) + 1};
        return true;
    }
    NTV2Audio8ChannelSelect octet = NTV2_AudioChannel1_8;
    if (!card_.GetHDMIOutAudioSource8Channel(octet, system))
        return false;
    out = HdmiOutputAudio{unsigned(system) + 1, 8, octet == NTV2_AudioChannel9_16 ? 9u : 1u};
    return true;
}

bool Ntv2CardAudio::writeHdmiOut(const HdmiOutputAudio& out)
{
    if (out.audioSystem < 1 || out.audioSystem > 8)
        return false;
    const NTV2AudioSystem system = NTV2AudioSystem(out.audioSystem - 1);
    if (out.channels == 8 && (out.firstChannel == 1 || out.firstChannel == 9))
        return card_.SetHDMIOutAudioSource8Channel(
            out.firstChannel == 9 ? NTV2_AudioChannel9_16 : NTV2_AudioChannel1_8, system);
    if (out.channels == 2 && out.firstChannel >= 1 && out.firstChannel <= 15 && out.firstChannel % 2 == 1)
        return card_.SetHDMIOutAudioSource2Channel(NTV2AudioChannelPair((out.firstChannel - 1) / 2), system);
    return false;
}

bool Ntv2CardAudio::sdiSends(unsigned connector, bool& sends)
{
    // On cards whose connectors go one way the SDK says every output sends,
    // which says nothing about the inputs: not an answer.
    if (!card_.features().HasBiDirectionalSDI() || connector >= capabilities_.sdiOutputs)
        return false;
    return card_.GetSDITransmitEnable(NTV2Channel(connector), sends);
}

bool Ntv2CardAudio::readXpandSelect(unsigned& value)
{
    ULWord field = 0;
    if (!saved_.haveXpand || !readSelectField(field))
        return false;
    value = field;
    return true;
}

bool Ntv2CardAudio::writeXpandSelect(unsigned value)
{
    if (!saved_.haveXpand || value > 15)
        return false;
    markAsOurs();
    changed_ = true;   // restore() puts the saved value back
    routedSelect_ = true;
    const bool ok = writeSelectField(value);
    takeAsOurs(routing::kXpandSelect);
    return ok;
}

void Ntv2CardAudio::takeAsOurs(unsigned parts)
{
    CardRouting now;
    if (!readRouting(now))
        return;
    if (!haveOurs_)
    {
        ours_ = now;   // the rest as found: restore() leaves what it did not change alone anyway
        haveOurs_ = true;
        return;
    }
    copyRoutingParts(ours_, now, parts);
}

bool Ntv2CardAudio::readRouting(CardRouting& r)
{
    r = CardRouting{};
    bool any = false;

    ULWord sourceSelect = 0;
    if (unsigned(system_) < sizeof(kSourceSelectRegister) / sizeof(kSourceSelectRegister[0]) &&
        card_.ReadRegister(kSourceSelectRegister[system_], sourceSelect))
    {
        r.haveSource = true;
        const ULWord code = sourceSelect & 0xFu;
        switch (code)
        {
            case kSourceCodeAes:      r.source = InputSource::Aes; break;
            case kSourceCodeEmbedded: r.source = InputSource::Embedded; break;
            case kSourceCodeAnalog:   r.source = InputSource::Analog; break;
            case kSourceCodeHdmi:     r.source = InputSource::Hdmi; break;
            default:
                r.source = InputSource::Other;
                r.sourceCode = unsigned(code);
                break;
        }
        NTV2EmbeddedAudioInput input = NTV2_EMBEDDED_AUDIO_INPUT_VIDEO_1;
        if (r.source == InputSource::Embedded && card_.GetEmbeddedAudioInput(input, system_) &&
            NTV2_IS_VALID_EMBEDDED_AUDIO_INPUT(input))
            r.embeddedInput = unsigned(input);
        any = true;
    }

    // The Xpand's select and level share one register: one read.
    ULWord bob = 0;
    if (saved_.haveXpand && card_.ReadRegister(kRegBOBAudioControl, bob))
    {
        r.haveXpandSelect = true;
        r.xpandSelect =
            unsigned((bob & ULWord(kRegMaskBOBAnalogInputSelect)) >> ULWord(kRegShiftBOBAnalogInputSelect));
        if (saved_.haveLevel)
        {
            const ULWord code =
                (bob & ULWord(kRegMaskBOBAnalogLevelControl)) >> ULWord(kRegShiftBOBAnalogLevelControl);
            r.haveAnalogLevel = true;
            r.analogLevelDbu = levelDbu(code);
            r.analogLevelCode = r.analogLevelDbu == 0 ? unsigned(code) : 0u;
        }
        any = true;
    }

    // The AES groups and the monitor share one register: one read.
    ULWord map = 0;
    if ((aesGroups_ > 0 || saved_.haveMonitor) && card_.ReadRegister(kRegAudioOutputSourceMap, map))
    {
        r.aesGroups = aesGroups_;
        for (unsigned g = 0; g < aesGroups_; ++g)
        {
            const unsigned nibble = unsigned(map >> (4 * g)) & 0xFu;
            r.aes[g] = OutputSource{nibble / 4 + 1, (nibble % 4) * 4 + 1};
        }
        if (saved_.haveMonitor)
        {
            const unsigned encoding = unsigned((map & ULWord(kRegMaskMonitorSource)) >> ULWord(kRegShiftMonitorSource));
            r.haveMonitor = true;
            r.monitor = OutputSource{(encoding >> 4) + 1, 2 * (encoding & 0xFu) + 1};
        }
        any = true;
    }

    // Two reads for each SDI output this program set, two for the HDMI.
    for (unsigned n = 0; n < CardRouting::kMaxSdiOutputs; ++n)
    {
        NTV2AudioSystem system = NTV2_AUDIOSYSTEM_INVALID;
        bool enabled = false;
        if ((routedSdi_ & (1u << n)) != 0 && card_.GetSDIOutputAudioSystem(NTV2Channel(n), system) &&
            card_.GetSDIOutputAudioEnabled(NTV2Channel(n), enabled))
        {
            r.sdiOutputs |= 1u << n;
            r.sdiOut[n] = SdiOutputAudio{unsigned(system) + 1, enabled};
            any = true;
        }
    }
    if (routedHdmi_ && readHdmiOut(r.hdmiOut))
    {
        r.haveHdmiOut = true;
        any = true;
    }

    NTV2TaskMode mode = NTV2_TASK_MODE_INVALID;
    if (card_.GetTaskMode(mode))
    {
        r.haveTaskMode = true;
        r.taskMode = fromNtv2(mode);
        any = true;
    }
    return any;
}

bool Ntv2CardAudio::writeRouting(const CardRouting& r, unsigned parts)
{
    parts &= routingParts(r) & routing::kWritable;
    bool ok = true;
    unsigned written = 0;
    if (parts != 0)
        markAsOurs();

    // The task mode first: with the card back in OEM mode AJA's services
    // are off before the rest is written, and cannot write over it again.
    if ((parts & routing::kTaskMode) != 0)
    {
        if (!saved_.haveTaskMode || toNtv2(r.taskMode) == NTV2_TASK_MODE_INVALID)
            ok = false;
        else
        {
            routedTaskMode_ = true;
            ok = card_.SetTaskMode(toNtv2(r.taskMode)) && ok;
            written |= routing::kTaskMode;
        }
    }

    if ((parts & routing::kSource) != 0 &&
        unsigned(system_) < sizeof(kSourceSelectRegister) / sizeof(kSourceSelectRegister[0]))
    {
        changed_ = true;   // restore() writes the saved source register back whole
        NTV2AudioSource source = NTV2_AUDIO_AES;
        NTV2EmbeddedAudioInput embedded = NTV2_EMBEDDED_AUDIO_INPUT_VIDEO_1;
        if (toNtv2Source(r.source, r.embeddedInput, source, embedded))
        {
            // As configure() does: the SDK rewrites the shared select too.
            routedSelect_ = true;
            ok = card_.SetAudioSystemInputSource(system_, source, embedded) && ok;
            if (r.source == InputSource::Aes || r.source == InputSource::Analog)
                ok = card_.SetEmbeddedAudioClock(NTV2_EMBEDDED_AUDIO_CLOCK_REFERENCE, system_) && ok;
            written |= routing::kXpandSelect;
        }
        else
        {
            // A source this code does not name goes back as its code.
            ok = card_.WriteRegister(kSourceSelectRegister[system_], ULWord(r.sourceCode),
                                     ULWord(kRegMaskAudioSource), ULWord(kRegShiftAudioSource)) &&
                 ok;
        }
        written |= routing::kSource;
    }
    if ((parts & routing::kXpandSelect) != 0)
    {
        if (!saved_.haveXpand || r.xpandSelect > 15)
            ok = false;
        else
        {
            changed_ = true;
            routedSelect_ = true;
            ok = writeSelectField(ULWord(r.xpandSelect)) && ok;
            written |= routing::kXpandSelect;
        }
    }
    if ((parts & routing::kAnalogLevel) != 0)
    {
        ULWord code = 0;
        const bool named = levelCode(r.analogLevelDbu, code);
        if (!named)
            code = ULWord(r.analogLevelCode);   // a value without a name goes back as it was
        if (!saved_.haveLevel || (!named && (r.analogLevelDbu != 0 || code > 15)))
            ok = false;
        else
        {
            changed_ = true;
            routedLevel_ = true;
            ok = writeLevelField(code) && ok;
            written |= routing::kAnalogLevel;
        }
    }
    for (unsigned g = 0; g < aesGroups_ && g < r.aesGroups; ++g)
    {
        const unsigned bit = routing::kAesGroup << g;
        if ((parts & bit) == 0)
            continue;
        ULWord nibble = 0;
        if (!aesNibble(r.aes[g], nibble))
        {
            ok = false;
            continue;
        }
        changed_ = true;
        routedAes_ = true;
        ok = card_.WriteRegister(kRegAudioOutputSourceMap, nibble, ULWord(0xFu) << (4 * g), ULWord(4 * g)) && ok;
        written |= bit;
    }
    if ((parts & routing::kMonitor) != 0)
    {
        ULWord encoding = 0;
        if (!saved_.haveMonitor || !monitorEncoding(r.monitor, encoding))
            ok = false;
        else
        {
            changed_ = true;
            routedMonitor_ = true;
            ok = card_.WriteRegister(kRegAudioOutputSourceMap, encoding, ULWord(kRegMaskMonitorSource),
                                     ULWord(kRegShiftMonitorSource)) &&
                 ok;
            written |= routing::kMonitor;
        }
    }
    for (unsigned n = 0; n < CardRouting::kMaxSdiOutputs; ++n)
    {
        const unsigned bit = routing::kSdiOut << n;
        if ((parts & bit) == 0)
            continue;
        const SdiOutputAudio out = r.sdiOut[n];
        if (!saved_.haveSdi[n] || out.audioSystem < 1 || out.audioSystem > 8)
        {
            ok = false;
            continue;
        }
        changed_ = true;
        routedSdi_ |= 1u << n;
        ok = card_.SetSDIOutputAudioSystem(NTV2Channel(n), NTV2AudioSystem(out.audioSystem - 1)) && ok;
        ok = card_.SetSDIOutputAudioEnabled(NTV2Channel(n), out.enabled) && ok;
        written |= bit;
    }
    if ((parts & routing::kHdmiOut) != 0)
    {
        if (!saved_.haveHdmi)
            ok = false;
        else
        {
            changed_ = true;
            routedHdmi_ = true;
            ok = writeHdmiOut(r.hdmiOut) && ok;
            written |= routing::kHdmiOut;
        }
    }
    takeAsOurs(written);
    return ok;
}

bool Ntv2CardAudio::breakoutBoardPresent(bool& present)
{
    // On the KONA X this reads the breakout board's status register (its
    // "absent" bits), so it says what is plugged in now.
    present = card_.IsSupported(kDeviceHasBreakoutBoard);
    return true;
}

std::string Ntv2CardAudio::clockReference()
{
    NTV2ReferenceSource source = NTV2_REFERENCE_INVALID;
    if (!card_.GetReference(source))
        return std::string();
    return ::NTV2ReferenceSourceToString(source, /*inForRetailDisplay*/ true);
}

bool Ntv2CardAudio::readAnalogLevel(int& fullScaleDbu)
{
    NTV2AudioLevel level = NTV2_AUDIOLEVEL_24dBu;
    if (!card_.IsSupported(kDeviceHasBreakoutBoard) || !card_.GetAudioAnalogLevel(level, system_))
        return false;
    switch (level)
    {
        case NTV2_AUDIOLEVEL_24dBu: fullScaleDbu = 24; return true;
        case NTV2_AUDIOLEVEL_18dBu: fullScaleDbu = 18; return true;
        case NTV2_AUDIOLEVEL_15dBu: fullScaleDbu = 15; return true;
        case NTV2_AUDIOLEVEL_12dBu: fullScaleDbu = 12; return true;
        default: return false;
    }
}

bool Ntv2CardAudio::routeAesOutputs(std::string& error)
{
    if (!saved_.haveAesMap)
    {
        error = "the card has no AES outputs to route";
        return false;
    }
    // One nibble per quad of AES outputs: quad q plays channels of the same
    // quad of this audio system. The KONA X has two (AES 1-4 and 5-8).
    routedAes_ = true;
    for (ULWord q = 0; q < aesGroups_; ++q)
    {
        const NTV2Audio4ChannelSelect quad = NTV2Audio4ChannelSelect(q);
        if (!card_.SetAESOutputSource(quad, system_, quad))
        {
            error = "SetAESOutputSource(AES " + std::to_string(4 * q + 1) + "-" + std::to_string(4 * q + 4) +
                    ") failed";
            return false;
        }
    }
    return true;
}

bool Ntv2CardAudio::writeOwnSettings(const SavedSettings& s)
{
    bool ok = card_.SetNumberAudioChannels(s.channels, system_);
    ok = card_.SetAudioRate(s.rate, system_) && ok;
    ok = card_.SetAudioBufferSize(s.bufferSize, system_) && ok;
    ok = card_.SetAudioLoopBack(s.loopBack, system_) && ok;
    ok = card_.SetAudioCaptureEnable(system_, s.captureEnable) && ok;
    if (s.haveDelays)
    {
        ok = card_.SetAudioInputDelay(system_, s.inputDelay) && ok;
        ok = card_.SetAudioOutputDelay(system_, s.outputDelay) && ok;
    }
    // Source, embedded input, embedded clock and erase mode in one write.
    ok = card_.WriteRegister(kSourceSelectRegister[system_], s.sourceSelect) && ok;
    return ok;
}

bool Ntv2CardAudio::writeSettings(const SavedSettings& s)
{
    // The settings the card shares go back only where it still holds what
    // this program set: what another program set since is its own.
    CardRouting now;
    const unsigned theirs = haveOurs_ && readRouting(now) ? routingDifferences(now, ours_) : 0u;
    restoreNote_.clear();
    auto note = [this](const std::string& text) { restoreNote_ += (restoreNote_.empty() ? "" : "; ") + text; };

    // The audio system's own, then the Xpand's select, which
    // SetAudioSystemInputSource may have changed for every audio system.
    bool ok = writeOwnSettings(s);
    if (s.haveXpand && routedSelect_)
    {
        if ((theirs & routing::kXpandSelect) == 0)
            ok = writeSelectField(s.xpandSelect) && ok;
        else
            note("the Xpand's input select left as another program set it");
    }
    if (s.haveLevel && routedLevel_)
    {
        if ((theirs & routing::kAnalogLevel) == 0)
            ok = writeLevelField(s.analogLevel) && ok;
        else
            note("the Xpand's analog level left as another program set it");
    }
    // The AES map is shared by every audio system: only put back what we
    // changed, group by group, so another program's routing survives.
    if (s.haveAesMap && routedAes_)
    {
        for (unsigned g = 0; g < aesGroups_; ++g)
        {
            if ((theirs & (routing::kAesGroup << g)) != 0)
            {
                note("AES out " + std::to_string(4 * g + 1) + "-" + std::to_string(4 * g + 4) +
                     " left as another program set them");
                continue;
            }
            ok = card_.WriteRegister(kRegAudioOutputSourceMap, (s.aesOutputMap >> (4 * g)) & 0xFu,
                                     ULWord(0xFu) << (4 * g), ULWord(4 * g)) &&
                 ok;
        }
    }
    if (s.haveMonitor && routedMonitor_)
    {
        if ((theirs & routing::kMonitor) == 0)
            ok = card_.WriteRegister(kRegAudioOutputSourceMap, s.monitorSource, ULWord(kRegMaskMonitorSource),
                                     ULWord(kRegShiftMonitorSource)) &&
                 ok;
        else
            note("the monitor left as another program set it");
    }
    for (unsigned n = 0; n < CardRouting::kMaxSdiOutputs; ++n)
    {
        if ((routedSdi_ & (1u << n)) == 0 || !s.haveSdi[n])
            continue;
        if ((theirs & (routing::kSdiOut << n)) != 0)
        {
            note("SDI out " + std::to_string(n + 1) + " left as another program set it");
            continue;
        }
        ok = card_.SetSDIOutputAudioSystem(NTV2Channel(n), s.sdiAudioSystem[n]) && ok;
        ok = card_.SetSDIOutputAudioEnabled(NTV2Channel(n), s.sdiEnabled[n]) && ok;
    }
    if (routedHdmi_ && s.haveHdmi)
    {
        if ((theirs & routing::kHdmiOut) != 0)
            note("the HDMI out left as another program set it");
        else if (s.hdmiChannels == NTV2_HDMIAudio2Channels)
            ok = card_.SetHDMIOutAudioSource2Channel(s.hdmiPair, s.hdmiPairSystem) && ok;
        else
            ok = card_.SetHDMIOutAudioSource8Channel(s.hdmiOctet, s.hdmiOctetSystem) && ok;
    }
    if (s.haveTaskMode && routedTaskMode_)
    {
        // The design's rule: back to the old mode only if nobody holds the
        // card, or a program that reserved it (AJA Input) is left to the
        // services. AJA's own agent holds it with process 0, and a program
        // that ended without letting go holds it no longer.
        ULWord app = 0;
        int32_t process = 0;
        const bool held =
            card_.CNTV2DriverInterface::GetStreamingApplication(app, process) && heldByAProgram(process);
        if ((theirs & routing::kTaskMode) != 0)
            note("the task mode left as another program set it");
        else if (held)
            note("the task mode left as it is: " + fourcc(app) + ", process " + std::to_string(process) +
                 ", holds the card");
        else if (s.taskMode == NTV2_OEM_TASKS && s.taskModeHeld)
        {
            // The OEM mode found at open was the program's that held the
            // card then (AJA Input sets it when it opens the card, and puts
            // back what it found when it closes it). It holds the card no
            // longer, and nobody is left to undo its OEM mode: back to
            // AJA's services, or the card would stay in OEM mode for good.
            ok = card_.SetTaskMode(NTV2_STANDARD_TASKS) && ok;
            note("the task mode put to standard, not back to OEM: " + fourcc(s.taskModeHolder) + ", process " +
                 std::to_string(s.taskModeHolderProcess) + ", which held the card at the start, holds it no longer");
        }
        else
            ok = card_.SetTaskMode(s.taskMode) && ok;
    }
    return ok;
}

bool Ntv2CardAudio::configure(const AudioConfig& config, std::string& error)
{
    if (startedInput_ || startedOutput_)
    {
        error = "the audio system is running; stop it before configuring";
        return false;
    }
    if (config.channels == 0 || config.channels > card_.features().GetMaxAudioChannels())
    {
        error = "the card does not do " + std::to_string(config.channels) + " audio channels";
        return false;
    }
    markAsOurs();
    changed_ = true;

    auto step = [&error](bool ok, const char* what) {
        if (!ok)
            error = std::string(what) + " failed";
        return ok;
    };

    // The task mode first, and when that takes the card from AJA's
    // services, nothing else until a pass they were in the middle of is
    // over: written at once, the rest could be written over by it (the
    // outputs on the services' mixer from the first block on, with nothing
    // to say so).
    if (config.taskMode != TaskMode::Unknown)
    {
        if (!saved_.haveTaskMode)
        {
            error = "the card's task mode could not be read, so it would not be put back";
            return false;
        }
        NTV2TaskMode current = NTV2_TASK_MODE_INVALID;
        const bool fromServices = card_.GetTaskMode(current) && current == NTV2_STANDARD_TASKS &&
                                  toNtv2(config.taskMode) != NTV2_STANDARD_TASKS;
        routedTaskMode_ = true;
        if (!step(card_.SetTaskMode(toNtv2(config.taskMode)), "SetTaskMode"))
            return false;
        if (fromServices)
            std::this_thread::sleep_for(kServicesSettle);
    }

    if (!step(card_.SetNumberAudioChannels(config.channels, system_), "SetNumberAudioChannels") ||
        !step(card_.SetAudioRate(NTV2_AUDIO_48K, system_), "SetAudioRate(48 kHz)") ||
        !step(card_.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, system_), "SetAudioBufferSize(4 MB)") ||
        !step(card_.SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF, system_), "SetAudioLoopBack(off)"))
        return false;

    if (config.source != InputSource::Unchanged)
    {
        NTV2AudioSource source = NTV2_AUDIO_AES;
        NTV2EmbeddedAudioInput embedded = NTV2_EMBEDDED_AUDIO_INPUT_VIDEO_1;
        if (!toNtv2Source(config.source, config.embeddedInput, source, embedded))
        {
            error = std::string("the input source cannot be set to ") + toString(config.source);
            return false;
        }
        routedSelect_ = true;   // the SDK rewrites the shared select with any source
        if (!step(card_.SetAudioSystemInputSource(system_, source, embedded), "SetAudioSystemInputSource"))
            return false;
        // The SDK only sets this clock when it picks an embedded source; for
        // the Xpand's inputs it could still be on a video input.
        if ((config.source == InputSource::Aes || config.source == InputSource::Analog) &&
            !step(card_.SetEmbeddedAudioClock(NTV2_EMBEDDED_AUDIO_CLOCK_REFERENCE, system_),
                  "SetEmbeddedAudioClock(reference)"))
            return false;
        if (config.keepXpandSelect && saved_.haveXpand &&
            !step(writeSelectField(saved_.xpandSelect), "restoring the Xpand analog/AES select"))
            return false;
    }

    if (config.analogLevelDbu != 0)
    {
        ULWord code = 0;
        if (!levelCode(config.analogLevelDbu, code))
        {
            error = "the Xpand's analog level is +24, +18, +15 or +12 dBu, not " + std::to_string(config.analogLevelDbu);
            return false;
        }
        if (!saved_.haveLevel)
        {
            error = "the Xpand's analog level could not be read, so it would not be put back";
            return false;
        }
        routedLevel_ = true;
        if (!step(writeLevelField(code), "setting the Xpand's analog level"))
            return false;
    }
    if (config.routeAesOutputs && !routeAesOutputs(error))
        return false;
    if (config.routeMonitor && !routeMonitor(config.monitorPair, error))
        return false;
    for (unsigned n = 0; n < 32; ++n)
    {
        if ((config.sdiOutputs & (1u << n)) == 0)
            continue;
        const std::string name = "SDI out " + std::to_string(n + 1);
        if (n >= capabilities_.sdiOutputs || n >= CardRouting::kMaxSdiOutputs)
        {
            error = "the card has " + std::to_string(capabilities_.sdiOutputs) + " SDI outputs, not " + name;
            return false;
        }
        if (!saved_.haveSdi[n])
        {
            error = name + "'s audio could not be read, so it would not be put back";
            return false;
        }
        routedSdi_ |= 1u << n;
        if (!step(card_.SetSDIOutputAudioSystem(NTV2Channel(n), system_), "SetSDIOutputAudioSystem") ||
            !step(card_.SetSDIOutputAudioEnabled(NTV2Channel(n), true), "SetSDIOutputAudioEnabled"))
            return false;
    }
    if (config.hdmiOutput)
    {
        if (capabilities_.hdmiOutputs == 0)
        {
            error = "the card has no HDMI output";
            return false;
        }
        if (!saved_.haveHdmi)
        {
            error = "the HDMI output's audio could not be read, so it would not be put back";
            return false;
        }
        routedHdmi_ = true;
        if (!step(card_.SetHDMIOutAudioSource8Channel(NTV2_AudioChannel1_8, system_),
                  "SetHDMIOutAudioSource8Channel(1-8)"))
            return false;
    }

    if (!step(card_.SetAudioCaptureEnable(system_, config.captureEnable), "SetAudioCaptureEnable") ||
        !step(card_.SetAudioOutputEraseMode(system_, config.eraseMode), "SetAudioOutputEraseMode"))
        return false;
    if (card_.features().CanDoAudioDelay() &&
        (!step(card_.SetAudioInputDelay(system_, 0), "SetAudioInputDelay(0)") ||
         !step(card_.SetAudioOutputDelay(system_, 0), "SetAudioOutputDelay(0)")))
        return false;

    ULWord readOffset = 0, wrap = 0;
    if (!step(card_.GetAudioReadOffset(readOffset, system_), "GetAudioReadOffset") ||
        !step(card_.GetAudioWrapAddress(wrap, system_), "GetAudioWrapAddress"))
        return false;
    layout_.ringBytes = wrap;
    layout_.captureOffset = readOffset;
    layout_.frameBytes = 4u * config.channels;
    // Each audio system has twice the read offset (8 MB): the playback
    // ring's room ends where the capture ring starts, the capture ring's
    // where the next audio system's buffers do.
    layout_.roomBytes = readOffset;
    if (wrap == 0 || wrap % layout_.frameBytes != 0 || wrap > readOffset)
    {
        error = "unexpected buffer layout: wrap " + std::to_string(wrap) + ", capture offset " +
                std::to_string(readOffset) + ", frame " + std::to_string(layout_.frameBytes) + " bytes";
        return false;
    }
    // The shared settings as they are now are ours, until someone else sets them.
    haveOurs_ = false;
    takeAsOurs(routing::kAll);
    return true;
}

bool Ntv2CardAudio::setCaptureEnable(bool enable)
{
    markAsOurs();
    changed_ = true;
    return card_.SetAudioCaptureEnable(system_, enable);
}

bool Ntv2CardAudio::startOutput()
{
    markAsOurs();
    if (!card_.StartAudioOutput(system_))
        return false;
    startedOutput_ = true;
    return true;
}

bool Ntv2CardAudio::startInput()
{
    markAsOurs();
    if (!card_.StartAudioInput(system_))
        return false;
    startedInput_ = true;
    return true;
}

bool Ntv2CardAudio::stopOutput()
{
    const bool ok = card_.StopAudioOutput(system_);
    startedOutput_ = startedOutput_ && !ok;
    return ok;
}

bool Ntv2CardAudio::stopInput()
{
    const bool ok = card_.StopAudioInput(system_);
    startedInput_ = startedInput_ && !ok;
    return ok;
}

bool Ntv2CardAudio::ringRange(Ring ring, std::uint32_t offset, std::uint32_t bytes, ULWord& absolute) const
{
    if (layout_.ringBytes == 0 || bytes == 0 || std::uint64_t(offset) + bytes > layout_.roomBytes)
        return false;
    absolute = (ring == Ring::Capture ? layout_.captureOffset : 0u) + offset;
    return true;
}

bool Ntv2CardAudio::dmaRead(Ring ring, std::uint32_t offset, void* host, std::uint32_t bytes)
{
    ULWord at = 0;
    lastDmaSystemError_ = 0;
    if (host == nullptr || !ringRange(ring, offset, bytes, at))
        return false;
    clearSystemError();
    if (card_.DMAReadAudio(system_, static_cast<ULWord*>(host), at, bytes))
        return true;
    lastDmaSystemError_ = systemError();
    return false;
}

bool Ntv2CardAudio::dmaWrite(Ring ring, std::uint32_t offset, const void* host, std::uint32_t bytes)
{
    ULWord at = 0;
    lastDmaSystemError_ = 0;
    if (host == nullptr || !ringRange(ring, offset, bytes, at))
        return false;
    clearSystemError();
    if (card_.DMAWriteAudio(system_, static_cast<const ULWord*>(host), at, bytes))
        return true;
    lastDmaSystemError_ = systemError();
    return false;
}

bool Ntv2CardAudio::subscribe(CardEvent event)
{
    if (!card_.SubscribeEvent(toInterrupt(event)))
        return false;
    subscribed_.insert(event);
    return true;
}

bool Ntv2CardAudio::unsubscribe(CardEvent event)
{
    subscribed_.erase(event);
    return card_.UnsubscribeEvent(toInterrupt(event));
}

void Ntv2CardAudio::restore()
{
    if (restored_ || !card_.IsOpen() || system_ == NTV2_AUDIOSYSTEM_INVALID)
        return;
    restored_ = true;

    for (const CardEvent event : std::set<CardEvent>(subscribed_))
        unsubscribe(event);
    if (startedInput_)
        stopInput();
    if (startedOutput_)
        stopOutput();
    if (lockedAny_)
        card_.DMABufferUnlockAll();
    if (changed_)
        writeSettings(saved_);
    // Everything is back: nothing of this program's is left on the card.
    if (marked_)
    {
        platform::ownermarks::remove(markName_);
        marked_ = false;
    }
}

} // namespace

std::unique_ptr<CardAudio> openNtv2CardAudio(const std::string& selector, unsigned audioSystem, std::string& error,
                                             const std::string& pickAnother)
{
    auto card = std::make_unique<Ntv2CardAudio>();
    if (!card->open(selector, audioSystem, error, pickAnother))
        return nullptr;
    return card;
}

} // namespace ajaduplex::device
