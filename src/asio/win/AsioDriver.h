// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AsioDriver.h
 * @brief   The COM object hosts create: IASIO on top of DriverCore.
 *
 * Hosts create it with CoCreateInstance, passing the CLSID as the IID too,
 * so QueryInterface takes either. Every IASIO call goes to DriverCore; what
 * is left here is COM's reference counting, the settings (HKCU), the
 * ASIO time base (timeGetTime, as the specification wants on Windows) and
 * the control panel.
 */

#pragma once

#include "asio/AsioSdk.h"

#include <windows.h>
#include <unknwn.h>

#include "iasiodrv.h"

#include <atomic>
#include <memory>
#include <string>

namespace ajaduplex
{
class Clock;
class Log;
}

namespace ajaduplex::asio
{
class CardOpener;
class DriverCore;
}

namespace ajaduplex::asio::win
{

class InstanceLock;
class TaskModeMark;

class AsioDriver final : public IASIO
{
public:
    explicit AsioDriver(bool simulated);
    ~AsioDriver();
    AsioDriver(const AsioDriver&) = delete;
    AsioDriver& operator=(const AsioDriver&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IASIO
    ASIOBool init(void* sysHandle) override;
    void getDriverName(char* name) override;
    long getDriverVersion() override;
    void getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                            ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

private:
    std::atomic<ULONG> references_{1};
    bool simulated_;
    HWND window_ = nullptr;
    DWORD initThread_ = 0;   ///< the thread that called init, for the log
    std::unique_ptr<Log> log_;   ///< before the core, which logs until its end
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<CardOpener> opener_;
    std::unique_ptr<DriverCore> core_;
    std::unique_ptr<InstanceLock> lock_;
    std::unique_ptr<TaskModeMark> taskModeMark_;   ///< set while the core answers for the card's task mode
    std::string lockError_;   ///< why init failed before the core was asked
};

/// Objects alive and LockServer locks, for DllCanUnloadNow.
extern std::atomic<long> gLiveObjects;

/// Where a driver writes its log: %LOCALAPPDATA%\AJA Duplex ASIO\driver.log,
/// or simulated.log on the simulated card (duplex-asiotest without --card).
std::wstring logPath(bool simulated);

} // namespace ajaduplex::asio::win
