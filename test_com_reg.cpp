// Verifies what a DAW would load: instantiates the driver through COM by CLSID the way ASIO hosts do
// (CoCreateInstance with the CLSID also as the IID) and prints the DLL that Windows actually loaded.
// Exit code 0 = loaded; prints "LOADED_PATH=<path>" for scripts (installer verification).
#include <windows.h>
#include <iostream>
#include <string>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#pragma comment(lib, "ole32.lib")

static const GUID CLSID_BehringerUM2AsioDriver =
    { 0x77dbbccd, 0x91c6, 0x4856, { 0xa9, 0x00, 0xfc, 0x8d, 0x00, 0x7d, 0x16, 0x1c } };

int main() {
    CoInitialize(nullptr);
    IASIO* pAsio = nullptr;
    // ASIO hosts (asiolist.cpp in the SDK) pass the CLSID as the interface id too
    HRESULT hr = CoCreateInstance(CLSID_BehringerUM2AsioDriver, nullptr, CLSCTX_INPROC_SERVER,
                                  CLSID_BehringerUM2AsioDriver, reinterpret_cast<void**>(&pAsio));
    int rc = 1;
    if (SUCCEEDED(hr) && pAsio) {
        char name[64]{};
        pAsio->getDriverName(name);
        wchar_t path[MAX_PATH]{};
        HMODULE mod = GetModuleHandleW(L"BehringerUM2ASIODriver.dll");
        if (mod) GetModuleFileNameW(mod, path, MAX_PATH);
        std::cout << "[SUCESSO] Driver instanciado pelo COM via CLSID. Nome: " << name << "\n";
        std::wcout << L"LOADED_PATH=" << path << L"\n";
        std::cout << "DRIVER_VERSION=" << pAsio->getDriverVersion() << "\n";
        pAsio->Release();
        rc = 0;
    } else {
        std::cout << "[ERRO] CoCreateInstance falhou: 0x" << std::hex << hr << std::dec << "\n";
    }
    CoUninitialize();
    return rc;
}
