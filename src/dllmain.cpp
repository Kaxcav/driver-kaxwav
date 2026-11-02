#include <windows.h>
#include <unknwn.h>
#include <olectl.h>  // SELFREG_E_CLASS
#include <string>
#include "AsioDriver.h"

// CLSID: {77DBBCCD-91C6-4856-A900-FC8D007D161C}
const GUID CLSID_BehringerUM2AsioDriver = 
    { 0x77dbbccd, 0x91c6, 0x4856, { 0xa9, 0x00, 0xfc, 0x8d, 0x00, 0x7d, 0x16, 0x1c } };

static const wchar_t* CLSID_STRING = L"{77DBBCCD-91C6-4856-A900-FC8D007D161C}";
static const wchar_t* DRIVER_NAME = L"Behringer U-Phoria ASIO";
static const wchar_t* DRIVER_NAME_LEGACY = L"Behringer UM2 ASIO";
static const wchar_t* DRIVER_DESC = L"Behringer U-Phoria Low Latency ASIO Driver";

static HINSTANCE g_hInstance = nullptr;
static LONG g_serverLocks = 0;

class BehringerClassFactory : public IClassFactory {
public:
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    virtual ULONG STDMETHODCALLTYPE AddRef() override {
        return 2;
    }

    virtual ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }

    virtual HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        auto* driver = new (std::nothrow) BehringerUM2AsioDriver();
        if (!driver) return E_OUTOFMEMORY;

        HRESULT hr = driver->QueryInterface(riid, ppvObject);
        if (FAILED(hr)) {
            *ppvObject = static_cast<IASIO*>(driver);
            driver->AddRef();
            hr = S_OK;
        }
        driver->Release();
        return hr;
    }

    virtual HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_serverLocks);
        else InterlockedDecrement(&g_serverLocks);
        return S_OK;
    }
};

static BehringerClassFactory g_factory;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualCLSID(rclsid, CLSID_BehringerUM2AsioDriver)) {
        return g_factory.QueryInterface(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return (g_serverLocks == 0) ? S_OK : S_FALSE;
}

// Registration: per-user (HKCU, works without elevation) AND machine-wide (HKLM, needs admin). Hosts started
// "as administrator" ignore per-user class registrations (HKCU\Software\Classes), so without the HKLM copy an
// elevated FL Studio lists the driver (HKLM\SOFTWARE\ASIO) but CoCreateInstance fails with REGDB_E_CLASSNOTREG.
// Writing through HKEY_CLASSES_ROOT is NOT used: when the HKCU key exists, an HKCR write lands in HKCU again.
static LONG SetStr(HKEY k, const wchar_t* name, const wchar_t* value) {
    return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                          static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

static LONG RegisterUnder(HKEY root, const wchar_t* modulePath) {
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + CLSID_STRING;
    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(root, clsidKey.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) return r;
    SetStr(hKey, nullptr, DRIVER_DESC);
    HKEY hInproc = nullptr;
    r = RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hInproc, nullptr);
    if (r == ERROR_SUCCESS) {
        r = SetStr(hInproc, nullptr, modulePath);
        if (r == ERROR_SUCCESS) r = SetStr(hInproc, L"ThreadingModel", L"Both");
        RegCloseKey(hInproc);
    }
    RegCloseKey(hKey);
    if (r != ERROR_SUCCESS) return r;
    for (const wchar_t* name : { DRIVER_NAME, DRIVER_NAME_LEGACY }) {  // modern + legacy names, same CLSID
        HKEY hAsio = nullptr;
        const std::wstring asioKey = std::wstring(L"Software\\ASIO\\") + name;
        r = RegCreateKeyExW(root, asioKey.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hAsio, nullptr);
        if (r != ERROR_SUCCESS) return r;
        SetStr(hAsio, L"CLSID", CLSID_STRING);
        SetStr(hAsio, L"Description", DRIVER_DESC);
        RegCloseKey(hAsio);
    }
    return ERROR_SUCCESS;
}

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_hInstance, modulePath, MAX_PATH)) return SELFREG_E_CLASS;
    if (RegisterUnder(HKEY_CURRENT_USER, modulePath) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    // Machine-wide copy: only possible elevated (the installer runs regsvr32 elevated). Not elevated -> access
    // denied, and the per-user registration above still serves non-elevated hosts.
    const LONG m = RegisterUnder(HKEY_LOCAL_MACHINE, modulePath);
    return (m == ERROR_SUCCESS || m == ERROR_ACCESS_DENIED) ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + CLSID_STRING;
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        RegDeleteTreeW(root, clsidKey.c_str());
        RegDeleteKeyExW(root, clsidKey.c_str(), KEY_WOW64_64KEY, 0);
        for (const wchar_t* name : { DRIVER_NAME, DRIVER_NAME_LEGACY }) {
            const std::wstring asioKey = std::wstring(L"Software\\ASIO\\") + name;
            RegDeleteKeyExW(root, asioKey.c_str(), KEY_WOW64_64KEY, 0);
        }
    }
    return S_OK;
}
