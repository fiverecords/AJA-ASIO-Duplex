// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    DriverModule.cpp
 * @brief   The DLL's COM entry points: class factory and registration.
 *
 * regsvr32 calls DllRegisterServer, which writes, for the driver (see
 * DriverIdentity.h):
 *
 *   HKLM\SOFTWARE\Classes\CLSID\{clsid}                 (default) = name
 *   HKLM\SOFTWARE\Classes\CLSID\{clsid}\InprocServer32  (default) = this DLL,
 *                                                       ThreadingModel = Apartment
 *   HKLM\SOFTWARE\ASIO\<name>                           CLSID, Description
 *
 * which needs an elevated prompt; "regsvr32 /u" takes them away again.
 * The DLL registers itself where it is: install-driver.cmd runs regsvr32
 * on a copy it puts in Program Files (0.1.22), so that hosts do not depend
 * on the folder the driver came in.
 * Either way, what versions up to 0.1.19 registered for the simulated card
 * as a second driver goes too, if it is still there and still ours. The
 * settings under HKCU are left alone.
 *
 * The class on the simulated card is still made here for whoever asks the
 * DLL for it by CLSID (duplex-asiotest); with nothing registered for it,
 * no host does.
 */

#include "asio/win/AsioDriver.h"
#include "asio/win/DriverIdentity.h"

#include <windows.h>
#include <olectl.h>

#include <new>
#include <string>

using namespace ajaduplex::asio::win;

namespace
{

HINSTANCE gModule = nullptr;
std::atomic<long> gLocks{0};

class ClassFactory final : public IClassFactory
{
public:
    explicit ClassFactory(bool simulated) : simulated_(simulated) { gLiveObjects.fetch_add(1); }
    ~ClassFactory() { gLiveObjects.fetch_sub(1); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
        {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1) + 1; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = references_.fetch_sub(1) - 1;
        if (left == 0)
            delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;
        *object = nullptr;
        if (outer != nullptr)
            return CLASS_E_NOAGGREGATION;
        AsioDriver* driver = new (std::nothrow) AsioDriver(simulated_);
        if (driver == nullptr)
            return E_OUTOFMEMORY;
        const HRESULT result = driver->QueryInterface(riid, object);
        driver->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock)
            gLocks.fetch_add(1);
        else
            gLocks.fetch_sub(1);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    bool simulated_;
};

std::wstring modulePath()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(gModule, path, MAX_PATH);
    return std::wstring(path, length);
}

bool setString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(), bytes) == ERROR_SUCCESS;
}

HRESULT registerDriver(const wchar_t* clsidText, const wchar_t* name)
{
    const std::wstring classKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsidText;
    const std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + name;
    bool ok = setString(HKEY_LOCAL_MACHINE, classKey, nullptr, name);
    ok = ok && setString(HKEY_LOCAL_MACHINE, classKey + L"\\InprocServer32", nullptr, modulePath());
    ok = ok && setString(HKEY_LOCAL_MACHINE, classKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    ok = ok && setString(HKEY_LOCAL_MACHINE, asioKey, L"CLSID", clsidText);
    ok = ok && setString(HKEY_LOCAL_MACHINE, asioKey, L"Description", name);
    // Without rights to HKLM (no elevated prompt) every write fails alike.
    return ok ? S_OK : SELFREG_E_CLASS;
}

void unregisterDriver(const wchar_t* clsidText, const wchar_t* name)
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsidText).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + name).c_str());
}

/// What versions up to 0.1.19 registered for the simulated card as a second
/// driver: its class key, which is ours whoever wrote it (the CLSID is),
/// and its key under HKLM\SOFTWARE\ASIO under either of the names it had,
/// only while that key still names our class: another driver of that name
/// is not ours to take away.
void removeFormerSimulatedDriver()
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kSimulatedClsidText).c_str());
    for (const wchar_t* name : kFormerSimulatedNamesW)
    {
        const std::wstring key = std::wstring(L"SOFTWARE\\ASIO\\") + name;
        wchar_t clsid[64] = {};
        DWORD bytes = sizeof(clsid);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &bytes) ==
                ERROR_SUCCESS &&
            lstrcmpiW(clsid, kSimulatedClsidText) == 0)
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    }
}

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object)
{
    if (object == nullptr)
        return E_POINTER;
    *object = nullptr;
    // The class on the simulated card is not registered: only a program
    // that loads this DLL itself asks for it (duplex-asiotest).
    bool simulated = false;
    if (IsEqualCLSID(clsid, kSimulatedClsid))
        simulated = true;
    else if (!IsEqualCLSID(clsid, kDriverClsid))
        return CLASS_E_CLASSNOTAVAILABLE;
    ClassFactory* factory = new (std::nothrow) ClassFactory(simulated);
    if (factory == nullptr)
        return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(riid, object);
    factory->Release();
    return result;
}

STDAPI DllCanUnloadNow()
{
    return gLiveObjects.load() == 0 && gLocks.load() == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    removeFormerSimulatedDriver();
    const HRESULT result = registerDriver(kDriverClsidText, kDriverNameW);
    if (FAILED(result))
        unregisterDriver(kDriverClsidText, kDriverNameW);
    return result;
}

STDAPI DllUnregisterServer()
{
    removeFormerSimulatedDriver();
    unregisterDriver(kDriverClsidText, kDriverNameW);
    return S_OK;
}
