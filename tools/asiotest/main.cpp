// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    main.cpp
 * @brief   duplex-asiotest: a small ASIO host for trying the driver.
 *
 * Loads AJADuplexASIO.dll (from beside this program, or --dll), creates the
 * driver the way a host does (or through COM with --registered), asks what
 * a host asks, runs it for a while and checks what came back:
 *
 *   - one callback per block, halves alternating, sample positions N apart
 *     and time stamps moving forward;
 *   - the round trip: output 1 plays a short click once a second; with
 *     output 1 looped back to input 1 (an XLR cable from out L to in L for
 *     the analog input, or AES out 1-2 to AES in 1-2 for AES; the
 *     simulated card is looped already) the click comes back, and the
 *     frames it took are compared with the latencies the driver reports,
 *     which is what hosts use to line up recordings;
 *   - the messages the driver sent (overloads, resync requests).
 *
 * By default it runs the simulated card, so it needs nothing but the DLL:
 * the DLL keeps a class for that which nothing registers, so no host lists
 * it, and this program asks the DLL for it itself. --card runs the real
 * one. Windows only.
 *
 * --installed runs nothing: it says in one line what hosts find of the
 * driver on this PC (asio/InstallState.h), since the DLL hosts load is the
 * copy that install-driver.cmd put in Program Files, not the one beside
 * this program.
 */

#include "asio/AsioSdk.h"
#include "asio/InstallState.h"

#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// The driver's two classes (src/asio/win/DriverIdentity.h).
constexpr CLSID kDriverClsid = {0x1423d042, 0xacd6, 0x4ff8, {0xad, 0x72, 0xa1, 0xa7, 0xad, 0x03, 0x60, 0x43}};
constexpr CLSID kSimulatedClsid = {0x88651c34, 0xa298, 0x43a1, {0x8d, 0xec, 0x33, 0xa1, 0x59, 0x56, 0x0e, 0x7f}};

constexpr std::int32_t kClick = 214748365;          // -20 dBFS: careful with monitors on the loop
constexpr std::int32_t kThreshold = 21474836;       // -40 dBFS
constexpr long kClickEvery = 48000;                 // once a second
constexpr long kClickAt = 12000;                    // a quarter of a second into each second

struct Options
{
    bool card = false;
    bool registered = false;
    bool installed = false;   // only say what hosts find of the driver
    std::wstring dll;
    double seconds = 5.0;
    long frames = 0;   // 0: the driver's preferred size
};

struct Session
{
    IASIO* driver = nullptr;
    long frames = 0;
    long inputs = 0;
    long outputs = 0;
    std::vector<ASIOBufferInfo> infos;

    // Written by the callbacks (the driver's thread), read after stop().
    std::vector<long> halves;
    std::vector<std::int64_t> positions;
    std::vector<std::int64_t> times;
    std::vector<std::int64_t> clicksSent;    // output frames
    std::vector<std::int64_t> clicksHeard;   // input frames
    std::int64_t outputFrame = 0;
    std::int64_t inputFrame = 0;
    bool aboveThreshold = false;
    std::atomic<long> overloads{0};
    std::atomic<long> resyncs{0};
    std::atomic<long> resets{0};
    std::atomic<long> latencyChanges{0};
};

Session gSession;

/// Ctrl+C, or the console window closed: the run ends early, so that the
/// driver is stopped and let go, and the card put back, as after a whole
/// run. Killed in the middle, this program would leave the card's audio
/// system running, and the next one to open it would find it in use.
std::atomic<bool> gInterrupted{false};

BOOL WINAPI onConsoleControl(DWORD type)
{
    gInterrupted.store(true);
    // A closing window ends the process when this returns, or about five
    // seconds later: time for the driver to be let go.
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        Sleep(10000);
    return TRUE;
}

std::int64_t hiLo(unsigned long hi, unsigned long lo)
{
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(hi) << 32) | (static_cast<std::uint64_t>(lo) & 0xFFFFFFFFu));
}

void processBlock(long index, const ASIOTime* time)
{
    Session& s = gSession;
    s.halves.push_back(index);
    if (time != nullptr)
    {
        s.positions.push_back(hiLo(time->timeInfo.samplePosition.hi, time->timeInfo.samplePosition.lo));
        s.times.push_back(hiLo(time->timeInfo.systemTime.hi, time->timeInfo.systemTime.lo));
    }
    else
    {
        ASIOSamples position{};
        ASIOTimeStamp stamp{};
        if (s.driver->getSamplePosition(&position, &stamp) == ASE_OK)
        {
            s.positions.push_back(hiLo(position.hi, position.lo));
            s.times.push_back(hiLo(stamp.hi, stamp.lo));
        }
    }
    for (const ASIOBufferInfo& b : s.infos)
    {
        auto* samples = static_cast<std::int32_t*>(b.buffers[index]);
        if (b.isInput && b.channelNum == 0)
        {
            for (long f = 0; f < s.frames; ++f)
            {
                const bool above = samples[f] > kThreshold || samples[f] < -kThreshold;
                if (above && !s.aboveThreshold)
                    s.clicksHeard.push_back(s.inputFrame + f);
                s.aboveThreshold = above;
            }
        }
        else if (!b.isInput)
        {
            std::memset(samples, 0, static_cast<std::size_t>(s.frames) * sizeof(std::int32_t));
            if (b.channelNum == 0)
            {
                for (long f = 0; f < s.frames; ++f)
                {
                    if ((s.outputFrame + f) % kClickEvery == kClickAt)
                    {
                        samples[f] = kClick;
                        s.clicksSent.push_back(s.outputFrame + f);
                    }
                }
            }
        }
    }
    s.inputFrame += s.frames;
    s.outputFrame += s.frames;
    s.driver->outputReady();
}

void bufferSwitch(long index, ASIOBool)
{
    processBlock(index, nullptr);
}

ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool)
{
    processBlock(index, time);
    return time;
}

void sampleRateDidChange(ASIOSampleRate) {}

long asioMessage(long selector, long value, void*, double*)
{
    switch (selector)
    {
        case kAsioSelectorSupported:
            return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
                           value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioOverload
                       ? 1
                       : 0;
        case kAsioEngineVersion: return 2;
        case kAsioSupportsTimeInfo: return 1;
        case kAsioResetRequest: gSession.resets++; return 1;
        case kAsioResyncRequest: gSession.resyncs++; return 1;
        case kAsioLatenciesChanged: gSession.latencyChanges++; return 1;
        case kAsioOverload: gSession.overloads++; return 1;
        default: return 0;
    }
}

const char* errorName(ASIOError e)
{
    switch (e)
    {
        case ASE_OK: return "ASE_OK";
        case ASE_SUCCESS: return "ASE_SUCCESS";
        case ASE_NotPresent: return "ASE_NotPresent";
        case ASE_HWMalfunction: return "ASE_HWMalfunction";
        case ASE_InvalidParameter: return "ASE_InvalidParameter";
        case ASE_InvalidMode: return "ASE_InvalidMode";
        case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
        case ASE_NoClock: return "ASE_NoClock";
        case ASE_NoMemory: return "ASE_NoMemory";
        default: return "?";
    }
}

/// The driver's log, where src/asio/win/AsioDriver.cpp puts it.
std::wstring driverLog(bool card)
{
    wchar_t folder[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        length = GetTempPathW(MAX_PATH, folder);
    std::wstring path(folder, length);
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    return path + L"AJA Duplex ASIO\\" + (card ? L"driver.log" : L"simulated.log");
}

std::string utf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                            nullptr);
    return out;
}

std::wstring besideThisProgram(const wchar_t* file)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder(path, length);
    const std::size_t slash = folder.find_last_of(L"\\/");
    folder = slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash + 1);
    return folder + file;
}

bool parse(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& value) {
            if (i + 1 >= argc)
                return false;
            value = argv[++i];
            return true;
        };
        std::wstring value;
        if (a == L"--card")
            options.card = true;
        else if (a == L"--simulated")
            options.card = false;
        else if (a == L"--registered")
            options.registered = true;
        else if (a == L"--installed")
            options.installed = true;
        else if (a == L"--dll" && next(value))
            options.dll = value;
        else if (a == L"--seconds" && next(value))
            options.seconds = _wtof(value.c_str());
        else if (a == L"--frames" && next(value))
            options.frames = _wtol(value.c_str());
        else
            return false;
    }
    return options.seconds > 0.0 && options.seconds <= 3600.0;
}

void usage()
{
    std::printf(
        "duplex-asiotest: tries AJA Duplex ASIO the way a host does.\n\n"
        "  duplex-asiotest [--card] [--registered] [--dll <path>] [--seconds <s>] [--frames <n>]\n"
        "  duplex-asiotest --installed\n\n"
        "  --simulated   the simulated card (default): needs nothing but the DLL\n"
        "  --card        the KONA X; loop output 1 to input 1 to measure the round trip\n"
        "                (XLR out L to in L for the analog input). The click is -20 dBFS:\n"
        "                keep monitors off that output\n"
        "  --registered  with --card: create the driver through COM, as hosts do (needs\n"
        "                install-driver.cmd first; the simulated card is not registered)\n"
        "  --dll         the DLL to load (default: AJADuplexASIO.dll beside this program)\n"
        "  --seconds     how long to run (default 5)\n"
        "  --frames      buffer size (default: the driver's preferred one)\n"
        "  --installed   run nothing: say which DLL hosts load (the copy install-driver.cmd\n"
        "                put in Program Files) and whether it is the build beside this\n"
        "                program; exit code 0 if it is, 1 if there is something to install\n");
}

/// A text value under HKEY_LOCAL_MACHINE ("" if it is not there), in this
/// program's own view of the registry: the 64-bit one, as the hosts' is.
std::wstring machineText(const std::wstring& key, const wchar_t* value)
{
    wchar_t text[1024] = {};
    DWORD bytes = static_cast<DWORD>(sizeof(text) - sizeof(wchar_t));   // the last one stays 0
    // RRF_RT_REG_SZ alone also takes a REG_EXPAND_SZ value, expanded; asking
    // for REG_EXPAND_SZ by name without RRF_NOEXPAND is refused.
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, text, &bytes) != ERROR_SUCCESS)
        return std::wstring();
    return std::wstring(text);
}

/// A whole file, also one that a host has loaded.
bool readWhole(const std::wstring& path, std::vector<char>& bytes)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0 && size.QuadPart < (LONGLONG{1} << 30);
    if (ok)
    {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0 &&
                               read == static_cast<DWORD>(bytes.size()));
    }
    CloseHandle(file);
    return ok;
}

/// When a file was last written, in local time ("" if it cannot be told).
std::string writtenAt(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToSystemTime(&data.ftLastWriteTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return std::string();
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", static_cast<unsigned>(local.wYear),
                  static_cast<unsigned>(local.wMonth), static_cast<unsigned>(local.wDay),
                  static_cast<unsigned>(local.wHour), static_cast<unsigned>(local.wMinute));
    return text;
}

/// --installed: what hosts find of the driver on this PC, in one line. They
/// list what is under HKLM\SOFTWARE\ASIO and load the DLL its class is
/// registered with: the copy install-driver.cmd put in Program Files, or,
/// up to 0.1.21, the DLL where it was when it was registered. The DLL
/// beside this program says whether that is this build.
int reportInstalled()
{
    wchar_t clsid[64] = {};
    StringFromGUID2(kDriverClsid, clsid, 64);
    const std::wstring listedClass = machineText(L"SOFTWARE\\ASIO\\AJA Duplex ASIO", L"CLSID");
    const std::wstring server =
        machineText(std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid + L"\\InprocServer32", nullptr);
    const std::wstring beside = besideThisProgram(L"AJADuplexASIO.dll");

    ajaduplex::asio::InstallFacts facts;
    facts.listed = lstrcmpiW(listedClass.c_str(), clsid) == 0;
    facts.server = utf8(server);
    facts.serverThere = !server.empty() && GetFileAttributesW(server.c_str()) != INVALID_FILE_ATTRIBUTES;
    facts.besideThere = GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (facts.serverThere)
    {
        facts.serverWritten = writtenAt(server);
        std::vector<char> installed;
        std::vector<char> mine;
        facts.sameAsBeside =
            facts.besideThere && readWhole(server, installed) && readWhole(beside, mine) && installed == mine;
    }
    const ajaduplex::asio::InstallVerdict verdict = ajaduplex::asio::judgeInstall(facts, "AJA Duplex ASIO");
    std::printf("%s\n", verdict.line.c_str());
    return verdict.ok ? 0 : 1;
}

IASIO* createDriver(const Options& options, HMODULE& module, std::string& error)
{
    const CLSID& clsid = options.card ? kDriverClsid : kSimulatedClsid;
    void* object = nullptr;
    if (options.registered && !options.card)
    {
        // Since 0.1.20 only the card's driver is registered.
        error = "--registered goes with --card: the simulated card is in the DLL for this test only, "
                "and nothing registers it";
        return nullptr;
    }
    if (options.registered)
    {
        const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &object);
        if (FAILED(hr))
        {
            char text[64];
            std::snprintf(text, sizeof(text), "CoCreateInstance failed: 0x%08lX", static_cast<unsigned long>(hr));
            error = std::string(text) + " (duplex-asiotest --installed says what hosts find of the driver)";
            return nullptr;
        }
        return static_cast<IASIO*>(object);
    }
    // What it needs, from beside it too, as COM loads a registered driver.
    const std::wstring path = options.dll.empty() ? besideThisProgram(L"AJADuplexASIO.dll") : options.dll;
    module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr)
    {
        const DWORD code = GetLastError();
        const bool there = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        error = "the driver could not be loaded from " + utf8(path) + " (error " + std::to_string(code) + "): " +
                (!there      ? "there is no such file; put duplex-asiotest.exe beside AJADuplexASIO.dll, give "
                               "--dll <path>, or --registered to load the one hosts load"
                 : code == 126 ? "the file is there, but a DLL it needs is not found"
                 : code == 193 ? "the file is there, but not a 64-bit DLL"
                               : "the file is there");
        return nullptr;
    }
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto getClassObject =
        reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
    if (getClassObject == nullptr)
    {
        error = "the DLL has no DllGetClassObject";
        return nullptr;
    }
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr)
    {
        error = "DllGetClassObject failed";
        return nullptr;
    }
    hr = factory->CreateInstance(nullptr, clsid, &object);
    factory->Release();
    if (FAILED(hr))
    {
        error = "CreateInstance failed";
        return nullptr;
    }
    return static_cast<IASIO*>(object);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!parse(argc, argv, options))
    {
        usage();
        return 2;
    }
    if (options.installed)
        return reportInstalled();
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE module = nullptr;
    std::string error;
    IASIO* driver = createDriver(options, module, error);
    if (driver == nullptr)
    {
        std::printf("FAIL  %s\n", error.c_str());
        return 1;
    }
    Session& s = gSession;
    s.driver = driver;
    int failures = 0;
    auto check = [&failures](bool ok, const std::string& what) {
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok)
            ++failures;
    };

    if (driver->init(GetConsoleWindow()) != ASIOTrue)
    {
        char message[124] = {};
        driver->getErrorMessage(message);
        std::printf("FAIL  init: %s\n", message);
        driver->Release();
        std::printf("log         %ls\n", driverLog(options.card).c_str());
        return 1;
    }
    char name[32] = {};
    driver->getDriverName(name);
    std::printf("driver      %s (version %ld)\n", name, driver->getDriverVersion());

    check(driver->getChannels(&s.inputs, &s.outputs) == ASE_OK && s.inputs > 0 && s.outputs > 0, "channels");
    std::printf("            %ld inputs, %ld outputs\n", s.inputs, s.outputs);
    std::string firstInput;
    for (int input = 1; input >= 0; --input)
    {
        const long count = input ? s.inputs : s.outputs;
        std::string names;
        for (long c = 0; c < count; ++c)
        {
            ASIOChannelInfo info{};
            info.channel = c;
            info.isInput = input ? ASIOTrue : ASIOFalse;
            if (driver->getChannelInfo(&info) == ASE_OK)
            {
                names += (names.empty() ? "" : ", ") + std::string(info.name);
                if (input && c == 0)
                    firstInput = info.name;
            }
            if (info.type != ASIOSTInt32LSB)
                check(false, "channel type is ASIOSTInt32LSB");
        }
        std::printf("            %s: %s\n", input ? "in " : "out", names.c_str());
    }

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    check(driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK, "buffer sizes");
    std::printf("            min %ld, max %ld, preferred %ld, granularity %ld\n", minSize, maxSize, preferred,
                granularity);
    check(driver->canSampleRate(48000.0) == ASE_OK, "48000 Hz");
    check(driver->canSampleRate(44100.0) == ASE_NoClock, "44100 Hz refused");
    ASIOClockSource clocks[4]{};
    long clockCount = 4;
    check(driver->getClockSources(clocks, &clockCount) == ASE_OK && clockCount == 1, "one clock source");
    std::printf("            clock: %s\n", clocks[0].name);
    check(driver->future(kAsioCanTimeInfo, nullptr) == ASE_SUCCESS, "time info");
    check(driver->future(kAsioCanReportOverload, nullptr) == ASE_SUCCESS, "overload reports");

    s.frames = options.frames > 0 ? options.frames : preferred;
    for (long c = 0; c < s.inputs; ++c)
        s.infos.push_back(ASIOBufferInfo{ASIOTrue, c, {nullptr, nullptr}});
    for (long c = 0; c < s.outputs; ++c)
        s.infos.push_back(ASIOBufferInfo{ASIOFalse, c, {nullptr, nullptr}});
    ASIOCallbacks callbacks{bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo};
    const ASIOError created =
        driver->createBuffers(s.infos.data(), static_cast<long>(s.infos.size()), s.frames, &callbacks);
    check(created == ASE_OK, std::string("createBuffers at N = ") + std::to_string(s.frames) + ": " + errorName(created));
    if (created != ASE_OK)
    {
        driver->Release();
        return 1;
    }
    long inputLatency = 0, outputLatency = 0;
    check(driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK, "latencies");
    std::printf("            in %ld, out %ld frames (%.2f + %.2f ms)\n", inputLatency, outputLatency,
                inputLatency / 48.0, outputLatency / 48.0);

    const long expectedBlocks = static_cast<long>(options.seconds * 48000.0 / static_cast<double>(s.frames));
    s.halves.reserve(static_cast<std::size_t>(expectedBlocks) * 2 + 64);
    s.positions.reserve(static_cast<std::size_t>(expectedBlocks) * 2 + 64);
    s.times.reserve(static_cast<std::size_t>(expectedBlocks) * 2 + 64);
    s.clicksSent.reserve(static_cast<std::size_t>(options.seconds) + 8);
    s.clicksHeard.reserve(static_cast<std::size_t>(options.seconds) * 4 + 64);

    const ASIOError started = driver->start();
    check(started == ASE_OK, std::string("start: ") + errorName(started));
    if (started != ASE_OK)
    {
        char message[124] = {};
        driver->getErrorMessage(message);
        std::printf("            %s\n", message);
        driver->disposeBuffers();
        driver->Release();
        return 1;
    }
    std::printf("run         %.1f s ... (Ctrl+C ends it)\n", options.seconds);
    const DWORD runFrom = GetTickCount();
    const DWORD runMs = static_cast<DWORD>(options.seconds * 1000.0);
    while (!gInterrupted.load() && GetTickCount() - runFrom < runMs)
        Sleep(20);
    check(driver->stop() == ASE_OK, "stop");
    if (gInterrupted.load())
    {
        // Nothing to check in a run cut short; the driver goes as after a whole one.
        driver->disposeBuffers();
        driver->Release();
        std::printf("----  interrupted: the driver was stopped and let go, and nothing was checked\n");
        return 1;
    }

    // Callbacks: one per block, halves alternating, positions N apart.
    const long blocks = static_cast<long>(s.halves.size());
    check(blocks > expectedBlocks * 9 / 10 && blocks < expectedBlocks * 11 / 10 + 2,
          std::to_string(blocks) + " callbacks, about " + std::to_string(expectedBlocks) + " expected");
    bool alternate = true;
    for (std::size_t i = 0; i < s.halves.size(); ++i)
        alternate = alternate && s.halves[i] == static_cast<long>(i % 2);
    check(alternate, "halves alternate from 0");
    long jumps = 0;
    bool forward = true;
    for (std::size_t i = 1; i < s.positions.size(); ++i)
    {
        if (s.positions[i] - s.positions[i - 1] != s.frames)
            ++jumps;
        forward = forward && s.times[i] >= s.times[i - 1];
    }
    check(!s.positions.empty() && s.positions.front() == 0, "the first sample position is 0");
    check(jumps == s.resyncs.load(), "sample positions N apart (" + std::to_string(jumps) + " jumps, " +
                                         std::to_string(s.resyncs.load()) + " resync requests)");
    check(forward, "time stamps never go back");
    if (s.times.size() > 10)
    {
        const double ms = static_cast<double>(s.times.back() - s.times[5]) / 1e6;
        const double expectedMs = static_cast<double>(s.positions.back() - s.positions[5]) / 48.0;
        check(std::fabs(ms - expectedMs) < expectedMs * 0.01 + 20.0,
              "time stamps follow the sample positions (" + std::to_string(ms) + " ms for " +
                  std::to_string(expectedMs) + " ms of audio)");
    }

    // The round trip: each click sent, the first one heard after it.
    std::vector<std::int64_t> trips;
    for (const std::int64_t sent : s.clicksSent)
    {
        for (const std::int64_t heard : s.clicksHeard)
        {
            if (heard > sent)
            {
                if (heard - sent < kClickEvery / 2)
                    trips.push_back(heard - sent);
                break;
            }
        }
    }
    const long reported = inputLatency + outputLatency;
    // An embedded input has no loop from output 1 here.
    const bool embeddedInput = firstInput.compare(0, 3, "SDI") == 0 || firstInput.compare(0, 4, "HDMI") == 0;
    if (embeddedInput)
    {
        std::printf("----  input 1 is %s: there is no loop from output 1 to measure the round trip with\n"
                    "      (pick the analog or the AES input in the settings to measure it)\n",
                    firstInput.c_str());
    }
    else if (trips.empty())
    {
        std::printf("----  no click came back on input 1: is output 1 looped to input 1?\n");
        if (!options.card)
            check(false, "the simulated card is looped: the click must come back");
    }
    else
    {
        const std::int64_t shortest = *std::min_element(trips.begin(), trips.end());
        const std::int64_t longest = *std::max_element(trips.begin(), trips.end());
        std::printf("round trip  %lld to %lld frames over %llu clicks; the driver reports %ld (in %ld + out %ld)\n",
                    static_cast<long long>(shortest), static_cast<long long>(longest),
                    static_cast<unsigned long long>(trips.size()), reported,
                    inputLatency, outputLatency);
        // On the simulated card every click takes the same frames. Through
        // converters (or the AES inputs' resampler) a click comes back spread
        // over a few frames, and the first of them over the threshold may be
        // one frame or the next: two frames apart is the same round trip.
        const std::int64_t spread = options.card ? 2 : 0;
        check(longest - shortest <= spread,
              spread == 0 ? std::string("the round trip holds")
                          : "the round trip holds (within " + std::to_string(spread) + " frames: converters spread a click)");
        // The design's target: reported latencies within 1 ms of the real ones.
        check(std::llabs(static_cast<long long>(shortest - reported)) <= 48,
              "reported latencies within 1 ms of the round trip (" +
                  std::to_string(static_cast<long long>(shortest - reported)) + " frames apart)");
    }
    std::printf("messages    %ld overloads, %ld resync requests, %ld reset requests, %ld latency changes\n",
                s.overloads.load(), s.resyncs.load(), s.resets.load(), s.latencyChanges.load());

    check(driver->disposeBuffers() == ASE_OK, "disposeBuffers");
    driver->Release();
    if (module != nullptr)
    {
        using CanUnload = HRESULT(STDAPICALLTYPE*)();
        const auto canUnload =
            reinterpret_cast<CanUnload>(reinterpret_cast<void*>(GetProcAddress(module, "DllCanUnloadNow")));
        check(canUnload != nullptr && canUnload() == S_OK, "the DLL can unload once released");
        FreeLibrary(module);
    }
    CoUninitialize();
    std::printf("log         %ls\n", driverLog(options.card).c_str());
    std::printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
