#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <vector>

#pragma comment(lib, "ole32.lib")

void ProbeEndpoint(IMMDevice* d, const wchar_t* dir) {
    IPropertyStore* ps = nullptr;
    d->OpenPropertyStore(STGM_READ, &ps);
    if (!ps) return;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    ps->GetValue(PKEY_Device_FriendlyName, &pv);
    std::wstring name = (pv.pwszVal ? pv.pwszVal : L"Unknown");

    PROPVARIANT pvDesc;
    PropVariantInit(&pvDesc);
    ps->GetValue(PKEY_Device_DeviceDesc, &pvDesc);
    std::wstring desc = (pvDesc.pwszVal ? pvDesc.pwszVal : L"");

    std::wcout << L"[" << dir << L"] " << name;
    if (!desc.empty()) std::wcout << L" (" << desc << L")";
    std::wcout << L"\n";

    IAudioClient* ac = nullptr;
    HRESULT hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr) || !ac) {
        ps->Release();
        return;
    }

    REFERENCE_TIME defP = 0, minP = 0;
    ac->GetDevicePeriod(&defP, &minP);
    std::cout << "  Periodo: Default=" << defP << " (" << (defP * 48 / 10000) << " frames @48k), Min=" 
              << minP << " (" << (minP * 48 / 10000) << " frames @48k)\n";

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 48000 * 4;

    WAVEFORMATEX* closest = nullptr;
    HRESULT hrEx = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx, nullptr);
    std::cout << "  Exclusive 48kHz 16-bit 2ch: " << (hrEx == S_OK ? "SUPORTADO" : "NAO SUPORTADO") << " (0x" << std::hex << hrEx << std::dec << ")\n";

    HRESULT hrSh = ac->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &wfx, &closest);
    std::cout << "  Shared 48kHz 16-bit 2ch: " << (hrSh == S_OK ? "SUPORTADO" : "NAO SUPORTADO") << " (0x" << std::hex << hrSh << std::dec << ")\n";

    ac->Release();
    ps->Release();
}

int main() {
    CoInitialize(nullptr);
    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (!pEnum) {
        std::cerr << "Falha ao instanciar MMDeviceEnumerator\n";
        return 1;
    }

    std::cout << "=== ENDPOINTS DE REPRODUCAO (RENDER) ===\n";
    IMMDeviceCollection* pRenderCol = nullptr;
    pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pRenderCol);
    if (pRenderCol) {
        UINT count = 0;
        pRenderCol->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = nullptr;
            pRenderCol->Item(i, &d);
            ProbeEndpoint(d, L"RENDER");
            d->Release();
        }
        pRenderCol->Release();
    }

    std::cout << "\n=== ENDPOINTS DE GRAVACAO (CAPTURE) ===\n";
    IMMDeviceCollection* pCapCol = nullptr;
    pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCapCol);
    if (pCapCol) {
        UINT count = 0;
        pCapCol->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = nullptr;
            pCapCol->Item(i, &d);
            ProbeEndpoint(d, L"CAPTURE");
            d->Release();
        }
        pCapCol->Release();
    }

    pEnum->Release();
    CoUninitialize();
    return 0;
}
