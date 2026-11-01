#include <windows.h>
#include <iostream>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#pragma comment(lib, "ole32.lib")

static const GUID CLSID_BehringerUM2AsioDriver = 
    { 0x77dbbccd, 0x91c6, 0x4856, { 0xa9, 0x00, 0xfc, 0x8d, 0x00, 0x7d, 0x16, 0x1c } };

static const GUID IID_IASIO = 
    { 0x933923e0, 0x01fe, 0x11d2, { 0xbc, 0x7b, 0x00, 0x80, 0xc7, 0x79, 0xb4, 0x3e } };

int main() {
    CoInitialize(nullptr);
    IASIO* pAsio = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_BehringerUM2AsioDriver, nullptr, CLSCTX_INPROC_SERVER, IID_IASIO, (void**)&pAsio);
    if (SUCCEEDED(hr) && pAsio) {
        std::cout << "[SUCESSO] Driver instanciado diretamente pelo Windows COM via CLSID!\n";
        char name[64]{};
        pAsio->getDriverName(name);
        std::cout << "Nome do Driver: " << name << "\n";
        pAsio->Release();
    } else {
        std::cout << "[ERRO] CoCreateInstance falhou: 0x" << std::hex << hr << std::dec << "\n";
    }
    CoUninitialize();
    return 0;
}
