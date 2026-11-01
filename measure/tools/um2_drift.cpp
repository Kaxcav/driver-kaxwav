// M6 / C8: clock drift of the UM2 capture and render streams against QPC.
//
// Opens capture and render of the UM2 at the same time in WASAPI exclusive event-driven mode
// (48 kHz / 16-bit / stereo, render = digital silence) and, once per second, logs to CSV:
//   - IAudioClock::GetPosition of both streams (stream position + QPC timestamp),
//   - the device position / QPC timestamp that IAudioCaptureClient::GetBuffer attaches to packets,
//   - frames counted on each side.
// The ppm of each stream and the differential are computed by measure/scripts (linear regression),
// and an endpoint estimate is printed here so a partial run is still useful.
//
// A progress line (local time, frames, QPC, partial ppm) goes to <csv>.progress.log every --progress-s
// seconds; a stream failure is written there with local time and HRESULT.
//
// Usage: um2_drift --seconds N --csv out.csv [--period-ms 10] [--progress-s 60]
#include "um2_common.h"
#include <avrt.h>
#include <atomic>
#include <thread>
#include <cmath>
#include <share.h>
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace {
struct Shared {
    std::atomic<uint64_t> capFrames{0}, renFrames{0}, capDiscont{0}, renTimeouts{0}, capTimeouts{0};
    std::atomic<uint64_t> capPktDevPos{0}, capPktQpc100ns{0};
    std::atomic<HRESULT> fatal{S_OK};
    std::atomic<DWORD> mmcssError{0};
};
HANDLE g_stop = nullptr;
void LocalStamp(char* out, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
BOOL WINAPI CtrlHandler(DWORD) { if (g_stop) SetEvent(g_stop); return TRUE; }

void AudioThread(IAudioClient* cap, IAudioClient* ren, HANDLE capEvt, HANDLE renEvt, UINT32 renFrames, Shared* sh) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (!mm) sh->mmcssError = GetLastError();  // reported, never masked with TIME_CRITICAL
    else AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);
    ComPtr<IAudioCaptureClient> cc;
    ComPtr<IAudioRenderClient> rc;
    cap->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cc.GetAddressOf()));
    ren->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(rc.GetAddressOf()));
    HANDLE evts[3] = {g_stop, renEvt, capEvt};
    for (;;) {
        DWORD w = WaitForMultipleObjects(3, evts, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) { sh->renTimeouts++; sh->capTimeouts++; continue; }
        if (w == WAIT_OBJECT_0 + 1) {
            BYTE* p = nullptr;
            HRESULT hr = rc->GetBuffer(renFrames, &p);
            if (FAILED(hr)) { sh->fatal = hr; break; }
            hr = rc->ReleaseBuffer(renFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) { sh->fatal = hr; break; }
            sh->renFrames += renFrames;
        } else if (w == WAIT_OBJECT_0 + 2) {
            for (;;) {
                BYTE* p = nullptr;
                UINT32 n = 0;
                DWORD fl = 0;
                UINT64 devPos = 0, qpc = 0;
                HRESULT hr = cc->GetBuffer(&p, &n, &fl, &devPos, &qpc);
                if (FAILED(hr)) { sh->fatal = hr; break; }
                if (hr == AUDCLNT_S_BUFFER_EMPTY || n == 0) break;
                if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) sh->capDiscont++;
                sh->capPktDevPos = devPos;
                sh->capPktQpc100ns = qpc;
                sh->capFrames += n;
                cc->ReleaseBuffer(n);
            }
            if (FAILED(sh->fatal.load())) break;
        }
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
}
}  // namespace

int main(int argc, char** argv) {
    double seconds = 3600.0, periodMs = 10.0, progressS = 60.0;
    const char* csvPath = "drift.csv";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--seconds") seconds = atof(argv[i + 1]);
        else if (a == "--csv") csvPath = argv[i + 1];
        else if (a == "--period-ms") periodMs = atof(argv[i + 1]);
        else if (a == "--progress-s") progressS = atof(argv[i + 1]);
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(en.GetAddressOf()));
    if (FAILED(hr)) { printf("{\"ok\":false,\"error\":\"MMDeviceEnumerator 0x%08lX\"}\n", hr); return 1; }
    std::wstring capName, renName;
    ComPtr<IMMDevice> capDev = um2::FindEndpoint(en.Get(), eCapture, &capName);
    ComPtr<IMMDevice> renDev = um2::FindEndpoint(en.Get(), eRender, &renName);
    if (!capDev || !renDev) { printf("{\"ok\":false,\"error\":\"UM2 endpoints not found\"}\n"); return 1; }

    // Device periods (C5 evidence: minimum exclusive period of usbaudio.sys for this device)
    REFERENCE_TIME capDef = 0, capMin = 0, renDef = 0, renMin = 0;
    {
        ComPtr<IAudioClient> t;
        if (SUCCEEDED(capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(t.GetAddressOf()))))
            t->GetDevicePeriod(&capDef, &capMin);
        t.Reset();
        if (SUCCEEDED(renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(t.GetAddressOf()))))
            t->GetDevicePeriod(&renDef, &renMin);
    }

    WAVEFORMATEX fmt = um2::Pcm16(48000, 2);
    REFERENCE_TIME period = static_cast<REFERENCE_TIME>(periodMs * 10000.0 + 0.5), capUsed = 0, renUsed = 0;
    ComPtr<IAudioClient> cap, ren;
    HRESULT hrCap = um2::InitExclusiveEvent(capDev.Get(), cap, &fmt, period, &capUsed);
    HRESULT hrRen = um2::InitExclusiveEvent(renDev.Get(), ren, &fmt, period, &renUsed);
    if (FAILED(hrCap) || FAILED(hrRen)) {
        printf("{\"ok\":false,\"error\":\"exclusive init\",\"hr_capture\":\"0x%08lX\",\"hr_render\":\"0x%08lX\"}\n", hrCap, hrRen);
        return 2;
    }
    UINT32 capBuf = 0, renBuf = 0;
    cap->GetBufferSize(&capBuf);
    ren->GetBufferSize(&renBuf);
    HANDLE capEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr), renEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    cap->SetEventHandle(capEvt);
    ren->SetEventHandle(renEvt);
    ComPtr<IAudioClock> capClk, renClk;
    cap->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(capClk.GetAddressOf()));
    ren->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(renClk.GetAddressOf()));
    UINT64 capFreq = 0, renFreq = 0;
    capClk->GetFrequency(&capFreq);
    renClk->GetFrequency(&renFreq);

    {   // pre-roll: one full buffer of silence before Start (exclusive event-driven requirement)
        ComPtr<IAudioRenderClient> rc;
        ren->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(rc.GetAddressOf()));
        BYTE* p = nullptr;
        if (SUCCEEDED(rc->GetBuffer(renBuf, &p))) rc->ReleaseBuffer(renBuf, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    // _SH_DENYWR: other processes may read the files while the run is in progress
    FILE* csv = _fsopen(csvPath, "w", _SH_DENYWR);
    if (!csv) { printf("{\"ok\":false,\"error\":\"csv open\"}\n"); return 3; }
    fprintf(csv, "# um2_drift v1 capture=\"%s\" render=\"%s\" cap_freq=%llu ren_freq=%llu cap_buf=%u ren_buf=%u\n",
            um2::Narrow(capName).c_str(), um2::Narrow(renName).c_str(), capFreq, renFreq, capBuf, renBuf);
    fprintf(csv, "t_qpc_s,cap_pos_s,cap_pos_qpc_s,ren_pos_s,ren_pos_qpc_s,cap_pkt_devpos,cap_pkt_qpc_s,cap_frames,ren_frames,cap_discont,timeouts\n");

    Shared sh;
    LARGE_INTEGER qf, q0, qn{};
    QueryPerformanceFrequency(&qf);
    cap->Start();
    ren->Start();
    QueryPerformanceCounter(&q0);
    std::thread th(AudioThread, cap.Get(), ren.Get(), capEvt, renEvt, renBuf, &sh);

    std::string progPath = std::string(csvPath) + ".progress.log";
    FILE* prog = _fsopen(progPath.c_str(), "w", _SH_DENYWR);
    char stamp[32];
    if (prog) {
        LocalStamp(stamp, sizeof(stamp));
        fprintf(prog, "%s START capture=\"%s\" render=\"%s\" buf=%u/%u frames\n", stamp,
                um2::Narrow(capName).c_str(), um2::Narrow(renName).c_str(), capBuf, renBuf);
        fflush(prog);
    }
    bool haveRef = false;
    double cp0 = 0, cq0 = 0, rp0 = 0, rq0 = 0, nextProgress = progressS;
    for (;;) {
        if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) break;
        QueryPerformanceCounter(&qn);
        double t = static_cast<double>(qn.QuadPart - q0.QuadPart) / static_cast<double>(qf.QuadPart);
        UINT64 cp = 0, cq = 0, rp = 0, rq = 0;
        HRESULT h1 = capClk->GetPosition(&cp, &cq);
        HRESULT h2 = renClk->GetPosition(&rp, &rq);
        if (FAILED(h1) || FAILED(h2)) { sh.fatal = FAILED(h1) ? h1 : h2; SetEvent(g_stop); break; }
        const double cps = static_cast<double>(cp) / capFreq, cqs = cq * 1e-7;
        const double rps = static_cast<double>(rp) / renFreq, rqs = rq * 1e-7;
        if (!haveRef) { cp0 = cps; cq0 = cqs; rp0 = rps; rq0 = rqs; haveRef = true; }
        fprintf(csv, "%.6f,%.9f,%.7f,%.9f,%.7f,%llu,%.7f,%llu,%llu,%llu,%llu\n", t,
                static_cast<double>(cp) / capFreq, cq * 1e-7, static_cast<double>(rp) / renFreq, rq * 1e-7,
                sh.capPktDevPos.load(), sh.capPktQpc100ns.load() * 1e-7, sh.capFrames.load(), sh.renFrames.load(),
                sh.capDiscont.load(), sh.renTimeouts.load());
        fflush(csv);
        if (FAILED(sh.fatal.load())) { SetEvent(g_stop); break; }
        if (t >= nextProgress && prog) {
            nextProgress += progressS;
            const double ppmCap = (cqs > cq0) ? ((cps - cp0) / (cqs - cq0) - 1.0) * 1e6 : 0.0;
            const double ppmRen = (rqs > rq0) ? ((rps - rp0) / (rqs - rq0) - 1.0) * 1e6 : 0.0;
            LocalStamp(stamp, sizeof(stamp));
            fprintf(prog, "%s t=%.0fs qpc=%lld cap_frames=%llu ren_frames=%llu ppm_cap=%+.3f ppm_ren=%+.3f diff=%+.3f discont=%llu timeouts=%llu\n",
                    stamp, t, static_cast<long long>(qn.QuadPart), sh.capFrames.load(), sh.renFrames.load(),
                    ppmCap, ppmRen, ppmCap - ppmRen, sh.capDiscont.load(), sh.renTimeouts.load());
            fflush(prog);
        }
        if (t >= seconds) { SetEvent(g_stop); break; }
    }
    th.join();
    if (prog) {
        LocalStamp(stamp, sizeof(stamp));
        if (FAILED(sh.fatal.load()))
            fprintf(prog, "%s FATAL hr=0x%08lX (stream stopped)\n", stamp, static_cast<unsigned long>(sh.fatal.load()));
        else
            fprintf(prog, "%s END ok\n", stamp);
        fclose(prog);
    }
    cap->Stop();
    ren->Stop();
    fclose(csv);
    printf("{\"ok\":%s,\"fatal_hr\":\"0x%08lX\",\"mmcss_error\":%lu,\"capture\":\"%s\",\"render\":\"%s\","
           "\"period_requested_100ns\":%lld,\"capture_period_100ns\":%lld,\"render_period_100ns\":%lld,"
           "\"capture_buffer_frames\":%u,\"render_buffer_frames\":%u,"
           "\"device_period_capture\":{\"default_100ns\":%lld,\"min_100ns\":%lld},"
           "\"device_period_render\":{\"default_100ns\":%lld,\"min_100ns\":%lld},"
           "\"cap_frames\":%llu,\"ren_frames\":%llu,\"cap_discontinuities\":%llu,\"timeouts\":%llu,\"csv\":\"%s\"}\n",
           SUCCEEDED(sh.fatal.load()) ? "true" : "false", static_cast<unsigned long>(sh.fatal.load()), sh.mmcssError.load(),
           um2::JsonEscape(um2::Narrow(capName)).c_str(), um2::JsonEscape(um2::Narrow(renName)).c_str(),
           period, capUsed, renUsed, capBuf, renBuf, capDef, capMin, renDef, renMin,
           sh.capFrames.load(), sh.renFrames.load(), sh.capDiscont.load(), sh.renTimeouts.load(),
           um2::JsonEscape(csvPath).c_str());
    return SUCCEEDED(sh.fatal.load()) ? 0 : 4;
}
