// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "asio/win/AsioDriver.h"

#include "DuplexVersion.h"
#include "asio/CardOpeners.h"
#include "asio/DriverCore.h"
#include "asio/win/DriverIdentity.h"
#include "asio/win/InstanceLock.h"
#include "asio/win/RegistryStore.h"
#include "common/Log.h"
#include "common/TextFormat.h"
#include "ui/SettingsDialog.h"
#include "platform/Realtime.h"

#include <mmsystem.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace ajaduplex::asio::win
{

std::atomic<long> gLiveObjects{0};

namespace
{

void copyText(char* target, std::size_t size, const std::string& text)
{
    if (target == nullptr || size == 0)
        return;
    const std::size_t n = text.size() < size - 1 ? text.size() : size - 1;
    std::memcpy(target, text.data(), n);
    target[n] = '\0';
}

/// "Now" in timeGetTime's base, in ns, taken right at one of its ticks so
/// the offset to the performance counter is good to a few microseconds
/// rather than to a tick (1 ms, or 15.6 ms when nothing has asked for more).
std::int64_t timeGetTimeNs()
{
    const DWORD first = timeGetTime();
    const DWORD deadline = GetTickCount() + 40;
    for (;;)
    {
        const DWORD now = timeGetTime();
        if (now != first || static_cast<LONG>(GetTickCount() - deadline) > 0)
            return static_cast<std::int64_t>(now) * 1000000;
    }
}

constexpr std::uintmax_t kLogBytes = 2 * 1024 * 1024;   // then <name>.old, and a new one

std::string thisProcess()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring name(path, length);
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        name = name.substr(slash + 1);
    std::string narrow;
    for (const wchar_t c : name)
        narrow += c < 128 ? static_cast<char>(c) : '?';
    return narrow + " (process " + std::to_string(GetCurrentProcessId()) + ")";
}

/// A count of samples (frames, in ASIO's words) as people read it: "592
/// samples (12.3 ms)".
std::string samples(long count)
{
    const std::uint64_t n = count > 0 ? static_cast<std::uint64_t>(count) : 0;
    return std::to_string(n) + " samples (" + text::milliseconds(n) + ")";
}

} // namespace

std::wstring logPath(bool simulated)
{
    wchar_t folder[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        length = GetTempPathW(MAX_PATH, folder);
    std::wstring path(folder, length);
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    return path + L"AJA Duplex ASIO\\" + (simulated ? L"simulated.log" : L"driver.log");
}

AsioDriver::AsioDriver(bool simulated) : simulated_(simulated)
{
    gLiveObjects.fetch_add(1);
    log_ = std::make_unique<Log>(std::make_unique<FileLogSink>(std::filesystem::path(logPath(simulated_)), kLogBytes));
    log_->linef("loaded by %s", thisProcess().c_str());
    clock_ = std::make_unique<platform::SystemClock>();
    if (simulated_)
        opener_ = std::make_unique<SimulatedOpener>(*clock_);
    else
        opener_ = std::make_unique<Ntv2Opener>();
    DriverOptions options;
    options.name = simulated_ ? kSimulatedName : kDriverName;
    options.threaded = true;
    options.proAudioPriority = true;
    options.systemTimeNs = timeGetTimeNs;
    options.log = log_.get();
    if (!simulated_)
    {
        // While the core answers for the card's task mode, a mark other
        // programs on the card can see (the simulated card is nobody
        // else's).
        taskModeMark_ = std::make_unique<TaskModeMark>();
        options.taskModeHeld = [this](bool held, unsigned card) {
            if (held)
                taskModeMark_->set(card);
            else
                taskModeMark_->clear();
        };
    }
    core_ = std::make_unique<DriverCore>(*opener_, *clock_, options);
}

AsioDriver::~AsioDriver()
{
    core_.reset();   // stops, frees the buffers, puts the card back
    taskModeMark_.reset();   // cleared by the core as it let the card go
    lock_.reset();   // and only then lets the next program have it
    opener_.reset();
    clock_.reset();
    if (log_)
        log_->line("released");
    log_.reset();    // writes what is left and ends its thread
    gLiveObjects.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE AsioDriver::QueryInterface(REFIID riid, void** object)
{
    if (object == nullptr)
        return E_POINTER;
    // Hosts pass the driver's CLSID as the IID; IASIO has no IID of its own.
    const CLSID& own = simulated_ ? kSimulatedClsid : kDriverClsid;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, own))
    {
        *object = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AsioDriver::AddRef()
{
    return references_.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE AsioDriver::Release()
{
    const ULONG left = references_.fetch_sub(1) - 1;
    if (left == 0)
        delete this;
    return left;
}

// ---------------------------------------------------------------------------
// IASIO
// ---------------------------------------------------------------------------

ASIOBool AsioDriver::init(void* sysHandle)
{
    window_ = static_cast<HWND>(sysHandle);
    initThread_ = GetCurrentThreadId();
    RegistryStore store(HKEY_CURRENT_USER, kSettingsKey);
    const DriverSettings settings = loadDriverSettings(store);
    lockError_.clear();
    if (!simulated_)
    {
        // One program at a time on the card's audio system (the simulated
        // card is the program's own).
        if (!lock_)
            lock_ = std::make_unique<InstanceLock>();
        std::string holder;
        if (!lock_->acquire(settings.card, settings.audioSystem, holder))
        {
            lockError_ = "AJA Duplex ASIO is already open in " + holder + " on this card and audio system.";
            log_->linef("init refused: %s", lockError_.c_str());
            return ASIOFalse;
        }
    }
    if (core_->init(settings))
        return ASIOTrue;
    if (lock_)
        lock_->release();   // a driver that did not open holds nothing
    return ASIOFalse;
}

void AsioDriver::getDriverName(char* name)
{
    copyText(name, 32, core_->driverName());
}

long AsioDriver::getDriverVersion()
{
    return core_->driverVersion();
}

void AsioDriver::getErrorMessage(char* string)
{
    copyText(string, 124, lockError_.empty() ? core_->errorMessage() : lockError_);
}

ASIOError AsioDriver::start() { return core_->start(); }
ASIOError AsioDriver::stop() { return core_->stop(); }

ASIOError AsioDriver::getChannels(long* numInputChannels, long* numOutputChannels)
{
    return core_->getChannels(numInputChannels, numOutputChannels);
}

ASIOError AsioDriver::getLatencies(long* inputLatency, long* outputLatency)
{
    return core_->getLatencies(inputLatency, outputLatency);
}

ASIOError AsioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity)
{
    return core_->getBufferSize(minSize, maxSize, preferredSize, granularity);
}

ASIOError AsioDriver::canSampleRate(ASIOSampleRate sampleRate) { return core_->canSampleRate(sampleRate); }
ASIOError AsioDriver::getSampleRate(ASIOSampleRate* sampleRate) { return core_->getSampleRate(sampleRate); }
ASIOError AsioDriver::setSampleRate(ASIOSampleRate sampleRate) { return core_->setSampleRate(sampleRate); }

ASIOError AsioDriver::getClockSources(ASIOClockSource* clocks, long* numSources)
{
    return core_->getClockSources(clocks, numSources);
}

ASIOError AsioDriver::setClockSource(long reference) { return core_->setClockSource(reference); }

ASIOError AsioDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp)
{
    return core_->getSamplePosition(sPos, tStamp);
}

ASIOError AsioDriver::getChannelInfo(ASIOChannelInfo* info) { return core_->getChannelInfo(info); }

ASIOError AsioDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks* callbacks)
{
    return core_->createBuffers(bufferInfos, numChannels, bufferSize, callbacks);
}

ASIOError AsioDriver::disposeBuffers() { return core_->disposeBuffers(); }

ASIOError AsioDriver::controlPanel()
{
    RegistryStore store(HKEY_CURRENT_USER, kSettingsKey);
    const DriverSettings stored = loadDriverSettings(store);   // what the next open uses

    // What the driver is doing, for the dialog's read-only box.
    const std::string version = core_->driverName() + " " + AJADUPLEX_VERSION_STRING;
    std::string status = version;
    if (simulated_)
        status += ", on the simulated card: outputs 1-2 come back on the inputs";
    status += ".\r\n";
    if (core_->initialized())
    {
        long inputLatency = 0, outputLatency = 0;
        core_->getLatencies(&inputLatency, &outputLatency);
        status += "Open: " + std::to_string(core_->channels().inputs.size()) + " inputs (" +
                  describeInput(core_->settings()) + "), " + std::to_string(core_->channels().outputs.size()) +
                  " outputs (" + describeOutputs(core_->settings()) + "); latencies reported: in " +
                  samples(inputLatency) + ", out " + samples(outputLatency) + ".\r\n";
        if (!core_->sdiNote().empty())
            status += core_->sdiNote() + "\r\n";
        if (core_->running())
            status += "Running.\r\n";
        const engine::EngineStats s = core_->stats();
        status += "This run: " + std::to_string(s.blocks) + " blocks, " + std::to_string(s.lateOutputs) +
                  " late outputs (" + std::to_string(s.droppedOutputFrames) + " samples dropped), " +
                  std::to_string(s.inputOverruns) + " overruns, " + std::to_string(s.stalls) + " stalls, " +
                  std::to_string(s.outputMoves) + " output moves, " + std::to_string(s.dmaErrors) +
                  " DMA errors (" + std::to_string(s.dmaRecovered) + " more recovered).\r\n";
        const DriverCore::RoutingStatus r = core_->routingStatus();
        if (r.watched)
        {
            // The design's coexistence rules: what other programs did to
            // the routing the driver set up.
            status += "Routing, read back once a second: ";
            if (r.changes == 0 && r.notOurs == 0)
                status += "as the driver set it";
            else
                status += std::to_string(r.changes) + " changes by other programs, " + std::to_string(r.putBack) +
                          " put back";
            if (r.notOurs != 0)
                status += "; not the driver's now: " + engine::routingPartNames(r.notOurs) + " (see the log)";
            status += ". Xpand analog level: " +
                      (r.analogLevelDbu != 0 ? "+" + std::to_string(r.analogLevelDbu) + " dBu" : std::string("unknown")) +
                      ". Card task mode: " + std::string(device::toString(r.taskMode)) + ".\r\n";
        }
    }
    else if (!lockError_.empty() || !core_->errorMessage().empty())
    {
        status += "Not open: " + (lockError_.empty() ? core_->errorMessage() : lockError_) + "\r\n";
    }
    else
    {
        // Asked for the settings without opening the driver: from
        // AJADuplexASIOSettings.exe, for a host with no settings button of
        // its own, or by a host before it opened the driver. What is saved
        // is for the next open, in this program or in the one that has the
        // driver now.
        status += "Not open in this program. The settings are kept for this Windows user: each program takes "
                  "them when it opens the driver.\r\n";
        std::string holder;
        if (!simulated_ && InstanceLock::heldBy(stored.card, stored.audioSystem, holder))
            status += "Open now in " + holder + ": it takes them when it opens the driver again (close it and "
                      "open it again).\r\n";
        else if (!simulated_)
            status += "No program has it open now.\r\n";
    }

    const std::wstring log = logPath(simulated_);
    const std::size_t slash = log.find_last_of(L'\\');
    ui::DialogContent content;
    content.title = widen(core_->driverName());
    content.settings = stored;
    content.latenciesFromCard = simulated_;
    content.status = widen(status) + L"Log: " + log;
    content.logFolder = slash == std::wstring::npos ? std::wstring() : log.substr(0, slash);
    content.about = widen(version + ", " + AJADUPLEX_RELEASE_DATE + ", by " + AJADUPLEX_AUTHOR);
    content.homepage = widen(AJADUPLEX_HOMEPAGE);
    // Over the window the user clicked in when that is this thread's: hosts
    // differ in what they give init, and in which thread calls this.
    const HWND owner = ui::chooseDialogOwner(window_);
    log_->linef("panel: %s, %s",
                initThread_ == 0                       ? "with the driver never opened (outside a host)"
                : GetCurrentThreadId() == initThread_ ? "on init's thread"
                                                        : "on another thread",
                owner == nullptr     ? "on its own (no window of this thread to go over)"
                : owner == window_   ? "over the window init was given"
                                     : "over the window the user clicked in");
    const ui::DialogResult result = ui::runSettingsDialog(owner, content);
    if (result == ui::DialogResult::AlreadyOpen)
        log_->linef("panel: already open; brought to the front");
    if (result != ui::DialogResult::Saved)
        return ASE_OK;

    if (!saveDriverSettings(store, content.settings))
    {
        MessageBoxW(owner, L"The settings could not be saved to HKEY_CURRENT_USER\\Software\\AJA Duplex ASIO.",
                    content.title.c_str(), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return ASE_OK;
    }
    if (core_->settingsChanged(content.settings) != DriverCore::SettingsApplied::NextOpen)
        return ASE_OK;
    std::wstring said = L"Saved. They take effect the next time the host opens the driver (or resets it from "
                        L"its audio settings).";
    if (!core_->initialized())
    {
        // Not open here: the program that has it, if one has, is the one
        // that waits for them (the lock is named as that program opened it,
        // with the settings stored before these).
        std::string holder;
        if (!simulated_ && InstanceLock::heldBy(stored.card, stored.audioSystem, holder))
            said = L"Saved. " + widen(holder) + L" has the driver open: close it and open it again to use them.";
        else
            said = L"Saved. The next program to open the driver takes them.";
        const std::string where = holder.empty() ? std::string("no program has the driver open") : "open in " + holder;
        log_->linef("panel: saved outside a host; %s", where.c_str());
    }
    MessageBoxW(owner, said.c_str(), content.title.c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return ASE_OK;
}

ASIOError AsioDriver::future(long selector, void* opt) { return core_->future(selector, opt); }
ASIOError AsioDriver::outputReady() { return core_->outputReady(); }

} // namespace ajaduplex::asio::win
