// Shared helpers for the KaxWav measurement tools (not part of the driver).
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace um2 {

// Same matching rule as tools/raw_capture_test.cpp: interface friendly name contains "USB Audio CODEC".
inline ComPtr<IMMDevice> FindEndpoint(IMMDeviceEnumerator* e, EDataFlow flow, std::wstring* outName = nullptr) {
    ComPtr<IMMDeviceCollection> c;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &c))) return nullptr;
    UINT n = 0;
    c->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(c->Item(i, &d))) continue;
        ComPtr<IPropertyStore> s;
        if (FAILED(d->OpenPropertyStore(STGM_READ, &s))) continue;
        PROPVARIANT v;
        PropVariantInit(&v);
        bool hit = SUCCEEDED(s->GetValue(PKEY_DeviceInterface_FriendlyName, &v)) && v.vt == VT_LPWSTR &&
                   wcsstr(v.pwszVal, L"USB Audio CODEC") != nullptr;
        PropVariantClear(&v);
        if (hit) {
            if (outName) {
                PROPVARIANT fn;
                PropVariantInit(&fn);
                if (SUCCEEDED(s->GetValue(PKEY_Device_FriendlyName, &fn)) && fn.vt == VT_LPWSTR) *outName = fn.pwszVal;
                PropVariantClear(&fn);
            }
            return d;
        }
    }
    return nullptr;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

inline std::string JsonEscape(const std::string& in) {
    std::string o;
    for (char ch : in) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if (ch == '\n') o += "\\n";
        else if (ch == '\r') {}
        else if (static_cast<unsigned char>(ch) < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

inline WAVEFORMATEX Pcm16(DWORD rate, WORD channels) {
    WAVEFORMATEX w{};
    w.wFormatTag = WAVE_FORMAT_PCM;
    w.nChannels = channels;
    w.nSamplesPerSec = rate;
    w.wBitsPerSample = 16;
    w.nBlockAlign = static_cast<WORD>(channels * 2);
    w.nAvgBytesPerSec = rate * w.nBlockAlign;
    return w;
}

// Exclusive event-driven Initialize with the mandatory AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED retry.
inline HRESULT InitExclusiveEvent(IMMDevice* dev, ComPtr<IAudioClient>& client, const WAVEFORMATEX* fmt,
                                  REFERENCE_TIME period, REFERENCE_TIME* usedPeriod) {
    client.Reset();
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return hr;
    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, fmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0;
        client->GetBufferSize(&frames);
        period = static_cast<REFERENCE_TIME>(10000000.0 * frames / fmt->nSamplesPerSec + 0.5);
        client.Reset();
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, fmt, nullptr);
    }
    if (usedPeriod) *usedPeriod = period;
    return hr;
}

inline bool ReadFileBytes(const char* path, std::vector<char>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    size_t got = n > 0 ? fread(out.data(), 1, out.size(), f) : 0;
    fclose(f);
    return got == out.size();
}

inline bool WriteFileBytes(const char* path, const void* data, size_t bytes) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    size_t w = bytes ? fwrite(data, 1, bytes, f) : 0;
    fclose(f);
    return w == bytes;
}

}  // namespace um2
