// ASIO measurement host for the KaxWav driver (M1 RTL, M3/M4 sweep and tone, M5 run, M7 digital taps).
//
// Loads BehringerUM2ASIODriver.dll by path (no COM registration needed), selects the engine mode
// and buffer size through the same registry values the driver reads (HKCU\Software\BehringerUM2\ASIO
// DriverMode/BufferSize - the caller saves and restores them), plays an int16 mono stimulus on
// both outputs starting at a fixed ASIO sample index and records inputs 1-2 on the same ASIO
// sample timeline. Output: one JSON line on stdout.
//
// Optional digital taps for M7 (only meaningful in SharedFlex): a WASAPI shared capture of the
// UM2 input (same engine data the driver reads) and a WASAPI loopback of the UM2 render endpoint
// (what the driver handed to the engine).
//
// Usage: um2_asio_probe --dll D --mode 0|1 --buffer N --seconds S [--rec rec.i16] [--stim stim.i16
//        --stim-at SAMPLE] [--tap-capture cap.f32] [--tap-loopback loop.f32] [--tap-raw 0|1]
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>
#include <share.h>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include "um2_common.h"
#pragma comment(lib, "ole32.lib")

namespace {
const GUID CLSID_UM2 = {0x77dbbccd, 0x91c6, 0x4856, {0xa9, 0x00, 0xfc, 0x8d, 0x00, 0x7d, 0x16, 0x1c}};
const GUID IID_IASIO_ = {0x933923e0, 0x01fe, 0x11d2, {0xbc, 0x7b, 0x00, 0x80, 0xc7, 0x79, 0xb4, 0x3e}};

struct State {
    long buffer = 0;
    ASIOBufferInfo info[4]{};  // in0, in1, out0, out1
    std::vector<int16_t> rec;   // interleaved in0/in1
    std::vector<int16_t> stim;  // mono
    long long stimAt = 0;
    std::atomic<long long> blocks{0};
    // time info (C6 evidence)
    std::atomic<long long> tiCount{0}, tiNonMonotonic{0};
    long long tiFirstPos = -1, tiLastPos = -1;
    double tiMaxAbsDiffUs = 0;  // |systemTime - QPC_ns(now)| measured inside the callback
    LARGE_INTEGER qpcFreq{};
    std::atomic<long> resetRequests{0};
} g;

uint64_t QpcToNs(int64_t qpc, int64_t freq) {
    return static_cast<uint64_t>(qpc / freq) * 1000000000ULL +
           static_cast<uint64_t>(qpc % freq) * 1000000000ULL / static_cast<uint64_t>(freq);
}

void Process(long index) {
    const long n = g.buffer;
    const long long pos = g.blocks.load(std::memory_order_relaxed) * n;
    auto* in0 = static_cast<int16_t*>(g.info[0].buffers[index]);
    auto* in1 = static_cast<int16_t*>(g.info[1].buffers[index]);
    auto* out0 = static_cast<int16_t*>(g.info[2].buffers[index]);
    auto* out1 = static_cast<int16_t*>(g.info[3].buffers[index]);
    const size_t cap = g.rec.size() / 2;
    for (long i = 0; i < n; ++i) {
        const long long s = pos + i;
        if (static_cast<size_t>(s) < cap) {
            g.rec[static_cast<size_t>(s) * 2 + 0] = in0 ? in0[i] : 0;
            g.rec[static_cast<size_t>(s) * 2 + 1] = in1 ? in1[i] : 0;
        }
        const long long k = s - g.stimAt;
        const int16_t v = (k >= 0 && static_cast<size_t>(k) < g.stim.size()) ? g.stim[static_cast<size_t>(k)] : 0;
        if (out0) out0[i] = v;
        if (out1) out1[i] = v;
    }
    g.blocks.fetch_add(1, std::memory_order_relaxed);
}

void BufferSwitch(long index, ASIOBool) { Process(index); }
void SampleRateChanged(ASIOSampleRate) {}
long AsioMessage(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported)
        return (value == kAsioSupportsTimeInfo || value == kAsioResetRequest || value == kAsioEngineVersion) ? 1 : 0;
    if (selector == kAsioSupportsTimeInfo) return 1;
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioResetRequest) { g.resetRequests++; return 1; }
    return 0;
}
ASIOTime* BufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (t) {
        const long long sp = (static_cast<long long>(t->timeInfo.samplePosition.hi) << 32) | t->timeInfo.samplePosition.lo;
        const uint64_t st = (static_cast<uint64_t>(t->timeInfo.systemTime.hi) << 32) | t->timeInfo.systemTime.lo;
        if (g.tiFirstPos < 0) g.tiFirstPos = sp;
        else if (sp <= g.tiLastPos) g.tiNonMonotonic++;
        g.tiLastPos = sp;
        const double diffUs = std::fabs(static_cast<double>(QpcToNs(now.QuadPart, g.qpcFreq.QuadPart)) - static_cast<double>(st)) / 1000.0;
        g.tiMaxAbsDiffUs = (std::max)(g.tiMaxAbsDiffUs, diffUs);
        g.tiCount++;
    }
    Process(index);
    return nullptr;
}
ASIOCallbacks g_cb = {BufferSwitch, SampleRateChanged, AsioMessage, BufferSwitchTimeInfo};

// ---------------- WASAPI taps (M7) ----------------
struct Tap {
    std::atomic<bool> stop{false};
    std::vector<float> data;
    WORD channels = 0;
    DWORD rate = 0;
    HRESULT hr = S_OK;
    UINT64 discontinuities = 0;
};

bool g_tapRaw = false;  // --tap-raw 1: capture tap opened with AUDCLNT_STREAMOPTIONS_RAW (same as the driver)

void TapThread(EDataFlow flow, bool loopback, Tap* tap) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> ac;
    ComPtr<IAudioCaptureClient> cc;
    WAVEFORMATEX* mf = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(en.GetAddressOf()));
    if (SUCCEEDED(hr)) { dev = um2::FindEndpoint(en.Get(), flow); if (!dev) hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ac.GetAddressOf()));
    if (SUCCEEDED(hr) && g_tapRaw && !loopback) {
        ComPtr<IAudioClient2> ac2;
        if (SUCCEEDED(ac.As(&ac2))) {
            AudioClientProperties props{};
            props.cbSize = sizeof(props);
            props.eCategory = AudioCategory_Other;
            props.Options = AUDCLNT_STREAMOPTIONS_RAW;
            hr = ac2->SetClientProperties(&props);
        }
    }
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&mf);
    if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 1000000, 0, mf, nullptr);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cc.GetAddressOf()));
    const bool isFloat = mf && (mf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (SUCCEEDED(hr) && (!isFloat || mf->wBitsPerSample != 32)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (SUCCEEDED(hr)) {
        tap->channels = mf->nChannels;
        tap->rate = mf->nSamplesPerSec;
        tap->data.reserve(static_cast<size_t>(mf->nSamplesPerSec) * mf->nChannels * 30);
        hr = ac->Start();
    }
    tap->hr = hr;
    while (SUCCEEDED(hr) && !tap->stop.load()) {
        Sleep(3);
        BYTE* p = nullptr;
        UINT32 n = 0;
        DWORD fl = 0;
        while (SUCCEEDED(hr = cc->GetBuffer(&p, &n, &fl, nullptr, nullptr)) && hr != AUDCLNT_S_BUFFER_EMPTY && n > 0) {
            if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) tap->discontinuities++;
            const float* f = reinterpret_cast<const float*>(p);
            const size_t cnt = static_cast<size_t>(n) * tap->channels;
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) tap->data.insert(tap->data.end(), cnt, 0.0f);
            else tap->data.insert(tap->data.end(), f, f + cnt);
            cc->ReleaseBuffer(n);
        }
        if (FAILED(hr)) tap->hr = hr;
    }
    if (ac) ac->Stop();
    if (mf) CoTaskMemFree(mf);
    CoUninitialize();
}

std::wstring DriverLogPath() {
    wchar_t lad[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) return {};
    return std::wstring(lad) + L"\\KaxWav\\UM2\\driver_um2.log";
}
long long FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}
std::vector<std::string> ReadLogSince(const std::wstring& p, long long from) {
    std::vector<std::string> lines;
    FILE* f = _wfsopen(p.c_str(), L"rb", _SH_DENYNO);
    if (!f) return lines;
    _fseeki64(f, from, SEEK_SET);
    std::string buf;
    char tmp[4096];
    size_t r;
    while ((r = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, r);
    fclose(f);
    size_t s = 0;
    while (s < buf.size()) {
        size_t e = buf.find('\n', s);
        if (e == std::string::npos) e = buf.size();
        std::string line = buf.substr(s, e - s);
        if (line.find("Initialize") != std::string::npos || line.find("completado") != std::string::npos ||
            line.find("allback") != std::string::npos || line.find("bloqueado") != std::string::npos ||
            line.find("Real Buffer") != std::string::npos || line.find("Buffer Total") != std::string::npos)
            lines.push_back(line);
        s = e + 1;
    }
    return lines;
}
void SetDriverRegistry(int mode, long buffer) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD m = static_cast<DWORD>(mode), b = static_cast<DWORD>(buffer);
        RegSetValueExW(k, L"DriverMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&m), sizeof(m));
        RegSetValueExW(k, L"BufferSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&b), sizeof(b));
        RegCloseKey(k);
    }
}
}  // namespace

int main(int argc, char** argv) {
    std::string dll, recPath, stimPath, tapCapPath, tapLoopPath;
    int mode = 1;
    bool useCom = false;  // --com 1: load through COM registration (InprocServer32) like a real host
    long buffer = 256;
    double seconds = 2.0;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--dll") dll = argv[i + 1];
        else if (a == "--mode") mode = atoi(argv[i + 1]);
        else if (a == "--buffer") buffer = atol(argv[i + 1]);
        else if (a == "--seconds") seconds = atof(argv[i + 1]);
        else if (a == "--rec") recPath = argv[i + 1];
        else if (a == "--stim") stimPath = argv[i + 1];
        else if (a == "--stim-at") g.stimAt = _atoi64(argv[i + 1]);
        else if (a == "--tap-capture") tapCapPath = argv[i + 1];
        else if (a == "--tap-loopback") tapLoopPath = argv[i + 1];
        else if (a == "--tap-raw") g_tapRaw = atoi(argv[i + 1]) != 0;
        else if (a == "--com") useCom = atoi(argv[i + 1]) != 0;
    }
    QueryPerformanceFrequency(&g.qpcFreq);
    if (!stimPath.empty()) {
        std::vector<char> b;
        if (!um2::ReadFileBytes(stimPath.c_str(), b)) { printf("{\"ok\":false,\"error\":\"stim read\"}\n"); return 1; }
        g.stim.resize(b.size() / 2);
        memcpy(g.stim.data(), b.data(), g.stim.size() * 2);
    }
    g.rec.assign(static_cast<size_t>((seconds + 1.0) * 48000.0) * 2, 0);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetDriverRegistry(mode, buffer);
    HMODULE h = nullptr;
    IASIO* asio = nullptr;
    if (useCom) {
        // Exactly what FL Studio / the ASIO SDK host does: CoCreateInstance with the CLSID as the IID,
        // so the DLL comes from InprocServer32 (the installed one), not from a path.
        HRESULT hr = CoCreateInstance(CLSID_UM2, nullptr, CLSCTX_INPROC_SERVER, CLSID_UM2, reinterpret_cast<void**>(&asio));
        if (FAILED(hr) || !asio) { printf("{\"ok\":false,\"error\":\"CoCreateInstance 0x%08lX\"}\n", static_cast<unsigned long>(hr)); return 1; }
    } else {
        std::wstring wdll(dll.begin(), dll.end());
        h = LoadLibraryW(wdll.c_str());
        if (!h) { printf("{\"ok\":false,\"error\":\"LoadLibrary %lu\"}\n", GetLastError()); return 1; }
        using GetCO = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
        auto getCO = reinterpret_cast<GetCO>(GetProcAddress(h, "DllGetClassObject"));
        IClassFactory* cf = nullptr;
        if (!getCO || FAILED(getCO(CLSID_UM2, IID_IClassFactory, reinterpret_cast<void**>(&cf))) ||
            FAILED(cf->CreateInstance(nullptr, IID_IASIO_, reinterpret_cast<void**>(&asio)))) {
            printf("{\"ok\":false,\"error\":\"CreateInstance\"}\n");
            return 1;
        }
        cf->Release();
    }
    std::string loadedPath;
    if (HMODULE hm = GetModuleHandleW(L"BehringerUM2ASIODriver.dll")) {
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(hm, p, MAX_PATH);
        loadedPath = um2::Narrow(p);
    }
    const long driverVersion = asio->getDriverVersion();
    if (!asio->init(nullptr)) { printf("{\"ok\":false,\"error\":\"init\"}\n"); return 1; }

    long minS = 0, maxS = 0, prefS = 0, gran = 0;
    asio->getBufferSize(&minS, &maxS, &prefS, &gran);
    ASIOSampleRate sr = 0;
    asio->getSampleRate(&sr);
    for (int i = 0; i < 4; ++i) {
        g.info[i].isInput = i < 2 ? ASIOTrue : ASIOFalse;
        g.info[i].channelNum = i % 2;
    }
    const std::wstring logPath = DriverLogPath();
    const long long logFrom = FileSize(logPath);
    g.buffer = buffer;
    ASIOError e = asio->createBuffers(g.info, 4, buffer, &g_cb);
    std::vector<std::string> logLines = ReadLogSince(logPath, logFrom);
    long inLat = 0, outLat = 0;
    asio->getLatencies(&inLat, &outLat);
    ASIOChannelInfo ci{};
    ci.channel = 0;
    ci.isInput = ASIOTrue;
    asio->getChannelInfo(&ci);

    Tap tapCap, tapLoop;
    std::thread thCap, thLoop;
    if (!tapCapPath.empty()) thCap = std::thread(TapThread, eCapture, false, &tapCap);
    if (!tapLoopPath.empty()) thLoop = std::thread(TapThread, eRender, true, &tapLoop);
    if (!tapCapPath.empty() || !tapLoopPath.empty()) Sleep(300);

    LARGE_INTEGER t0{}, t1{};
    ASIOError es = ASE_NotPresent;
    if (e == ASE_OK) {
        QueryPerformanceCounter(&t0);
        es = asio->start();
        Sleep(static_cast<DWORD>(seconds * 1000.0));
        asio->stop();
        QueryPerformanceCounter(&t1);
    }
    if (!tapCapPath.empty() || !tapLoopPath.empty()) Sleep(200);
    tapCap.stop = true;
    tapLoop.stop = true;
    if (thCap.joinable()) thCap.join();
    if (thLoop.joinable()) thLoop.join();
    asio->disposeBuffers();
    asio->Release();
    if (h) FreeLibrary(h);
    CoUninitialize();

    const long long blocks = g.blocks.load();
    const long long recFrames = (std::min)(static_cast<long long>(g.rec.size() / 2), blocks * buffer);
    if (!recPath.empty()) um2::WriteFileBytes(recPath.c_str(), g.rec.data(), static_cast<size_t>(recFrames) * 4);
    if (!tapCapPath.empty()) um2::WriteFileBytes(tapCapPath.c_str(), tapCap.data.data(), tapCap.data.size() * 4);
    if (!tapLoopPath.empty()) um2::WriteFileBytes(tapLoopPath.c_str(), tapLoop.data.data(), tapLoop.data.size() * 4);

    const double elapsed = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(g.qpcFreq.QuadPart);
    std::string mode_active = "unknown";
    for (const auto& l : logLines) {
        size_t p = l.find("Mode=");
        if (l.find("completado") != std::string::npos && p != std::string::npos) mode_active = l.substr(p + 5, l.find(',', p) - (p + 5));
    }
    printf("{\"load\":\"%s\",\"loaded_path\":\"%s\",\"driver_version\":%ld,", useCom ? "com" : "path",
           um2::JsonEscape(loadedPath).c_str(), driverVersion);
    printf("\"ok\":%s,\"create_buffers\":%ld,\"start\":%ld,\"mode_requested\":%d,\"mode_active\":\"%s\",\"buffer\":%ld,"
           "\"sample_rate\":%.1f,\"buffer_size_range\":[%ld,%ld,%ld,%ld],\"input_latency\":%ld,\"output_latency\":%ld,"
           "\"sample_type\":%ld,\"blocks\":%lld,\"elapsed_s\":%.6f,\"rec_frames\":%lld,\"stim_at\":%lld,\"stim_len\":%zu,"
           "\"reset_requests\":%ld,\"time_info\":{\"count\":%lld,\"first_pos\":%lld,\"last_pos\":%lld,\"non_monotonic\":%lld,"
           "\"max_abs_systemtime_minus_qpc_us\":%.3f},"
           "\"tap_capture\":{\"hr\":\"0x%08lX\",\"channels\":%u,\"rate\":%lu,\"frames\":%zu,\"discontinuities\":%llu},"
           "\"tap_loopback\":{\"hr\":\"0x%08lX\",\"channels\":%u,\"rate\":%lu,\"frames\":%zu,\"discontinuities\":%llu},"
           "\"driver_log\":[",
           (e == ASE_OK && es == ASE_OK) ? "true" : "false", static_cast<long>(e), static_cast<long>(es), mode,
           mode_active.c_str(), buffer, sr, minS, maxS, prefS, gran, inLat, outLat, static_cast<long>(ci.type), blocks,
           elapsed, recFrames, g.stimAt, g.stim.size(), g.resetRequests.load(), g.tiCount.load(), g.tiFirstPos, g.tiLastPos,
           g.tiNonMonotonic.load(), g.tiMaxAbsDiffUs,
           static_cast<unsigned long>(tapCap.hr), tapCap.channels, tapCap.rate, tapCap.channels ? tapCap.data.size() / tapCap.channels : 0,
           tapCap.discontinuities,
           static_cast<unsigned long>(tapLoop.hr), tapLoop.channels, tapLoop.rate, tapLoop.channels ? tapLoop.data.size() / tapLoop.channels : 0,
           tapLoop.discontinuities);
    for (size_t i = 0; i < logLines.size(); ++i) printf("%s\"%s\"", i ? "," : "", um2::JsonEscape(logLines[i]).c_str());
    printf("]}\n");
    return (e == ASE_OK && es == ASE_OK) ? 0 : 2;
}
