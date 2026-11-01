// volume_watch: who changes the UM2 capture endpoint volume?
//
// Registers IAudioEndpointVolumeCallback on the UM2 capture endpoint ("USB Audio CODEC") and, on every
// change (plus a 5 s polling safety net and a start line), appends to the log: local time, level in dB,
// slider scalar, mute, the event-context GUID passed by whoever changed it (GUID_NULL = caller passed
// none) and every process that has an audio session on any active endpoint at that moment.
// Read-only: never writes the volume.
//
// Usage: volume_watch [--log path]   (default %LOCALAPPDATA%\KaxWav\UM2\volume_watch.log)
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <share.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {
FILE* g_log = nullptr;
HANDLE g_changed = nullptr;
std::mutex g_mx;
struct Note { GUID ctx; float scalar; BOOL muted; };
std::vector<Note> g_notes;

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::string Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[40];
    snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}
std::string GuidStr(const GUID& g) {
    if (g == GUID_NULL) return "GUID_NULL";
    wchar_t w[64];
    StringFromGUID2(g, w, 64);
    return Narrow(w);
}
void Log(const std::string& line) {
    if (!g_log) return;
    fprintf(g_log, "%s\n", line.c_str());
    fflush(g_log);
}

class VolumeCallback final : public IAudioEndpointVolumeCallback {
    LONG ref_ = 1;
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Called on an MMDevAPI thread: copy and signal, the logging happens on the main thread.
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (!d) return E_POINTER;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_notes.push_back({d->guidEventContext, d->fMasterVolume, d->bMuted});
        }
        SetEvent(g_changed);
        return S_OK;
    }
};

ComPtr<IMMDevice> FindUm2Capture(IMMDeviceEnumerator* e, std::string* name) {
    ComPtr<IMMDeviceCollection> c;
    if (FAILED(e->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &c))) return nullptr;
    UINT n = 0;
    c->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        ComPtr<IPropertyStore> s;
        if (FAILED(c->Item(i, &d)) || FAILED(d->OpenPropertyStore(STGM_READ, &s))) continue;
        PROPVARIANT v;
        PropVariantInit(&v);
        bool hit = SUCCEEDED(s->GetValue(PKEY_DeviceInterface_FriendlyName, &v)) && v.vt == VT_LPWSTR &&
                   wcsstr(v.pwszVal, L"USB Audio CODEC");
        PropVariantClear(&v);
        if (hit) {
            PROPVARIANT f;
            PropVariantInit(&f);
            if (name && SUCCEEDED(s->GetValue(PKEY_Device_FriendlyName, &f)) && f.vt == VT_LPWSTR) *name = Narrow(f.pwszVal);
            PropVariantClear(&f);
            return d;
        }
    }
    return nullptr;
}

std::string ProcessName(DWORD pid) {
    if (pid == 0) return "(sistema)";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "(sem acesso)";
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    std::string out = "(?)";
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        out = Narrow(base ? base + 1 : path);
    }
    CloseHandle(h);
    return out;
}

void LogSessions(IMMDeviceEnumerator* e) {
    for (EDataFlow flow : {eCapture, eRender}) {
        ComPtr<IMMDeviceCollection> c;
        if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &c))) continue;
        UINT n = 0;
        c->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> d;
            ComPtr<IPropertyStore> ps;
            if (FAILED(c->Item(i, &d))) continue;
            std::string ep = "?";
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT f;
                PropVariantInit(&f);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &f)) && f.vt == VT_LPWSTR) ep = Narrow(f.pwszVal);
                PropVariantClear(&f);
            }
            ComPtr<IAudioSessionManager2> mgr;
            ComPtr<IAudioSessionEnumerator> en;
            if (FAILED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(mgr.GetAddressOf()))) ||
                FAILED(mgr->GetSessionEnumerator(&en))) continue;
            int count = 0;
            en->GetCount(&count);
            for (int k = 0; k < count; ++k) {
                ComPtr<IAudioSessionControl> sc;
                ComPtr<IAudioSessionControl2> sc2;
                if (FAILED(en->GetSession(k, &sc)) || FAILED(sc.As(&sc2))) continue;
                DWORD pid = 0;
                sc2->GetProcessId(&pid);
                AudioSessionState st = AudioSessionStateInactive;
                sc2->GetState(&st);
                if (st == AudioSessionStateExpired) continue;
                const bool sys = sc2->IsSystemSoundsSession() == S_OK;
                char line[512];
                snprintf(line, sizeof(line), "    [%s] %s: pid %lu %s %s%s", flow == eCapture ? "captura" : "render", ep.c_str(),
                         pid, ProcessName(pid).c_str(), st == AudioSessionStateActive ? "ATIVA" : "inativa", sys ? " (sons do sistema)" : "");
                Log(line);
            }
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    std::string logPath;
    for (int i = 1; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--log") logPath = argv[i + 1];
    if (logPath.empty()) {
        char lad[MAX_PATH];
        if (GetEnvironmentVariableA("LOCALAPPDATA", lad, MAX_PATH)) {
            std::string dir = std::string(lad) + "\\KaxWav\\UM2";
            CreateDirectoryA((std::string(lad) + "\\KaxWav").c_str(), nullptr);
            CreateDirectoryA(dir.c_str(), nullptr);
            logPath = dir + "\\volume_watch.log";
        } else {
            logPath = "volume_watch.log";
        }
    }
    g_log = _fsopen(logPath.c_str(), "a", _SH_DENYWR);
    if (!g_log) return 1;
    g_changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(en.GetAddressOf())))) {
        Log(Stamp() + " ERRO MMDeviceEnumerator");
        return 2;
    }
    Log(Stamp() + " START volume_watch pid " + std::to_string(GetCurrentProcessId()));

    ComPtr<IAudioEndpointVolume> vol;
    VolumeCallback* cb = nullptr;
    float lastDb = NAN;
    for (;;) {
        if (!vol) {  // (re)attach to the UM2 capture endpoint
            std::string name;
            ComPtr<IMMDevice> dev = FindUm2Capture(en.Get(), &name);
            if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(vol.GetAddressOf())))) {
                cb = new VolumeCallback();
                vol->RegisterControlChangeNotify(cb);
                float db = 0, sc = 0;
                BOOL mute = FALSE;
                vol->GetMasterVolumeLevel(&db);
                vol->GetMasterVolumeLevelScalar(&sc);
                vol->GetMute(&mute);
                char line[256];
                snprintf(line, sizeof(line), "%s ANEXADO a \"%s\": %+.2f dB (slider %.3f)%s", Stamp().c_str(), name.c_str(), db, sc,
                         mute ? " MUDO" : "");
                Log(line);
                LogSessions(en.Get());
                lastDb = db;
            } else {
                Sleep(5000);
                continue;
            }
        }
        const DWORD w = WaitForSingleObject(g_changed, 5000);
        float db = 0, sc = 0;
        BOOL mute = FALSE;
        if (FAILED(vol->GetMasterVolumeLevel(&db)) || FAILED(vol->GetMasterVolumeLevelScalar(&sc))) {
            Log(Stamp() + " endpoint perdido (desconectado?) - tentando reanexar");
            if (cb) { vol->UnregisterControlChangeNotify(cb); cb->Release(); cb = nullptr; }
            vol.Reset();
            continue;
        }
        vol->GetMute(&mute);
        std::vector<Note> notes;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            notes.swap(g_notes);
        }
        const bool polledChange = (w == WAIT_TIMEOUT) && std::fabs(db - lastDb) > 0.005f;
        if (notes.empty() && !polledChange) continue;
        char line[384];
        snprintf(line, sizeof(line), "%s MUDANCA %+.2f dB -> %+.2f dB (slider %.3f)%s | contexto: %s | origem: %s", Stamp().c_str(),
                 lastDb, db, sc, mute ? " MUDO" : "", notes.empty() ? "-" : GuidStr(notes.back().ctx).c_str(),
                 notes.empty() ? "polling (sem notificacao)" : (std::to_string(notes.size()) + " notificacao(oes)").c_str());
        Log(line);
        LogSessions(en.Get());
        lastDb = db;
    }
}
