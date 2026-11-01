#include <windows.h>
#include <unknwn.h>
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

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_hInstance, modulePath, MAX_PATH);

    auto registerUnderRoot = [&](HKEY root) {
        // Registra CLSID
        std::wstring clsidKey = std::wstring(root == HKEY_CLASSES_ROOT ? L"CLSID\\" : L"Software\\Classes\\CLSID\\") + CLSID_STRING;
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(root, clsidKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(DRIVER_DESC), static_cast<DWORD>((wcslen(DRIVER_DESC) + 1) * sizeof(wchar_t)));
            
            HKEY hInprocKey = nullptr;
            if (RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &hInprocKey, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(hInprocKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(modulePath), static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
                const wchar_t* threading = L"Both";
                RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(threading), static_cast<DWORD>((wcslen(threading) + 1) * sizeof(wchar_t)));
                RegCloseKey(hInprocKey);
            }
            RegCloseKey(hKey);
        }

        // Registra ASIO entry (Moderno U-Phoria e Legado UM2)
        auto registerAsioKey = [&](const wchar_t* name) {
            std::wstring asioKey = std::wstring(L"Software\\ASIO\\") + name;
            HKEY hAsioKey = nullptr;
            if (RegCreateKeyExW(root == HKEY_CLASSES_ROOT ? HKEY_LOCAL_MACHINE : root, asioKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hAsioKey, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(hAsioKey, L"CLSID", 0, REG_SZ, reinterpret_cast<const BYTE*>(CLSID_STRING), static_cast<DWORD>((wcslen(CLSID_STRING) + 1) * sizeof(wchar_t)));
                RegSetValueExW(hAsioKey, L"Description", 0, REG_SZ, reinterpret_cast<const BYTE*>(DRIVER_DESC), static_cast<DWORD>((wcslen(DRIVER_DESC) + 1) * sizeof(wchar_t)));
                RegCloseKey(hAsioKey);
            }
        };

        registerAsioKey(DRIVER_NAME);
        registerAsioKey(DRIVER_NAME_LEGACY);
    };

    registerUnderRoot(HKEY_CURRENT_USER);
    registerUnderRoot(HKEY_CLASSES_ROOT);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    auto unregisterAsioKey = [&](HKEY root, const wchar_t* name) {
        std::wstring asioKey = std::wstring(L"Software\\ASIO\\") + name;
        RegDeleteKeyW(root, asioKey.c_str());
    };

    unregisterAsioKey(HKEY_LOCAL_MACHINE, DRIVER_NAME);
    unregisterAsioKey(HKEY_LOCAL_MACHINE, DRIVER_NAME_LEGACY);
    unregisterAsioKey(HKEY_CURRENT_USER, DRIVER_NAME);
    unregisterAsioKey(HKEY_CURRENT_USER, DRIVER_NAME_LEGACY);

    std::wstring clsidInproc = std::wstring(L"CLSID\\") + CLSID_STRING + L"\\InprocServer32";
    RegDeleteKeyW(HKEY_CLASSES_ROOT, clsidInproc.c_str());

    std::wstring clsidKey = std::wstring(L"CLSID\\") + CLSID_STRING;
    RegDeleteKeyW(HKEY_CLASSES_ROOT, clsidKey.c_str());

    return S_OK;
}
