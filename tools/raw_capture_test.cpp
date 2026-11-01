// Captura N segundos do endpoint UM2 em shared mode com e sem AUDCLNT_STREAMOPTIONS_RAW (bypass de APOs)
// e grava WAV float. Uso: raw_capture_test.exe <segundos> <out_prefix>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#pragma comment(lib, "ole32.lib")

static IMMDevice* FindUm2(IMMDeviceEnumerator* e) {
    IMMDeviceCollection* c = nullptr;
    e->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &c);
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; c->Item(i, &d);
        IPropertyStore* s = nullptr; d->OpenPropertyStore(STGM_READ, &s);
        PROPVARIANT v; PropVariantInit(&v);
        s->GetValue(PKEY_DeviceInterface_FriendlyName, &v);
        bool hit = v.vt == VT_LPWSTR && wcsstr(v.pwszVal, L"USB Audio CODEC");
        PropVariantClear(&v); s->Release();
        if (hit) { c->Release(); return d; }
        d->Release();
    }
    c->Release();
    return nullptr;
}

static void WriteWav(const char* path, const std::vector<float>& x, int ch, int sr) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    uint32_t dataBytes = (uint32_t)(x.size() * 4);
    uint32_t riff = 36 + dataBytes; uint16_t fmt = 3, nch = (uint16_t)ch, bits = 32, ba = (uint16_t)(ch * 4);
    uint32_t srate = sr, br = sr * ch * 4, fl = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&nch, 2, 1, f); fwrite(&srate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    fwrite(x.data(), 4, x.size(), f); fclose(f);
}

static bool Capture(IMMDevice* dev, bool raw, double secs, const char* out) {
    IAudioClient2* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, (void**)&ac))) return false;
    if (raw) {
        AudioClientProperties p{}; p.cbSize = sizeof(p); p.eCategory = AudioCategory_Media; p.Options = AUDCLNT_STREAMOPTIONS_RAW;
        HRESULT hr = ac->SetClientProperties(&p);
        printf("  SetClientProperties(RAW) hr=0x%08lX\n", hr);
    }
    WAVEFORMATEX* mf = nullptr; ac->GetMixFormat(&mf);
    printf("  mix: ch=%u sr=%lu bits=%u tag=0x%X\n", mf->nChannels, mf->nSamplesPerSec, mf->wBitsPerSample, mf->wFormatTag);
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mf, nullptr);
    if (FAILED(hr)) { printf("  Initialize hr=0x%08lX\n", hr); return false; }
    IAudioCaptureClient* cc = nullptr; ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc);
    std::vector<float> buf; size_t want = (size_t)(secs * mf->nSamplesPerSec) * mf->nChannels;
    ac->Start();
    while (buf.size() < want) {
        Sleep(5);
        BYTE* d; UINT32 n; DWORD fl;
        while (SUCCEEDED(cc->GetBuffer(&d, &n, &fl, nullptr, nullptr)) && n) {
            const float* fd = (const float*)d;
            for (UINT32 i = 0; i < n * mf->nChannels; ++i) buf.push_back((fl & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.f : fd[i]);
            cc->ReleaseBuffer(n);
        }
    }
    ac->Stop();
    WriteWav(out, buf, mf->nChannels, mf->nSamplesPerSec);
    CoTaskMemFree(mf); cc->Release(); ac->Release();
    return true;
}

int main(int argc, char** argv) {
    double secs = argc > 1 ? atof(argv[1]) : 5.0;
    std::string pre = argc > 2 ? argv[2] : "cap";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* e = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
    IMMDevice* d = FindUm2(e);
    if (!d) { printf("UM2 nao encontrada\n"); return 1; }
    IPropertyStore* s = nullptr; d->OpenPropertyStore(STGM_READ, &s);
    PROPERTYKEY rawKey{ {0x8943b373,0x388c,0x4395,{0xb5,0x57,0xbc,0x6d,0xbc,0xff,0xaf,0xdb}}, 2 };
    PROPVARIANT v; PropVariantInit(&v); s->GetValue(rawKey, &v);
    printf("RawProcessingSupported vt=%d val=%d\n", v.vt, v.vt == VT_BOOL ? v.boolVal : -1);
    s->Release();
    IAudioEndpointVolume* ev = nullptr;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&ev))) {
        float lo, hi, st, cur, sc; ev->GetVolumeRange(&lo, &hi, &st); ev->GetMasterVolumeLevel(&cur); ev->GetMasterVolumeLevelScalar(&sc);
        DWORD hwm = 0; ev->QueryHardwareSupport(&hwm);
        printf("EndpointVolume: range %.2f..%.2f dB step %.3f | atual %.2f dB (scalar %.2f) | hwSupport=0x%lX\n", lo, hi, st, cur, sc, hwm);
        ev->Release();
    }
    {
        IAudioClient* t = nullptr;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&t))) {
            for (int ch = 1; ch <= 2; ++ch) for (DWORD sr : {44100u, 48000u}) {
                WAVEFORMATEX w{}; w.wFormatTag = WAVE_FORMAT_PCM; w.nChannels = (WORD)ch; w.nSamplesPerSec = sr; w.wBitsPerSample = 16;
                w.nBlockAlign = (WORD)(ch * 2); w.nAvgBytesPerSec = sr * w.nBlockAlign;
                printf("Exclusive %dch %lu Hz 16-bit: hr=0x%08lX\n", ch, sr, t->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w, nullptr));
            }
            t->Release();
        }
    }
    if (argc > 3) return 0;
    printf("[default/APO]\n"); Capture(d, false, secs, (pre + "_apo.wav").c_str());
    printf("[raw]\n"); Capture(d, true, secs, (pre + "_raw.wav").c_str());
    d->Release(); e->Release();
    return 0;
}
