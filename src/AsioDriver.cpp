#include "AsioDriver.h"
#include "UM2Ipc.h"
#include <sddl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cstdarg>
#include <cmath>

static void LogAsio(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    OutputDebugStringA("[BehringerUM2ASIO] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::wstring logDir = std::wstring(localAppData) + L"\\KaxWav\\UM2";
        CreateDirectoryW(logDir.c_str(), nullptr);
        std::wstring logPath = logDir + L"\\driver_um2.log";
        FILE* f = nullptr;
        _wfopen_s(&f, logPath.c_str(), L"a");
        if (f) {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            time_t t = std::chrono::system_clock::to_time_t(now);
            struct tm tmBuf;
            localtime_s(&tmBuf, &t);
            fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", 
                    tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, static_cast<int>(ms.count()), buffer);
            fflush(f);
            fclose(f);
        }
    }
}

// Invariante 6: APOs desligados com AUDCLNT_STREAMOPTIONS_RAW em todo stream shared da UM2.
// Tem de rodar depois do Activate e antes de GetMixFormat/Initialize: o mix format em RAW pode
// ser diferente do padrao (na UM2 desta maquina a captura RAW vem mono, como o formato do device).
static HRESULT ApplyRawStreamOption(IAudioClient* client, const char* what) {
    IAudioClient2* client2 = nullptr;
    HRESULT hr = client ? client->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&client2)) : E_POINTER;
    if (SUCCEEDED(hr) && client2) {
        AudioClientProperties props{};
        props.cbSize = sizeof(props);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Other;
        props.Options = AUDCLNT_STREAMOPTIONS_RAW;
        hr = client2->SetClientProperties(&props);
        client2->Release();
    }
    LogAsio("%s: SetClientProperties(AUDCLNT_STREAMOPTIONS_RAW) hr=0x%08X", what, hr);
    return hr;
}

// Bit-perfect float -> int16 for the DAW path: no dither, no soft-clip. A sample that came from the
// 16-bit converter (k/32768) maps back to exactly k.
static inline int16_t ExactToInt16(float x) {
    const float scaled = x * 32768.0f;
    const int32_t r = static_cast<int32_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, r)));
}

// GUIDs
const GUID IID_IASIO = 
    { 0x933923e0, 0x01fe, 0x11d2, { 0xbc, 0x7b, 0x00, 0x80, 0xc7, 0x79, 0xb4, 0x3e } };

static const PROPERTYKEY PKEY_Device_DeviceDesc_Local = 
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };

static const PROPERTYKEY PKEY_DeviceInterface_FriendlyName_Local = 
    { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 6 };

static bool ContainsCaseInsensitive(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle) return false;
    std::wstring h(haystack);
    std::wstring n(needle);
    std::transform(h.begin(), h.end(), h.begin(), ::towlower);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return (h.find(n) != std::wstring::npos);
}

BehringerUM2AsioDriver::BehringerUM2AsioDriver() {
    LogAsio("=== BehringerUM2AsioDriver() Constructor v3.0 ===");
    CoInitialize(nullptr);
    QueryPerformanceFrequency(&m_qpcFreq);
    SetupAllDsp(static_cast<float>(m_sampleRate));
    LoadSavedSettings();
}

void BehringerUM2AsioDriver::SetupAllDsp(float sr) {
    m_hpfCh1.SetupHighPass(sr, 80.0f);
    m_gateCh1.Setup(sr);
    m_gateCh2.Setup(sr);
    m_clipCh1.Setup(sr);
    m_clipCh2.Setup(sr);
    m_preampCh1.Setup(sr);
    m_preampCh2.Setup(sr);
    m_deesserCh1.Setup(sr);
    m_compCh1.Setup(sr);
    m_compCh2.Setup(sr);
    m_cabCh2.Setup(sr);
    m_tuner.Setup(sr);
    m_masterMon.Setup(sr);
}

void BehringerUM2AsioDriver::UpdateLatencyTelemetry() {
    long pdcOffset = m_pSharedState ? m_pSharedState->pdcOffsetSamples : m_pdcOffsetSamples;
    long baseIn = 0;
    long baseOut = 0;
    if (m_isSharedModeActive || m_driverMode == DriverEngineMode::SharedFlex) {
        long eng = static_cast<long>(m_enginePeriodFrames > 0 ? m_enginePeriodFrames : 480);
        baseIn = eng + m_bufferSize * 2;
        baseOut = eng + m_bufferSize * 2;
    } else {
        long capHw = static_cast<long>(m_exclusiveCaptureFrames > 0 ? m_exclusiveCaptureFrames : m_bufferSize);
        long renHw = static_cast<long>(m_exclusiveRenderFrames > 0 ? m_exclusiveRenderFrames : m_bufferSize);
        baseIn = capHw + m_bufferSize;
        baseOut = renHw + m_bufferSize;
    }
    long inLat = (std::max)(32L, baseIn + pdcOffset);
    long outLat = (std::max)(32L, baseOut);
    if (m_pSharedState) {
        m_pSharedState->inputLatencySamples = static_cast<uint32_t>(inLat);
        m_pSharedState->outputLatencySamples = static_cast<uint32_t>(outLat);
    }
}

BehringerUM2AsioDriver::~BehringerUM2AsioDriver() {
    LogAsio("=== BehringerUM2AsioDriver() Destructor ===");
    m_stopIpcThread = true;
    if (m_ipcThread.joinable()) {
        m_ipcThread.join();
    }
    stop();
    disposeBuffers();
    m_processLoopback.Shutdown();
    ReleaseEndpoints();
    CloseTelemetry();
    if (m_hStreamsStarted) { CloseHandle(m_hStreamsStarted); m_hStreamsStarted = nullptr; }
    CloseSharedMemory();
    CoUninitialize();
}

void BehringerUM2AsioDriver::ReadDawDspFlags() {
    DWORD v1 = 0, v2 = 0, cb = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", L"DawDspCh1", RRF_RT_REG_DWORD, nullptr, &v1, &cb) != ERROR_SUCCESS) v1 = 0;
    cb = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", L"DawDspCh2", RRF_RT_REG_DWORD, nullptr, &v2, &cb) != ERROR_SUCCESS) v2 = 0;
    m_dawDspCh1.store(v1 != 0, std::memory_order_relaxed);
    m_dawDspCh2.store(v2 != 0, std::memory_order_relaxed);
}

void BehringerUM2AsioDriver::LoadSavedSettings() {
    ReadDawDspFlags();
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwMode = 1; // Default: SharedFlex (1)
        DWORD cbData = sizeof(dwMode);
        DWORD dwType = REG_DWORD;
        if (RegQueryValueExW(hKey, L"DriverMode", nullptr, &dwType, reinterpret_cast<BYTE*>(&dwMode), &cbData) == ERROR_SUCCESS) {
            m_driverMode = (dwMode == 0) ? DriverEngineMode::ExclusivePro : DriverEngineMode::SharedFlex;
            LogAsio("Modo do driver carregado do Registro: %s", (m_driverMode == DriverEngineMode::SharedFlex ? "SharedFlex (1)" : "ExclusivePro (0)"));
        }

        DWORD dwSize = 0;
        cbData = sizeof(dwSize);
        if (RegQueryValueExW(hKey, L"BufferSize", nullptr, &dwType, reinterpret_cast<BYTE*>(&dwSize), &cbData) == ERROR_SUCCESS) {
            long minAllowed = 32;
            if (dwSize >= static_cast<DWORD>(minAllowed) && dwSize <= 2048) {
                m_preferredBufferSize = dwSize;
                LogAsio("Buffer preferido carregado do Registro: %ld samples", m_preferredBufferSize);
            }
        }

        DWORD dwPdc = 0;
        cbData = sizeof(dwPdc);
        if (RegQueryValueExW(hKey, L"PdcOffsetSamples", nullptr, &dwType, reinterpret_cast<BYTE*>(&dwPdc), &cbData) == ERROR_SUCCESS) {
            int32_t pdc = static_cast<int32_t>(dwPdc);
            if (pdc >= -2048 && pdc <= 2048) {
                m_pdcOffsetSamples = pdc;
            }
        }

        DWORD dwHwModel = 0;
        cbData = sizeof(dwHwModel);
        if (RegQueryValueExW(hKey, L"HardwareModel", nullptr, &dwType, reinterpret_cast<BYTE*>(&dwHwModel), &cbData) == ERROR_SUCCESS) {
            if (dwHwModel <= 1) {
                m_detectedHardwareModel = static_cast<int32_t>(dwHwModel);
            }
        }
        RegCloseKey(hKey);
    }

    if (m_pSharedState && m_pSharedState->reloadFlag == 1) {
        if (m_pSharedState->requestedDriverMode <= 1) {
            m_driverMode = static_cast<DriverEngineMode>(m_pSharedState->requestedDriverMode);
        }
        long minAllowed = 32;
        if (m_pSharedState->requestedBufferSize >= static_cast<uint32_t>(minAllowed) && m_pSharedState->requestedBufferSize <= 2048) {
            m_preferredBufferSize = m_pSharedState->requestedBufferSize;
            LogAsio("Buffer preferido carregado do requestedBufferSize: %ld samples", m_preferredBufferSize);
        }
    }
}

void BehringerUM2AsioDriver::SaveCurrentSettings() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD dwSize = static_cast<DWORD>(m_preferredBufferSize);
        RegSetValueExW(hKey, L"BufferSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwSize), sizeof(dwSize));
        DWORD dwMode = (m_driverMode == DriverEngineMode::SharedFlex) ? 1 : 0;
        RegSetValueExW(hKey, L"DriverMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwMode), sizeof(dwMode));
        if (m_pSharedState) {
            DWORD dwPdc = static_cast<DWORD>(m_pSharedState->pdcOffsetSamples);
            RegSetValueExW(hKey, L"PdcOffsetSamples", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwPdc), sizeof(dwPdc));
            DWORD dwHw = static_cast<DWORD>(m_pSharedState->hardwareModel);
            RegSetValueExW(hKey, L"HardwareModel", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwHw), sizeof(dwHw));
        }
        RegCloseKey(hKey);
    }
    if (m_pSharedState) {
        m_pSharedState->bufferSize = static_cast<uint32_t>(m_bufferSize);
        m_pSharedState->driverMode = static_cast<uint32_t>(m_driverMode);
    }
    UpdateLatencyTelemetry();
}

// Security for the driver's named sections ("Local\BehringerUM2*"). Default DACL of an elevated host would lock out
// the non-elevated panel (High integrity, Administrators-only). Built once, outside the audio thread.
static SECURITY_ATTRIBUTES* SharedSectionSecurity() {
    static SECURITY_ATTRIBUTES sa{};
    static PSECURITY_DESCRIPTOR sd = nullptr;
    static bool tried = false;
    if (tried) return sd ? &sa : nullptr;
    tried = true;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return nullptr;
    BYTE buf[256]{};
    DWORD len = 0;
    LPWSTR sidStr = nullptr;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &sidStr)) {
        const std::wstring sddl = std::wstring(L"D:(A;;GA;;;") + sidStr + L")(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NW;;;ME)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
        }
        LocalFree(sidStr);
    }
    CloseHandle(tok);
    return sd ? &sa : nullptr;
}

void BehringerUM2AsioDriver::InitSharedMemory() {
    if (m_pSharedState) return;

    m_hSharedMem = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        SharedSectionSecurity(),
        PAGE_READWRITE,
        0,
        sizeof(UM2SharedState),
        UM2_SHARED_MEM_NAME
    );

    if (m_hSharedMem) {
        bool isFirst = (GetLastError() != ERROR_ALREADY_EXISTS);
        m_pSharedState = static_cast<UM2SharedState*>(
            MapViewOfFile(m_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UM2SharedState))
        );
        if (m_pSharedState && isFirst) {
            std::memset(m_pSharedState, 0, sizeof(UM2SharedState));
            m_pSharedState->magic = UM2_IPC_MAGIC;
            m_pSharedState->version = 4;
            m_pSharedState->sampleRate = static_cast<uint32_t>(m_sampleRate);
            m_pSharedState->bufferSize = static_cast<uint32_t>(m_preferredBufferSize);
            m_pSharedState->inputChannels = NUM_INPUTS;
            m_pSharedState->outputChannels = NUM_OUTPUTS;
            m_pSharedState->loopbackEnabled = 1;
            m_pSharedState->loopbackGain = 1.0f;
            m_pSharedState->driverStatus = 1;
            m_pSharedState->driverMode = static_cast<uint32_t>(m_driverMode);
            m_pSharedState->requestedDriverMode = static_cast<uint32_t>(m_driverMode);
            m_pSharedState->ch1Trim = 1.0f;
            m_pSharedState->ch2Trim = 1.0f;
            m_pSharedState->masterTrim = 1.0f;
            m_pSharedState->gateThresholdDb = -52.0f;
            m_pSharedState->isPhysicalHardware = 1;
            m_pSharedState->tunerSource = 1;
            m_pSharedState->pdcOffsetSamples = m_pdcOffsetSamples;
            m_pSharedState->hardwareModel = m_detectedHardwareModel;
        } else if (m_pSharedState && m_pSharedState->reloadFlag == 0) {
            m_pSharedState->version = 4;
            m_pSharedState->driverMode = static_cast<uint32_t>(m_driverMode);
            m_pSharedState->requestedDriverMode = static_cast<uint32_t>(m_driverMode);
            m_pSharedState->bufferSize = static_cast<uint32_t>(m_preferredBufferSize);
        }
        UpdateLatencyTelemetry();
        CheckLicenseStatus();
    }
}

void BehringerUM2AsioDriver::CheckLicenseStatus() {
    auto lic = UM2License::CheckLicense();
    m_isLicensed = lic.isLicensed;
    m_licenseTier = lic.tier;
    if (m_pSharedState) {
        m_pSharedState->isLicensed = lic.isLicensed ? 1 : 0;
        m_pSharedState->licenseDaysLeft = lic.daysLeft;
        strncpy_s(m_pSharedState->licenseTier, lic.tier.c_str(), sizeof(m_pSharedState->licenseTier) - 1);
        strncpy_s(m_pSharedState->machineHwid, lic.hwid.c_str(), sizeof(m_pSharedState->machineHwid) - 1);
    }
}

void BehringerUM2AsioDriver::IpcWatcherLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uint32_t licCounter = 0;
    while (!m_stopIpcThread.load(std::memory_order_relaxed)) {
        CheckAndHandleReloadRequest();
        if (m_pSharedState && m_active) {
            m_processLoopback.SyncConfiguration(
                m_pSharedState->loopbackMode,
                m_pSharedState->loopbackTargetPid,
                static_cast<uint32_t>(m_sampleRate)
            );
            UpdateLatencyTelemetry();
        }
        const uint32_t reprimes = m_renderReprimes.load(std::memory_order_relaxed);
        if (reprimes != m_renderReprimesLogged) {
            LogAsio("ExclusivePro: render re-preparado (fila vazia em eventos seguidos) - total %u", reprimes);
            m_renderReprimesLogged = reprimes;
        }
        if (licCounter % 14 == 0) ReadDawDspFlags();  // painel liga/desliga DSP na DAW sem reiniciar o stream
        if (++licCounter >= 60) {
            licCounter = 0;
            CheckLicenseStatus();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CoUninitialize();
}

void BehringerUM2AsioDriver::CheckAndHandleReloadRequest() {
    if (!m_pSharedState) return;
    uint32_t reload = m_pSharedState->reloadFlag;
    if (reload != 1) return;

    uint32_t req = m_pSharedState->requestedBufferSize;
    uint32_t reqMode = m_pSharedState->requestedDriverMode;

    bool modeChanged = (reqMode <= 1 && reqMode != static_cast<uint32_t>(m_driverMode));
    long minAllowed = (reqMode == 1 || (reqMode > 1 && m_driverMode == DriverEngineMode::SharedFlex)) ? 32 : 144;

    if (req > 0 && req < static_cast<uint32_t>(minAllowed)) req = static_cast<uint32_t>(minAllowed);

    bool expected = false;
    if (!m_handlingReload.compare_exchange_strong(expected, true)) return;

    LogAsio("KaxWav solicitou reload (reqSize=%u, reqMode=%u, reloadFlag=%u). Enviando kAsioResetRequest ao host...",
            req, reqMode, reload);

    if (modeChanged || reqMode <= 1) {
        m_driverMode = static_cast<DriverEngineMode>(reqMode);
    }
    if (req >= static_cast<uint32_t>(minAllowed) && req <= 2048) {
        m_preferredBufferSize = req;
    }

    m_pSharedState->reloadFlag = 0;
    m_pSharedState->requestedBufferSize = 0;
    m_pSharedState->requestedDriverMode = static_cast<uint32_t>(m_driverMode);
    m_pSharedState->driverMode = static_cast<uint32_t>(m_driverMode);

    SaveCurrentSettings();

    if (m_callbacks && m_callbacks->asioMessage) {
        LogAsio("Disparando asioMessage(kAsioResetRequest, 0, nullptr, nullptr)...");
        m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

    m_handlingReload = false;
}

void BehringerUM2AsioDriver::CloseSharedMemory() {
    if (m_pSharedState) {
        m_pSharedState->isStreaming = 0;
        m_pSharedState->driverStatus = 0;
        UnmapViewOfFile(m_pSharedState);
        m_pSharedState = nullptr;
    }
    if (m_hSharedMem) {
        CloseHandle(m_hSharedMem);
        m_hSharedMem = nullptr;
    }
}

// M5: called from start() before the audio thread exists (never from the audio thread).
void BehringerUM2AsioDriver::OpenTelemetry() {
    if (!m_pTelemetry) {
        m_hTelemetry = CreateFileMappingW(INVALID_HANDLE_VALUE, SharedSectionSecurity(), PAGE_READWRITE, 0,
                                          static_cast<DWORD>(sizeof(UM2Telemetry)), UM2_TELEMETRY_NAME);
        if (!m_hTelemetry) {
            LogAsio("Telemetria M5 indisponivel: CreateFileMappingW falhou (%lu)", GetLastError());
            return;
        }
        m_pTelemetry = static_cast<UM2Telemetry*>(MapViewOfFile(m_hTelemetry, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UM2Telemetry)));
        if (!m_pTelemetry) {
            LogAsio("Telemetria M5 indisponivel: MapViewOfFile falhou (%lu)", GetLastError());
            CloseHandle(m_hTelemetry);
            m_hTelemetry = nullptr;
            return;
        }
    }
    const uint32_t nextSession = (m_pTelemetry->magic == UM2_TELEMETRY_MAGIC) ? m_pTelemetry->sessionId + 1 : 1;
    std::memset(m_pTelemetry, 0, sizeof(UM2Telemetry));
    m_pTelemetry->version = UM2_TELEMETRY_VERSION;
    m_pTelemetry->ownerPid = GetCurrentProcessId();
    m_pTelemetry->sessionId = nextSession;
    m_pTelemetry->qpcFrequency = m_qpcFreq.QuadPart;
    m_pTelemetry->sampleRate = static_cast<uint32_t>(m_sampleRate);
    m_pTelemetry->bufferSize = static_cast<uint32_t>(m_bufferSize);
    m_pTelemetry->budgetTicks = (m_sampleRate > 0.0)
        ? static_cast<int64_t>(static_cast<double>(m_qpcFreq.QuadPart) * m_bufferSize / m_sampleRate)
        : 0;
    m_pTelemetry->histBinNs = UM2_TIMING_BIN_NS;
    m_pTelemetry->histBins = UM2_TIMING_HIST_BINS;
    std::atomic_thread_fence(std::memory_order_release);
    m_pTelemetry->magic = UM2_TELEMETRY_MAGIC;
}

void BehringerUM2AsioDriver::CloseTelemetry() {
    if (m_pTelemetry) {
        UnmapViewOfFile(m_pTelemetry);
        m_pTelemetry = nullptr;
    }
    if (m_hTelemetry) {
        CloseHandle(m_hTelemetry);
        m_hTelemetry = nullptr;
    }
}

void BehringerUM2AsioDriver::ReleaseEndpoints() {
    ReleaseCableEndpoint();
    if (m_pRenderService) { m_pRenderService->Release(); m_pRenderService = nullptr; }
    if (m_pCaptureService) { m_pCaptureService->Release(); m_pCaptureService = nullptr; }
    if (m_pRenderClient) { m_pRenderClient->Release(); m_pRenderClient = nullptr; }
    if (m_pCaptureClient) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
    if (m_pRenderDevice) { m_pRenderDevice->Release(); m_pRenderDevice = nullptr; }
    if (m_pCaptureDevice) { m_pCaptureDevice->Release(); m_pCaptureDevice = nullptr; }

    if (m_hRenderEvent) { CloseHandle(m_hRenderEvent); m_hRenderEvent = nullptr; }
    if (m_hCaptureEvent) { CloseHandle(m_hCaptureEvent); m_hCaptureEvent = nullptr; }
}

void BehringerUM2AsioDriver::ReleaseCableEndpoint() {
    if (m_pCableClient) {
        m_pCableClient->Stop();
        m_pCableClient->Release();
        m_pCableClient = nullptr;
    }
    if (m_pCableRenderService) {
        m_pCableRenderService->Release();
        m_pCableRenderService = nullptr;
    }
    if (m_pCableDevice) {
        m_pCableDevice->Release();
        m_pCableDevice = nullptr;
    }
}

void BehringerUM2AsioDriver::InitCableEndpoint() {
    ReleaseCableEndpoint();

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    if (FAILED(hr) || !pEnum) return;

    IMMDeviceCollection* pCol = nullptr;
    pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
    if (pCol) {
        UINT count = 0;
        pCol->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                IPropertyStore* pStore = nullptr;
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                        std::wstring name = pv.pwszVal;
                        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                        if (name.find(L"16 ch") == std::wstring::npos &&
                            (name.find(L"cable input") != std::wstring::npos ||
                             name.find(L"vb-audio") != std::wstring::npos ||
                             name.find(L"voicemeeter") != std::wstring::npos)) {
                            m_pCableDevice = pDev;
                            m_pCableDevice->AddRef();
                            PropVariantClear(&pv);
                            pStore->Release();
                            pDev->Release();
                            break;
                        }
                    }
                    PropVariantClear(&pv);
                    pStore->Release();
                }
                pDev->Release();
            }
        }
        pCol->Release();
    }
    pEnum->Release();

    if (m_pCableDevice) {
        hr = m_pCableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCableClient));
        if (SUCCEEDED(hr) && m_pCableClient) {
            WAVEFORMATEX* pMix = nullptr;
            hr = m_pCableClient->GetMixFormat(&pMix);
            if (SUCCEEDED(hr) && pMix) {
                m_cableChannels = (std::max)(1u, static_cast<UINT32>(pMix->nChannels));
                DWORD origCableRate = pMix->nSamplesPerSec;
                if (pMix->nSamplesPerSec != static_cast<DWORD>(m_sampleRate)) {
                    pMix->nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
                    pMix->nAvgBytesPerSec = pMix->nSamplesPerSec * pMix->nBlockAlign;
                }
                hr = m_pCableClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    600000, // 60ms para absorver jitter entre clocks USB e VB-Cable
                    0,
                    pMix,
                    nullptr
                );
                if (FAILED(hr)) {
                    pMix->nSamplesPerSec = origCableRate;
                    pMix->nAvgBytesPerSec = pMix->nSamplesPerSec * pMix->nBlockAlign;
                    m_pCableClient->Release();
                    m_pCableClient = nullptr;
                    m_pCableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCableClient));
                    if (m_pCableClient) {
                        hr = m_pCableClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 600000, 0, pMix, nullptr);
                    }
                }
                if (SUCCEEDED(hr) && m_pCableClient) {
                    m_pCableClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_pCableRenderService));
                    m_lastCableL = 0.0f;
                    m_lastCableR = 0.0f;
                    m_cableGateCh2.Setup(static_cast<float>(m_sampleRate));
                    // Pre-roll de 1200 frames (~25ms) antes de Start() para evitar underrun de 50Hz a 64 samples
                    if (m_pCableRenderService) {
                        UINT32 cableBufSize = 0;
                        m_pCableClient->GetBufferSize(&cableBufSize);
                        UINT32 preRoll = (std::min)(1200u, cableBufSize / 2u);
                        BYTE* pPre = nullptr;
                        if (preRoll > 0 && SUCCEEDED(m_pCableRenderService->GetBuffer(preRoll, &pPre)) && pPre) {
                            m_pCableRenderService->ReleaseBuffer(preRoll, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                    }
                    m_pCableClient->Start();
                    LogAsio("Cabo Virtual (VB-Audio) conectado com sucesso ao driver ASIO (channels=%u)!", m_cableChannels);
                } else {
                    ReleaseCableEndpoint();
                }
                CoTaskMemFree(pMix);
            } else {
                ReleaseCableEndpoint();
            }
        }
    }
}

void BehringerUM2AsioDriver::FeedCableVirtual(long frames, bool loopBroadcast, float loopGain, float gateThresh, const float* pIn0, const float* pIn1, const int16_t* pOut0, const int16_t* pOut1) {
    if (!m_pCableRenderService || !m_pCableClient || frames <= 0 || !pIn0 || !pIn1) return;

    UINT32 padding = 0;
    if (FAILED(m_pCableClient->GetCurrentPadding(&padding))) return;

    UINT32 bufferSize = 0;
    m_pCableClient->GetBufferSize(&bufferSize);
    if (bufferSize == 0) return;

    const UINT32 nCh = (std::max)(1u, m_cableChannels);

    // Recuperação suave de colchão caso padding caia abaixo de 1 período WASAPI (480 frames)
    if (padding < 480 && (bufferSize - padding) >= static_cast<UINT32>(frames + 720)) {
        BYTE* pPadDst = nullptr;
        const UINT32 cushionFrames = 720;
        if (SUCCEEDED(m_pCableRenderService->GetBuffer(cushionFrames, &pPadDst)) && pPadDst) {
            float* pPadFloats = reinterpret_cast<float*>(pPadDst);
            for (UINT32 i = 0; i < cushionFrames; ++i) {
                float decay = 1.0f - (static_cast<float>(i) / static_cast<float>(cushionFrames));
                pPadFloats[i * nCh + 0] = m_lastCableL * decay;
                if (nCh >= 2) pPadFloats[i * nCh + 1] = m_lastCableR * decay;
                for (UINT32 ch = 2; ch < nCh; ++ch) pPadFloats[i * nCh + ch] = 0.0f;
            }
            m_pCableRenderService->ReleaseBuffer(cushionFrames, 0);
            m_lastCableL = 0.0f;
            m_lastCableR = 0.0f;
            padding += cushionFrames;
        }
    }

    // Controle de deriva de clock: se o buffer acumular mais de 2160 frames (~45ms), descarta 1 amostra suavemente
    long writeFrames = frames;
    if (padding > 2160 && frames > 4) {
        writeFrames = frames - 1;
    }

    UINT32 available = bufferSize - padding;
    if (available < static_cast<UINT32>(writeFrames)) return;

    BYTE* pDst = nullptr;
    if (SUCCEEDED(m_pCableRenderService->GetBuffer(static_cast<UINT32>(writeFrames), &pDst)) && pDst) {
        float* pFloats = reinterpret_cast<float*>(pDst);
        for (long s = 0; s < writeFrames; ++s) {
            float in0 = pIn0[s];
            // Gate automático no CH 2 para o cabo virtual, impedindo chiado do jack Hi-Z aberto no Discord
            float in1 = m_cableGateCh2.Process(pIn1[s], true, gateThresh);

            float micMix = in0 + in1;
            float out0 = micMix;
            float out1 = micMix;

            if (loopBroadcast && pOut0 && pOut1) {
                float pc0 = (static_cast<float>(pOut0[s]) / 32768.0f) * loopGain;
                float pc1 = (static_cast<float>(pOut1[s]) / 32768.0f) * loopGain;
                out0 = pc0 + micMix * 0.7071f;
                out1 = pc1 + micMix * 0.7071f;
            }

            float c0 = StudioDspProcessor::SoftClip(out0);
            float c1 = StudioDspProcessor::SoftClip(out1);
            pFloats[s * nCh + 0] = c0;
            if (nCh >= 2) pFloats[s * nCh + 1] = c1;
            for (UINT32 ch = 2; ch < nCh; ++ch) {
                pFloats[s * nCh + ch] = 0.0f;
            }
            m_lastCableL = c0;
            m_lastCableR = c1;
        }
        m_pCableRenderService->ReleaseBuffer(static_cast<UINT32>(writeFrames), 0);
    }
}

bool BehringerUM2AsioDriver::FindBehringerEndpoints() {
    ReleaseEndpoints();

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&pEnum)
    );
    if (FAILED(hr) || !pEnum) {
        LogAsio("Erro ao criar MMDeviceEnumerator: 0x%08X", hr);
        return false;
    }

    auto findDevice = [&](EDataFlow flow, IMMDevice** ppOutDev) {
        IMMDeviceCollection* pCol = nullptr;
        pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol);
        if (!pCol) return;

        UINT count = 0;
        pCol->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            pCol->Item(i, &pDev);
            if (!pDev) continue;

            IPropertyStore* pStore = nullptr;
            pDev->OpenPropertyStore(STGM_READ, &pStore);
            if (pStore) {
                bool matched = false;
                auto checkKey = [&](const PROPERTYKEY& key) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    if (SUCCEEDED(pStore->GetValue(key, &var)) && var.pwszVal) {
                        if (ContainsCaseInsensitive(var.pwszVal, L"behringer") || 
                            ContainsCaseInsensitive(var.pwszVal, L"usb audio codec") ||
                            ContainsCaseInsensitive(var.pwszVal, L"busb_audio") ||
                            ContainsCaseInsensitive(var.pwszVal, L"pcm2902") ||
                            ContainsCaseInsensitive(var.pwszVal, L"u-phoria") ||
                            ContainsCaseInsensitive(var.pwszVal, L"umc22") ||
                            ContainsCaseInsensitive(var.pwszVal, L"um2")) {
                            matched = true;
                            if (ContainsCaseInsensitive(var.pwszVal, L"umc22")) {
                                m_detectedHardwareModel = 1; // UMC22 (MIDAS Preamp)
                            }
                            char ansiName[256]{};
                            WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, ansiName, sizeof(ansiName), nullptr, nullptr);
                            LogAsio("Match Behringer U-Phoria Endpoint (%s, model=%d): %s",
                                    (flow == eRender ? "Render" : "Capture"), m_detectedHardwareModel, ansiName);
                        }
                    }
                    PropVariantClear(&var);
                };

                checkKey(PKEY_Device_FriendlyName);
                if (!matched) checkKey(PKEY_DeviceInterface_FriendlyName_Local);
                if (!matched) checkKey(PKEY_Device_DeviceDesc_Local);

                pStore->Release();

                if (matched) {
                    m_isPhysicalUM2 = true;
                    *ppOutDev = pDev;
                    (*ppOutDev)->AddRef();
                    pDev->Release();
                    break;
                }
            }
            pDev->Release();
        }
        pCol->Release();

        // Fallback para endpoint padrao se nao encontrar por substring
        if (!*ppOutDev) {
            m_isPhysicalUM2 = false;
            LogAsio("Aviso: Behringer UM2 nao encontrado explicitamente em %s. Usando padrao do Windows.", 
                    (flow == eRender ? "Render" : "Capture"));
            pEnum->GetDefaultAudioEndpoint(flow, eMultimedia, ppOutDev);
        }
    };

    findDevice(eRender, &m_pRenderDevice);
    findDevice(eCapture, &m_pCaptureDevice);

    if (m_pSharedState) {
        m_pSharedState->isPhysicalHardware = m_isPhysicalUM2 ? 1 : 0;
        if (m_detectedHardwareModel == 1) {
            m_pSharedState->hardwareModel = 1;
        }
    }

    pEnum->Release();
    return (m_pRenderDevice != nullptr || m_pCaptureDevice != nullptr);
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE BehringerUM2AsioDriver::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || 
        IsEqualIID(riid, IID_IASIO) || 
        IsEqualIID(riid, CLSID_BehringerUM2AsioDriver)) {
        *ppv = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE BehringerUM2AsioDriver::AddRef() { return ++m_refCount; }
ULONG STDMETHODCALLTYPE BehringerUM2AsioDriver::Release() {
    ULONG count = --m_refCount;
    if (count == 0) delete this;
    return count;
}

// ============================================================================
// IASIO Implementation
// ============================================================================

ASIOBool BehringerUM2AsioDriver::init(void* sysHandle) {
    LogAsio("init(sysHandle=%p)", sysHandle);
    m_sysHandle = sysHandle;
    InitSharedMemory();
    LoadSavedSettings();
    FindBehringerEndpoints();
    m_stopIpcThread = false;
    if (!m_ipcThread.joinable()) {
        m_ipcThread = std::thread(&BehringerUM2AsioDriver::IpcWatcherLoop, this);
    }
    return ASIOTrue;
}

void BehringerUM2AsioDriver::getDriverName(char* name) {
    LogAsio("getDriverName()");
    if (name) strcpy_s(name, 32, "Behringer U-Phoria ASIO");
}

long BehringerUM2AsioDriver::getDriverVersion() { return 300; }

void BehringerUM2AsioDriver::getErrorMessage(char* string) {
    if (string) strcpy_s(string, 128, "Operacao concluida com sucesso.");
}

ASIOError BehringerUM2AsioDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    if (!numInputChannels || !numOutputChannels) return ASE_InvalidParameter;
    *numInputChannels = NUM_INPUTS;   // 2 Fisicos (Mic/Line + Hi-Z Inst) + 2 Virtuais (Loopback L/R)
    *numOutputChannels = NUM_OUTPUTS; // 2 Fisicos (Master Out L/R)
    LogAsio("getChannels() -> in=%ld, out=%ld", *numInputChannels, *numOutputChannels);
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::getLatencies(long* inputLatency, long* outputLatency) {
    if (!inputLatency || !outputLatency) return ASE_InvalidParameter;
    UpdateLatencyTelemetry();
    if (m_pSharedState && m_pSharedState->inputLatencySamples > 0 && m_pSharedState->outputLatencySamples > 0) {
        *inputLatency = static_cast<long>(m_pSharedState->inputLatencySamples);
        *outputLatency = static_cast<long>(m_pSharedState->outputLatencySamples);
    } else {
        *inputLatency = m_bufferSize;
        *outputLatency = m_bufferSize;
    }
    LogAsio("getLatencies() -> in=%ld, out=%ld (PDC sample-accurate)", *inputLatency, *outputLatency);
    return ASE_OK;
}

// Smallest ASIO buffer whose duration fits the device's minimum exclusive period (render and
// capture), at the current sample rate. Queried at runtime (IAudioClient::GetDevicePeriod), never
// hard-coded: 3.0 ms -> 144 frames @ 48 kHz on the UM2 today. Returns 0 if it cannot be determined.
long BehringerUM2AsioDriver::QueryExclusiveMinBufferFrames() {
    if (!m_pRenderDevice && !m_pCaptureDevice) FindBehringerEndpoints();
    REFERENCE_TIME worst = 0;
    IMMDevice* devices[2] = { m_pRenderDevice, m_pCaptureDevice };
    for (IMMDevice* dev : devices) {
        if (!dev) continue;
        IAudioClient* probe = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&probe))) && probe) {
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            if (SUCCEEDED(probe->GetDevicePeriod(&defPeriod, &minPeriod)) && minPeriod > worst) worst = minPeriod;
            probe->Release();
        }
    }
    if (worst <= 0) return 0;
    return static_cast<long>(std::ceil(static_cast<double>(worst) * m_sampleRate / 10000000.0 - 1e-9));
}

ASIOError BehringerUM2AsioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    if (!minSize || !maxSize || !preferredSize || !granularity) return ASE_InvalidParameter;
    LoadSavedSettings();
    *maxSize = 2048;
    if (m_driverMode == DriverEngineMode::ExclusivePro) {
        // ExclusivePro: the minimum is what the device accepts in exclusive mode; sizes step by 16 from it
        const long exMin = QueryExclusiveMinBufferFrames();
        *minSize = (std::max)(64L, exMin);
        *granularity = 16;
        long pref = (std::max)(*minSize, (std::min)(2048L, m_preferredBufferSize));
        pref = *minSize + ((pref - *minSize + 8) / 16) * 16;
        *preferredSize = (std::min)(pref, 2048L);
    } else {
        *minSize = 64;          // SharedFlex: FIFO elastico sobre o periodo do engine
        *preferredSize = (std::max)(64L, (std::min)(2048L, m_preferredBufferSize));
        *granularity = -1;      // Potencias de 2
    }

    LogAsio("getBufferSize() -> mode=%s, min=%ld, max=%ld, pref=%ld, gran=%ld", 
            (m_driverMode == DriverEngineMode::SharedFlex ? "SharedFlex" : "ExclusivePro"),
            *minSize, *maxSize, *preferredSize, *granularity);
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::canSampleRate(ASIOSampleRate sampleRate) {
    LogAsio("canSampleRate(%.1f)", sampleRate);
    return (sampleRate == 48000.0 || sampleRate == 44100.0) ? ASE_OK : ASE_NoClock;
}

ASIOError BehringerUM2AsioDriver::getSampleRate(ASIOSampleRate* sampleRate) {
    if (!sampleRate) return ASE_InvalidParameter;
    *sampleRate = m_sampleRate;
    LogAsio("getSampleRate() -> %.1f", *sampleRate);
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::setSampleRate(ASIOSampleRate sampleRate) {
    LogAsio("setSampleRate(%.1f)", sampleRate);
    if (sampleRate == 48000.0 || sampleRate == 44100.0) {
        m_sampleRate = sampleRate;
        SetupAllDsp(static_cast<float>(m_sampleRate));
        return ASE_OK;
    }
    return ASE_NoClock;
}

ASIOError BehringerUM2AsioDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    if (!clocks || !numSources) return ASE_InvalidParameter;
    *numSources = 1;
    clocks[0].index = 0;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy_s(clocks[0].name, "U-Phoria TI PCM2902 Clock");
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::setClockSource(long) { return ASE_OK; }

ASIOError BehringerUM2AsioDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    if (!sPos || !tStamp) return ASE_InvalidParameter;
    sPos->hi = static_cast<unsigned long>((m_samplePosition >> 32) & 0xFFFFFFFF);
    sPos->lo = static_cast<unsigned long>(m_samplePosition & 0xFFFFFFFF);

    uint64_t nanoTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    tStamp->hi = static_cast<unsigned long>((nanoTime >> 32) & 0xFFFFFFFF);
    tStamp->lo = static_cast<unsigned long>(nanoTime & 0xFFFFFFFF);
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::getChannelInfo(ASIOChannelInfo* info) {
    if (!info) return ASE_InvalidParameter;

    int32_t hwModel = m_pSharedState ? m_pSharedState->hardwareModel : m_detectedHardwareModel;
    if (info->isInput) {
        if (info->channel >= NUM_INPUTS) return ASE_InvalidParameter;
        if (info->channel == 0) {
            if (hwModel == 1) strcpy_s(info->name, "U-Phoria MIDAS Mic 1 (XLR)");
            else strcpy_s(info->name, "U-Phoria Mic/Line 1 (XLR)");
        }
        else if (info->channel == 1) strcpy_s(info->name, "U-Phoria Inst 2 (Hi-Z)");
        else if (info->channel == 2) strcpy_s(info->name, "U-Phoria Loopback L");
        else if (info->channel == 3) strcpy_s(info->name, "U-Phoria Loopback R");
    } else {
        if (info->channel >= NUM_OUTPUTS) return ASE_InvalidParameter;
        if (info->channel == 0) strcpy_s(info->name, "U-Phoria Master Out L");
        else if (info->channel == 1) strcpy_s(info->name, "U-Phoria Master Out R");
    }

    info->type = ASIOSTInt16LSB;
    info->isActive = ASIOTrue;
    info->channelGroup = 0;
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::createBuffers(
    ASIOBufferInfo* bufferInfos, 
    long numChannels, 
    long bufferSize, 
    ASIOCallbacks* callbacks
) {
    LogAsio("createBuffers(numChannels=%ld, requestedSize=%ld)", numChannels, bufferSize);
    if (!bufferInfos || !callbacks || numChannels <= 0) return ASE_InvalidParameter;

    disposeBuffers();
    LoadSavedSettings();

    long minAllowed = 32;
    if (bufferSize < minAllowed) bufferSize = minAllowed;
    if (bufferSize > 2048) bufferSize = 2048;

    if (m_driverMode == DriverEngineMode::ExclusivePro) {
        const long exMin = QueryExclusiveMinBufferFrames();
        if (exMin > 0 && bufferSize < exMin) {
            LogAsio("createBuffers: ExclusivePro recusado - buffer %ld (%.2f ms) abaixo do minimo exclusive do device "
                    "(%ld frames). Nao ha fallback para SharedFlex.", bufferSize, 1000.0 * bufferSize / m_sampleRate, exMin);
            return ASE_InvalidMode;
        }
    }

    m_bufferSize = bufferSize;
    m_preferredBufferSize = bufferSize;
    m_callbacks = callbacks;
    m_bufferInfos.assign(bufferInfos, bufferInfos + numChannels);

    m_supportsTimeInfo = false;
    if (m_callbacks && m_callbacks->asioMessage) {
        if (m_callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1) {
            m_supportsTimeInfo = true;
            LogAsio("Host ASIO suporta bufferSwitchTimeInfo (kAsioSupportsTimeInfo = 1)");
        } else {
            LogAsio("Host ASIO opera via bufferSwitch padrao");
        }
    }

    const size_t totalChannels = NUM_INPUTS + NUM_OUTPUTS;
    m_channelMemory.resize(totalChannels);

    for (size_t c = 0; c < totalChannels; ++c) {
        m_channelMemory[c].assign(m_bufferSize * 2, 0);
    }
    m_cableIn0.assign(m_bufferSize, 0.0f);
    m_cableIn1.assign(m_bufferSize, 0.0f);
    m_loopBuf0.assign(m_bufferSize, 0.0f);
    m_loopBuf1.assign(m_bufferSize, 0.0f);

    for (long i = 0; i < numChannels; ++i) {
        size_t memIdx = bufferInfos[i].isInput ? bufferInfos[i].channelNum : (NUM_INPUTS + bufferInfos[i].channelNum);
        if (memIdx < totalChannels) {
            bufferInfos[i].buffers[0] = &m_channelMemory[memIdx][0];
            bufferInfos[i].buffers[1] = &m_channelMemory[memIdx][m_bufferSize];
        }
    }

    SaveCurrentSettings();
    FindBehringerEndpoints();
    SetupAllDsp(static_cast<float>(m_sampleRate));

    if (m_driverMode == DriverEngineMode::ExclusivePro) {
        // ====================================================================
        // MODO EXCLUSIVO / PRO (Acesso direto ao Hardware com Elastic FIFO)
        // ====================================================================
        m_isSharedModeActive = false;
        m_hwBufferSize = m_bufferSize;
        m_exclusiveRenderFrames = 0;
        m_exclusiveCaptureFrames = 0;

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = static_cast<DWORD>(m_sampleRate * 4);

        REFERENCE_TIME hwDuration = static_cast<REFERENCE_TIME>((10000000.0 * m_bufferSize / m_sampleRate) + 0.5);
        HRESULT renderHr = AUDCLNT_E_DEVICE_INVALIDATED;  // stays if there is no render device

        if (m_pRenderDevice) {
            m_pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pRenderClient));
            if (m_pRenderClient) {
                HRESULT hr = m_pRenderClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    hwDuration,
                    hwDuration,
                    &wfx,
                    nullptr
                );
                LogAsio("Render Exclusive Initialize: 0x%08X", hr);

                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    UINT32 numFrames = 0;
                    if (SUCCEEDED(m_pRenderClient->GetBufferSize(&numFrames)) && numFrames > 0) {
                        m_pRenderClient->Release();
                        m_pRenderClient = nullptr;
                        m_pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pRenderClient));
                        hwDuration = static_cast<REFERENCE_TIME>((10000000.0 * numFrames / m_sampleRate) + 0.5);
                        hr = m_pRenderClient->Initialize(
                            AUDCLNT_SHAREMODE_EXCLUSIVE,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            hwDuration,
                            hwDuration,
                            &wfx,
                            nullptr
                        );
                        LogAsio("Render Alinhado Re-Initialize: 0x%08X (numFrames=%u)", hr, numFrames);
                    }
                }

                renderHr = hr;
                if (SUCCEEDED(hr)) {
                    m_pRenderClient->GetBufferSize(&m_exclusiveRenderFrames);
                    m_hwBufferSize = m_exclusiveRenderFrames;
                    LogAsio("Render Exclusive Real Buffer Size: %u frames", m_exclusiveRenderFrames);
                    m_hRenderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    m_pRenderClient->SetEventHandle(m_hRenderEvent);
                    m_pRenderClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_pRenderService));
                }
            }
        }

        if (m_pRenderService && m_pCaptureDevice) {
            m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCaptureClient));
            if (m_pCaptureClient) {
                WAVEFORMATEX capWfx = wfx;
                capWfx.nChannels = 2;
                capWfx.nBlockAlign = 4;
                capWfx.nAvgBytesPerSec = static_cast<DWORD>(m_sampleRate * 4);

                HRESULT hr = m_pCaptureClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    hwDuration,
                    hwDuration,
                    &capWfx,
                    nullptr
                );
                LogAsio("Capture Exclusive Initialize (Stereo): 0x%08X", hr);

                if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
                    m_pCaptureClient->Release();
                    m_pCaptureClient = nullptr;
                    m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCaptureClient));
                    if (m_pCaptureClient) {
                        capWfx.nChannels = 1;
                        capWfx.nBlockAlign = 2;
                        capWfx.nAvgBytesPerSec = static_cast<DWORD>(m_sampleRate * 2);
                        hr = m_pCaptureClient->Initialize(
                            AUDCLNT_SHAREMODE_EXCLUSIVE,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            hwDuration,
                            hwDuration,
                            &capWfx,
                            nullptr
                        );
                        LogAsio("Capture Exclusive Fallback Mono Initialize: 0x%08X", hr);
                    }
                }

                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    UINT32 numFrames = 0;
                    if (SUCCEEDED(m_pCaptureClient->GetBufferSize(&numFrames)) && numFrames > 0) {
                        m_pCaptureClient->Release();
                        m_pCaptureClient = nullptr;
                        m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCaptureClient));
                        REFERENCE_TIME capHwDuration = static_cast<REFERENCE_TIME>((10000000.0 * numFrames / m_sampleRate) + 0.5);
                        hr = m_pCaptureClient->Initialize(
                            AUDCLNT_SHAREMODE_EXCLUSIVE,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            capHwDuration,
                            capHwDuration,
                            &capWfx,
                            nullptr
                        );
                        LogAsio("Capture Alinhado Re-Initialize: 0x%08X (numFrames=%u)", hr, numFrames);
                    }
                }

                if (SUCCEEDED(hr)) {
                    m_captureChannels = capWfx.nChannels;
                    m_captureIsFloat = false;
                    m_pCaptureClient->GetBufferSize(&m_exclusiveCaptureFrames);
                    LogAsio("Capture Exclusive Real Buffer Size: %u frames (channels=%u)", m_exclusiveCaptureFrames, m_captureChannels);
                    m_hCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    m_pCaptureClient->SetEventHandle(m_hCaptureEvent);
                    m_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&m_pCaptureService));
                } else {
                    m_pCaptureClient->Release();
                    m_pCaptureClient = nullptr;
                    m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCaptureClient));
                    if (m_pCaptureClient) ApplyRawStreamOption(m_pCaptureClient, "Capture Shared (fallback hibrido)");
                    if (m_pCaptureClient) {
                        WAVEFORMATEX* pCapMix = nullptr;
                        m_pCaptureClient->GetMixFormat(&pCapMix);
                        if (pCapMix) {
                            m_captureChannels = pCapMix->nChannels;
                            m_captureIsFloat = (pCapMix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                (pCapMix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pCapMix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                            hr = m_pCaptureClient->Initialize(
                                AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                0, 0,
                                pCapMix,
                                nullptr
                            );
                            CoTaskMemFree(pCapMix);
                            if (SUCCEEDED(hr)) {
                                LogAsio("Capture Exclusive em uso; fallback hibrido Capture Shared ativado (channels=%u, float=%d)",
                                        m_captureChannels, m_captureIsFloat);
                                m_hCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                                m_pCaptureClient->SetEventHandle(m_hCaptureEvent);
                                m_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&m_pCaptureService));
                            }
                        }
                    }
                }
            }
        }

        if (!m_pRenderService) {
            ASIOError err = ASE_HWMalfunction;
            if (renderHr == AUDCLNT_E_INVALID_DEVICE_PERIOD) {
                LogAsio("Render Exclusive: periodo de %.2f ms abaixo do minimo do device (AUDCLNT_E_INVALID_DEVICE_PERIOD 0x%08X). "
                        "ExclusivePro recusado, sem fallback para SharedFlex.", hwDuration / 10000.0, renderHr);
                err = ASE_InvalidMode;
            } else if (renderHr == AUDCLNT_E_DEVICE_IN_USE) {
                LogAsio("Render Exclusive: device em uso por outro processo (AUDCLNT_E_DEVICE_IN_USE 0x%08X). "
                        "ExclusivePro recusado, sem fallback para SharedFlex.", renderHr);
                err = ASE_NotPresent;
            } else {
                LogAsio("Render Exclusive falhou (hr=0x%08X). ExclusivePro recusado, sem fallback para SharedFlex.", renderHr);
            }
            if (m_hRenderEvent) { CloseHandle(m_hRenderEvent); m_hRenderEvent = nullptr; }
            if (m_hCaptureEvent) { CloseHandle(m_hCaptureEvent); m_hCaptureEvent = nullptr; }
            if (m_pCaptureService) { m_pCaptureService->Release(); m_pCaptureService = nullptr; }
            if (m_pRenderClient) { m_pRenderClient->Release(); m_pRenderClient = nullptr; }
            if (m_pCaptureClient) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
            m_channelMemory.clear();
            m_bufferInfos.clear();
            m_callbacks = nullptr;
            return err;
        }
    }

    if (m_driverMode == DriverEngineMode::SharedFlex) {
        // ====================================================================
        // MODO COMPARTILHADO / FLEX (Elastic FIFO - Ultra Estavel a 64 samples)
        // ====================================================================
        m_isSharedModeActive = true;
        m_hwBufferSize = m_bufferSize;

        // 1. Inicializa Render Shared
        if (m_pRenderDevice) {
            m_pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pRenderClient));
            if (m_pRenderClient) ApplyRawStreamOption(m_pRenderClient, "Render Shared");
            if (m_pRenderClient) {
                WAVEFORMATEX* pMixFormat = nullptr;
                m_pRenderClient->GetMixFormat(&pMixFormat);
                if (pMixFormat) {
                    LogAsio("Render Shared Format: channels=%u, rate=%lu, bits=%u, tag=0x%04X",
                            pMixFormat->nChannels, pMixFormat->nSamplesPerSec, pMixFormat->wBitsPerSample, pMixFormat->wFormatTag);
                }

                DWORD origRenderRate = pMixFormat ? pMixFormat->nSamplesPerSec : static_cast<DWORD>(m_sampleRate);
                HRESULT hr = E_FAIL;

                if (origRenderRate == static_cast<DWORD>(m_sampleRate)) {
                    IAudioClient3* pClient3 = nullptr;
                    if (SUCCEEDED(m_pRenderClient->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&pClient3))) && pClient3) {
                        UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
                        if (SUCCEEDED(pClient3->GetSharedModeEnginePeriod(pMixFormat, &defP, &fundP, &minP, &maxP))) {
                            UINT32 targetPeriod = (minP > 0) ? minP : defP;
                            hr = pClient3->InitializeSharedAudioStream(
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                targetPeriod,
                                pMixFormat,
                                nullptr
                            );
                            if (SUCCEEDED(hr)) {
                                m_enginePeriodFrames = targetPeriod;
                                LogAsio("IAudioClient3 InitializeSharedAudioStream: SUCESSO (targetPeriod=%u frames)", targetPeriod);
                            } else {
                                LogAsio("IAudioClient3 InitializeSharedAudioStream: FALHOU hr=0x%08X", hr);
                            }
                        }
                        pClient3->Release();
                    }
                } else {
                    if (pMixFormat) {
                        pMixFormat->nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
                        pMixFormat->nAvgBytesPerSec = pMixFormat->nSamplesPerSec * pMixFormat->nBlockAlign;
                        LogAsio("Ajustando taxa do Render Shared para amostragem ASIO com SRC: %lu Hz (orig=%lu Hz)", pMixFormat->nSamplesPerSec, origRenderRate);
                    }
                }

                if (FAILED(hr)) {
                    // Reativa instancia limpa de IAudioClient apos tentativa falha de IAudioClient3
                    if (m_pRenderClient) {
                        m_pRenderClient->Release();
                        m_pRenderClient = nullptr;
                    }
                    m_pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pRenderClient));
                    if (m_pRenderClient) ApplyRawStreamOption(m_pRenderClient, "Render Shared (fallback)");
                    if (m_pRenderClient) {
                        hr = m_pRenderClient->Initialize(
                            AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            0, 0,
                            pMixFormat,
                            nullptr
                        );
                        if (FAILED(hr) && pMixFormat) {
                            // Fallback restaurando taxa original caso o driver nao aceite SRC
                            pMixFormat->nSamplesPerSec = origRenderRate;
                            pMixFormat->nAvgBytesPerSec = pMixFormat->nSamplesPerSec * pMixFormat->nBlockAlign;
                            m_pRenderClient->Release();
                            m_pRenderClient = nullptr;
                            m_pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pRenderClient));
                            if (m_pRenderClient) ApplyRawStreamOption(m_pRenderClient, "Render Shared (fallback taxa original)");
                            if (m_pRenderClient) {
                                hr = m_pRenderClient->Initialize(
                                    AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    0, 0,
                                    pMixFormat,
                                    nullptr
                                );
                            }
                        }
                        REFERENCE_TIME defDuration = 0, minDuration = 0;
                        if (m_pRenderClient && SUCCEEDED(m_pRenderClient->GetDevicePeriod(&defDuration, &minDuration)) && defDuration > 0) {
                            m_enginePeriodFrames = static_cast<UINT32>((defDuration * m_sampleRate) / 10000000.0 + 0.5);
                        } else {
                            m_enginePeriodFrames = 480;
                        }
                        LogAsio("Render Shared Fallback Initialize: 0x%08X (enginePeriod=%u frames)", hr, m_enginePeriodFrames);
                    }
                }

                if (pMixFormat) CoTaskMemFree(pMixFormat);

                if (SUCCEEDED(hr)) {
                    m_hRenderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    m_pRenderClient->SetEventHandle(m_hRenderEvent);
                    m_pRenderClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_pRenderService));
                    m_pRenderClient->GetBufferSize(&m_totalRenderFrames);
                    LogAsio("Render Shared Buffer Total: %u frames", m_totalRenderFrames);
                }
            }
        }

        // 2. Inicializa Capture Shared
        if (m_pCaptureDevice) {
            m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCaptureClient));
            if (m_pCaptureClient) ApplyRawStreamOption(m_pCaptureClient, "Capture Shared");
            if (m_pCaptureClient) {
                WAVEFORMATEX* pMixFormat = nullptr;
                m_pCaptureClient->GetMixFormat(&pMixFormat);
                DWORD origCapRate = pMixFormat ? pMixFormat->nSamplesPerSec : static_cast<DWORD>(m_sampleRate);
                if (pMixFormat && pMixFormat->nSamplesPerSec != static_cast<DWORD>(m_sampleRate)) {
                    pMixFormat->nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
                    pMixFormat->nAvgBytesPerSec = pMixFormat->nSamplesPerSec * pMixFormat->nBlockAlign;
                    LogAsio("Ajustando taxa do Capture Shared para amostragem ASIO: %lu Hz", pMixFormat->nSamplesPerSec);
                }
                if (pMixFormat) {
                    m_captureChannels = pMixFormat->nChannels;
                    m_captureIsFloat = (pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                        (pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pMixFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                    LogAsio("Capture Shared Format: channels=%u, rate=%lu, float=%d", 
                            m_captureChannels, pMixFormat->nSamplesPerSec, m_captureIsFloat);
                }
                HRESULT hr = m_pCaptureClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    0, 0,
                    pMixFormat,
                    nullptr
                );
                if (FAILED(hr)) {
                    if (pMixFormat) {
                        pMixFormat->nSamplesPerSec = origCapRate;
                        pMixFormat->nAvgBytesPerSec = pMixFormat->nSamplesPerSec * pMixFormat->nBlockAlign;
                    }
                    m_pCaptureClient->Release();
                    m_pCaptureClient = nullptr;
                    m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pCaptureClient));
                    if (m_pCaptureClient) ApplyRawStreamOption(m_pCaptureClient, "Capture Shared (fallback)");
                    if (m_pCaptureClient) {
                        hr = m_pCaptureClient->Initialize(
                            AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                            0, 0,
                            pMixFormat,
                            nullptr
                        );
                    }
                }
                LogAsio("Capture Shared Initialize: 0x%08X", hr);
                if (pMixFormat) CoTaskMemFree(pMixFormat);

                if (SUCCEEDED(hr)) {
                    m_hCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    m_pCaptureClient->SetEventHandle(m_hCaptureEvent);
                    m_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&m_pCaptureService));
                }
            }
        }
    }

    m_buffersCreated = true;
    LogAsio("createBuffers completado: Mode=%s, DAW=%ld smp, Hardware=%ld smp",
            (m_isSharedModeActive ? "SharedFlex" : "ExclusivePro"), m_bufferSize, m_hwBufferSize);
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::disposeBuffers() {
    LogAsio("disposeBuffers()");
    stop();
    ReleaseEndpoints();
    m_channelMemory.clear();
    m_bufferInfos.clear();
    m_buffersCreated = false;
    return ASE_OK;
}

struct WindowSearch {
    HWND foundHwnd = nullptr;
};

static BOOL CALLBACK FindUM2ConsoleWindowProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, 512) > 0) {
        if (wcsstr(title, L"Behringer U-Phoria Console") != nullptr &&
            wcsstr(title, L"Antigravity") == nullptr &&
            wcsstr(title, L"Visual Studio") == nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProc) {
                    wchar_t exePath[MAX_PATH]{};
                    DWORD sz = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProc, 0, exePath, &sz)) {
                        _wcslwr_s(exePath, MAX_PATH);
                        if (wcsstr(exePath, L"antigravity") != nullptr ||
                            wcsstr(exePath, L"code.exe") != nullptr ||
                            wcsstr(exePath, L"devenv.exe") != nullptr) {
                            CloseHandle(hProc);
                            return TRUE;
                        }
                    }
                    CloseHandle(hProc);
                }
            }
            auto* search = reinterpret_cast<WindowSearch*>(lParam);
            search->foundHwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static bool LaunchNativeAppWindow() {
    WindowSearch ws{};
    EnumWindows(FindUM2ConsoleWindowProc, reinterpret_cast<LPARAM>(&ws));
    if (ws.foundHwnd) {
        if (IsIconic(ws.foundHwnd)) ShowWindow(ws.foundHwnd, SW_RESTORE);
        else ShowWindow(ws.foundHwnd, SW_SHOW);
        SetForegroundWindow(ws.foundHwnd);
        return true;
    }

    wchar_t localAppData[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring profileDir = std::wstring(localAppData) + L"\\KaxWav\\UM2\\AppProfile";

    const wchar_t* browsers[] = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"
    };

    for (const auto* bPath : browsers) {
        if (GetFileAttributesW(bPath) != INVALID_FILE_ATTRIBUTES) {
            std::wstring cmd = L"\"" + std::wstring(bPath) + L"\" --app=http://127.0.0.1:8930 --user-data-dir=\"" + profileDir + L"\" --window-size=1120,780 --no-first-run --no-default-browser-check";
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return true;
            }
        }
    }
    return false;
}

ASIOError BehringerUM2AsioDriver::controlPanel() {
    LogAsio("controlPanel() acionado para Behringer UM2");

    // 1. Se a janela do console ja estiver aberta, foca instantaneamente
    WindowSearch ws{};
    EnumWindows(FindUM2ConsoleWindowProc, reinterpret_cast<LPARAM>(&ws));
    if (ws.foundHwnd) {
        LogAsio("Janela Behringer U-Phoria UM2 Console encontrada. Focando...");
        if (IsIconic(ws.foundHwnd)) ShowWindow(ws.foundHwnd, SW_RESTORE);
        else ShowWindow(ws.foundHwnd, SW_SHOW);
        SetForegroundWindow(ws.foundHwnd);
        return ASE_OK;
    }

    // 2. Se o servidor backend ja estiver rodando na porta 8930, abre a janela --app instantaneamente em C++
    HANDLE hReady = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\BehringerUM2PanelServerReady");
    if (hReady != nullptr) {
        CloseHandle(hReady);
        LogAsio("Backend ja ativo em 8930. Abrindo janela dedicada diretamente...");
        if (!LaunchNativeAppWindow()) {
            ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:8930", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return ASE_OK;
    }

    bool launched = false;

    // 3. Caso o backend ainda nao esteja rodando, inicia BehringerUM2Panel.exe (que aguarda a porta 8930 e abre a janela)
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LogAsio), &hModule) && hModule) {
        wchar_t dllPath[MAX_PATH];
        if (GetModuleFileNameW(hModule, dllPath, MAX_PATH) > 0) {
            std::wstring dir = dllPath;
            size_t lastSlash = dir.find_last_of(L"\\/");
            if (lastSlash != std::wstring::npos) {
                dir = dir.substr(0, lastSlash);
            }

            std::wstring exeCandidates[] = {
                dir + L"\\BehringerUM2Panel.exe",
                dir + L"\\panel\\BehringerUM2Panel.exe",
                dir + L"\\..\\panel\\BehringerUM2Panel.exe",
                dir + L"\\..\\..\\panel\\BehringerUM2Panel.exe",
                dir + L"\\..\\..\\release_package\\BehringerUM2Panel.exe"
            };

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            for (const auto& exePath : exeCandidates) {
                if (GetFileAttributesW(exePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    std::wstring cmd = L"\"" + exePath + L"\"";
                    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                    cmdBuf.push_back(L'\0');
                    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                        launched = true;
                        break;
                    }
                }
            }

            if (!launched) {
                std::wstring pyCandidates[] = {
                    dir + L"\\panel\\panel_backend.py",
                    dir + L"\\..\\panel\\panel_backend.py",
                    dir + L"\\..\\..\\panel\\panel_backend.py"
                };
                for (const auto& pyPath : pyCandidates) {
                    if (GetFileAttributesW(pyPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        std::wstring pyDir = pyPath.substr(0, pyPath.find_last_of(L"\\/"));
                        std::wstring cmd = L"pythonw.exe \"" + pyPath + L"\"";
                        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                        cmdBuf.push_back(L'\0');
                        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, pyDir.c_str(), &si, &pi)) {
                            CloseHandle(pi.hThread);
                            CloseHandle(pi.hProcess);
                            launched = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!launched) {
        if (!LaunchNativeAppWindow()) {
            ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:8930", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::future(long selector, void*) {
    LogAsio("future(selector=%ld)", selector);
    switch (selector) {
        case kAsioEnableTimeCodeRead:
        case kAsioDisableTimeCodeRead:
        case kAsioSetInputMonitor:
        case kAsioCanInputMonitor:
        case kAsioCanTimeInfo:
        case kAsioCanTimeCode:
            return ASE_SUCCESS;
        default:
            return ASE_NotPresent;
    }
}

ASIOError BehringerUM2AsioDriver::outputReady() { return ASE_OK; }

ASIOError BehringerUM2AsioDriver::start() {
    LogAsio("start()");
    if (m_active) return ASE_OK;
    if (!m_buffersCreated) return ASE_NotPresent;

    m_stopThread = false;
    m_samplePosition = 0;

    UpdateLatencyTelemetry();
    if (m_pSharedState) {
        m_pSharedState->isStreaming = 1;
        m_pSharedState->driverStatus = 2;
        m_pSharedState->sampleRate = static_cast<uint32_t>(m_sampleRate);
        m_pSharedState->bufferSize = static_cast<uint32_t>(m_bufferSize);
        m_pSharedState->inputChannels = NUM_INPUTS;
        m_pSharedState->outputChannels = NUM_OUTPUTS;
        m_pSharedState->driverMode = static_cast<uint32_t>(m_driverMode);
        m_processLoopback.SyncConfiguration(
            m_pSharedState->loopbackMode,
            m_pSharedState->loopbackTargetPid,
            static_cast<uint32_t>(m_sampleRate)
        );
    }

    InitCableEndpoint();
    OpenTelemetry();
    // The audio thread starts the WASAPI streams itself, already under MMCSS, and enters its wait loop right
    // away: started from here (host thread, normal priority) the exclusive render could run dry before the
    // first event was serviced and stay with a single packet in flight (render trap, FATOS 2026-11-01).
    if (!m_hStreamsStarted) m_hStreamsStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(m_hStreamsStarted);
    m_streamStartHr.store(E_PENDING);
    m_audioThread = std::thread(&BehringerUM2AsioDriver::AudioThreadLoop, this);
    const DWORD wr = m_hStreamsStarted ? WaitForSingleObject(m_hStreamsStarted, 2000) : WAIT_FAILED;
    const HRESULT shr = m_streamStartHr.load();
    if (wr != WAIT_OBJECT_0 || FAILED(shr)) {
        LogAsio("ERRO: a thread de audio nao iniciou os streams (wait=%lu hr=0x%08X)", wr, static_cast<unsigned>(shr));
        m_stopThread = true;
        if (m_hRenderEvent) SetEvent(m_hRenderEvent);
        if (m_hCaptureEvent) SetEvent(m_hCaptureEvent);
        if (m_audioThread.joinable()) m_audioThread.join();
        if (m_pRenderClient) m_pRenderClient->Stop();
        if (m_pCaptureClient) m_pCaptureClient->Stop();
        return ASE_HWMalfunction;
    }
    if (!m_isSharedModeActive && m_exclusiveRenderFrames > 0)
        LogAsio("Streams iniciados pela thread de audio (pre-roll exclusive de %u frames)", m_exclusiveRenderFrames);
    m_active = true;
    LogAsio("start() executado com SUCESSO");
    return ASE_OK;
}

ASIOError BehringerUM2AsioDriver::stop() {
    LogAsio("stop()");
    if (!m_active) return ASE_OK;

    m_stopThread = true;
    if (m_hRenderEvent) SetEvent(m_hRenderEvent);
    if (m_hCaptureEvent) SetEvent(m_hCaptureEvent);

    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }

    m_processLoopback.Shutdown();
    ReleaseCableEndpoint();
    if (m_pRenderClient) m_pRenderClient->Stop();
    if (m_pCaptureClient) m_pCaptureClient->Stop();

    if (m_pSharedState) {
        m_pSharedState->isStreaming = 0;
        m_pSharedState->driverStatus = 1;
        m_pSharedState->peakIn1 = 0.0f;
        m_pSharedState->peakIn2 = 0.0f;
        m_pSharedState->peakLoopbackL = 0.0f;
        m_pSharedState->peakLoopbackR = 0.0f;
        m_pSharedState->peakOut1 = 0.0f;
        m_pSharedState->peakOut2 = 0.0f;
        m_pSharedState->ch1GainReductionDb = 0.0f;
        m_pSharedState->ch2GainReductionDb = 0.0f;
        m_pSharedState->dspCpuLoadPercent = 0.0f;
    }

    m_active = false;
    LogAsio("stop() finalizado");
    return ASE_OK;
}

void BehringerUM2AsioDriver::AudioThreadLoop() {
    HRESULT hrCoInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hMmcss) {
        // Invariant 5: MMCSS + AvSetMmThreadPriority(CRITICAL), nothing else. A SetThreadPriority(TIME_CRITICAL)
        // here REPLACES the MMCSS boost: measured 15 instead of 24 (MMCSS only) / 26 (CRITICAL) on 2026-11-01.
        if (!AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL))
            LogAsio("AVISO: AvSetMmThreadPriority(CRITICAL) falhou (%lu) - segue na prioridade padrao do MMCSS", GetLastError());
        LogAsio("MMCSS \"Pro Audio\" OK (taskIndex=%lu, tid=%lu) + AvSetMmThreadPriority(CRITICAL)", taskIndex, GetCurrentThreadId());
    } else {
        // No raw-priority fallback: report and run at the default priority.
        LogAsio("ERRO: AvSetMmThreadCharacteristicsW(\"Pro Audio\") falhou (%lu) - thread de audio SEM MMCSS (prioridade padrao)",
                GetLastError());
    }

    uint32_t currentBufferHalf = 0;

    const size_t outLeftIdx   = NUM_INPUTS + 0;
    const size_t outRightIdx  = NUM_INPUTS + 1;
    const size_t loopLeftIdx  = 2;
    const size_t loopRightIdx = 3;

    constexpr size_t FIFO_CAP = 16384;
    std::vector<float> renFifo(FIFO_CAP * 2, 0.0f);
    size_t renHead = 0, renTail = 0;

    std::vector<float> capFifo(FIFO_CAP * 2, 0.0f);
    size_t capHead = 0, capTail = 0;

    auto renFifoAvail = [&]() -> size_t {
        return (renHead >= renTail) ? (renHead - renTail) : (FIFO_CAP - renTail + renHead);
    };
    auto capFifoAvail = [&]() -> size_t {
        return (capHead >= capTail) ? (capHead - capTail) : (FIFO_CAP - capTail + capHead);
    };

    float lastCap0 = 0.0f, lastCap1 = 0.0f;
    float smoothedCpuPercent = 0.0f;

    auto handleDeviceInvalidated = [&]() {
        LogAsio("ALERTA CRÍTICO: Dispositivo USB desconectado (AUDCLNT_E_DEVICE_INVALIDATED)!");
        m_deviceInvalidated.store(true);
        if (m_pSharedState) {
            m_pSharedState->isPhysicalHardware = 0;
            m_pSharedState->isStreaming = 0;
        }
        if (m_callbacks && m_callbacks->asioMessage) {
            LogAsio("Disparando asioMessage(kAsioResetRequest) para hot-reconnect na DAW...");
            m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
    };

    // Starts capture, then render with one period of silence pre-rolled (exclusive), on this thread, and
    // releases start() through the handshake. Not logged here: the first event must find this thread waiting.
    auto startStreams = [&]() -> bool {
        HRESULT hr = S_OK;
        if (m_pCaptureClient) hr = m_pCaptureClient->Start();
        if (SUCCEEDED(hr) && m_pRenderClient) {
            if (!m_isSharedModeActive && m_pRenderService && m_exclusiveRenderFrames > 0) {
                BYTE* pData = nullptr;
                if (SUCCEEDED(m_pRenderService->GetBuffer(m_exclusiveRenderFrames, &pData)) && pData)
                    m_pRenderService->ReleaseBuffer(m_exclusiveRenderFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            hr = m_pRenderClient->Start();
        }
        m_streamStartHr.store(hr);
        if (m_hStreamsStarted) SetEvent(m_hStreamsStarted);
        return SUCCEEDED(hr);
    };

    auto pushCapturePacket = [&](BYTE* pCapData, UINT32 numFrames, DWORD flags, bool isDriverMuted) {
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            if (m_pSharedState) m_pSharedState->xruns++;
        }
        if (!pCapData || numFrames == 0) return;

        if (isDriverMuted || (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            for (UINT32 i = 0; i < numFrames; ++i) {
                capFifo[capHead * 2 + 0] = 0.0f;
                capFifo[capHead * 2 + 1] = 0.0f;
                capHead = (capHead + 1) % FIFO_CAP;
                if (capHead == capTail) capTail = (capTail + 1) % FIFO_CAP;
            }
        } else if (m_captureChannels == 1) {
            const auto* pFloats = reinterpret_cast<const float*>(pCapData);
            const auto* pInts = reinterpret_cast<const int16_t*>(pCapData);
            for (UINT32 i = 0; i < numFrames; ++i) {
                float sample = m_captureIsFloat ? pFloats[i] : (pInts[i] / 32768.0f);
                capFifo[capHead * 2 + 0] = sample;
                capFifo[capHead * 2 + 1] = 0.0f;
                capHead = (capHead + 1) % FIFO_CAP;
                if (capHead == capTail) capTail = (capTail + 1) % FIFO_CAP;
            }
        } else {
            const auto* pFloats = reinterpret_cast<const float*>(pCapData);
            const auto* pInts = reinterpret_cast<const int16_t*>(pCapData);
            for (UINT32 i = 0; i < numFrames; ++i) {
                float s0 = m_captureIsFloat ? pFloats[i * 2 + 0] : (pInts[i * 2 + 0] / 32768.0f);
                float s1 = m_captureIsFloat ? pFloats[i * 2 + 1] : (pInts[i * 2 + 1] / 32768.0f);
                capFifo[capHead * 2 + 0] = s0;
                capFifo[capHead * 2 + 1] = s1;
                capHead = (capHead + 1) % FIFO_CAP;
                if (capHead == capTail) capTail = (capTail + 1) % FIFO_CAP;
            }
        }
    };

    auto processOneAsioBlock = [&](bool isDriverMuted) {
        LARGE_INTEGER qpcStart{};
        QueryPerformanceCounter(&qpcStart);

        volatile UM2SharedState* pState = m_pSharedState;
        const bool dawDsp0  = m_dawDspCh1.load(std::memory_order_relaxed);
        const bool dawDsp1  = m_dawDspCh2.load(std::memory_order_relaxed);
        bool ch1PhaseInv    = (pState && pState->ch1Phase != 0);
        bool ch1HpfOn       = (pState && pState->ch1Hpf != 0);
        bool ch1GateOn      = (pState && pState->ch1Gate != 0);
        bool ch1Muted       = (pState && pState->ch1Mute != 0);
        float ch1Trim       = pState ? pState->ch1Trim : 1.0f;
        uint32_t ch1Preamp  = pState ? pState->ch1PreampMode : 0;
        float ch1CompAmt    = pState ? pState->ch1CompAmount : 0.0f;
        float ch1DeessAmt   = pState ? pState->ch1DeesserAmount : 0.0f;

        bool ch2PhaseInv    = (pState && pState->ch2Phase != 0);
        bool ch2PadOn       = (pState && pState->ch2Pad != 0);
        bool ch2GateOn      = (pState && pState->ch2Gate != 0);
        bool ch2Muted       = (pState && pState->ch2Mute != 0);
        float ch2Trim       = pState ? pState->ch2Trim : 1.0f;
        uint32_t ch2Preamp  = pState ? pState->ch2PreampMode : 0;
        float ch2CompAmt    = pState ? pState->ch2CompAmount : 0.0f;
        bool ch2CabSimOn    = (pState && pState->ch2CabSim != 0);

        float gateThresh    = pState ? pState->gateThresholdDb : -58.0f;
        if (gateThresh > -53.0f && gateThresh < -51.0f) {
            gateThresh = -58.0f; // Migra automaticamente o limiar antigo (-52dB) para -58dB mais sensivel
        }
        bool dspMonOnly     = (pState && pState->dspMonitorOnly != 0);

        bool loopEnabled    = (pState ? (pState->loopbackEnabled != 0) : true);
        bool loopMono       = (pState && pState->loopbackMono != 0);
        bool loopBroadcast  = (pState && pState->loopbackBroadcastMix != 0);
        float loopGain      = (pState ? pState->loopbackGain : 1.0f);
        uint32_t loopMode   = (pState ? pState->loopbackMode : 0);

        bool masterDim      = (pState && pState->masterDim != 0);
        bool masterMono     = (pState && pState->masterMono != 0);
        bool masterMuted    = (pState && pState->masterMute != 0);
        float masterTrim    = pState ? pState->masterTrim : 1.0f;
        bool monSubCheck    = (pState && pState->monSubCheck != 0);
        bool monPhoneSim    = (pState && pState->monPhoneSim != 0);
        bool monCrossfeed   = (pState && pState->monCrossfeed != 0);

        uint32_t tunerSrc   = pState ? pState->tunerSource : 0;

        if (capFifoAvail() < static_cast<size_t>(m_bufferSize) && m_pCaptureService) {
            BYTE* pOppCap = nullptr;
            UINT32 oppFrames = 0;
            DWORD oppFlags = 0;
            while (SUCCEEDED(m_pCaptureService->GetBuffer(&pOppCap, &oppFrames, &oppFlags, nullptr, nullptr)) && oppFrames > 0) {
                pushCapturePacket(pOppCap, oppFrames, oppFlags, isDriverMuted);
                m_pCaptureService->ReleaseBuffer(oppFrames);
            }
        }

        int16_t* pIn0 = &m_channelMemory[0][currentBufferHalf * m_bufferSize];
        int16_t* pIn1 = &m_channelMemory[1][currentBufferHalf * m_bufferSize];

        for (long s = 0; s < m_bufferSize; ++s) {
            float in0Val = 0.0f, in1Val = 0.0f;
            if (capHead != capTail) {
                in0Val = capFifo[capTail * 2 + 0];
                in1Val = capFifo[capTail * 2 + 1];
                capTail = (capTail + 1) % FIFO_CAP;
                lastCap0 = in0Val;
                lastCap1 = in1Val;
            } else {
                lastCap0 *= 0.95f;
                lastCap1 *= 0.95f;
                in0Val = lastCap0;
                in1Val = lastCap1;
                if (pState) const_cast<UM2SharedState*>(pState)->xruns++;
            }

            // Detector de Clip Analógico Pré-Trim (ADC 16-bit da Behringer)
            m_clipCh1.CheckSample(in0Val);
            m_clipCh2.CheckSample(in1Val);

            // Estágio Base (Dry Utility: HPF, Gate, Fase, Trim, Mute)
            float dry0 = m_hpfCh1.Process(in0Val, ch1HpfOn);
            float effGateThresh1 = ch1HpfOn ? (gateThresh - 3.0f) : gateThresh;
            dry0 = m_gateCh1.Process(dry0, ch1GateOn, effGateThresh1);
            if (ch1PhaseInv) dry0 = -dry0;
            dry0 *= ch1Trim;
            if (ch1Muted || isDriverMuted) dry0 = 0.0f;

            float dry1 = m_gateCh2.Process(in1Val, ch2GateOn, gateThresh);
            if (ch2PhaseInv) dry1 = -dry1;
            if (ch2PadOn) dry1 *= 0.31622776f; // PAD -10dB
            dry1 *= ch2Trim;
            if (ch2Muted || isDriverMuted) dry1 = 0.0f;

            // Estágio Color/Dynamics (Unison Preamp 73/Tube + De-Esser + FET 76 Compressor + Cab 4x12)
            float wet0 = m_preampCh1.Process(dry0, ch1Preamp);
            wet0 = m_deesserCh1.Process(wet0, ch1DeessAmt);
            wet0 = m_compCh1.Process(wet0, ch1CompAmt);

            float wet1 = m_preampCh2.Process(dry1, ch2Preamp);
            wet1 = m_compCh2.Process(wet1, ch2CompAmt);
            wet1 = m_cabCh2.Process(wet1, ch2CabSimOn);

            // VB-Cable / bridge, monitor (MON ONLY) e medidores recebem sempre o sinal colorido (wet)
            m_cableIn0[s] = wet0;
            m_cableIn1[s] = wet1;

            // ADR 0004: por padrao a DAW recebe a captura bit-perfect (so fase/PAD/trim/mute do usuario,
            // sem HPF, gate, preamp, de-esser, compressor, cab sim nem dither). O DSP acima continua
            // rodando para a bridge/cabo virtual, monitor e medidores; vai para a DAW so se o canal tiver
            // "DSP na DAW" ligado (DawDspCh1/DawDspCh2).
            if (dawDsp0) {
                pIn0[s] = m_dsp.Quantize16(wet0);
            } else {
                float raw0 = ch1PhaseInv ? -in0Val : in0Val;
                raw0 *= ch1Trim;
                if (ch1Muted || isDriverMuted) raw0 = 0.0f;
                pIn0[s] = ExactToInt16(raw0);
            }
            if (dawDsp1) {
                pIn1[s] = m_dsp.Quantize16(wet1);
            } else {
                float raw1 = ch2PhaseInv ? -in1Val : in1Val;
                if (ch2PadOn) raw1 *= 0.31622776f;
                raw1 *= ch2Trim;
                if (ch2Muted || isDriverMuted) raw1 = 0.0f;
                pIn1[s] = ExactToInt16(raw1);
            }
        }

        // Loopback Virtual (IN 3-4): Per-App Loopback (Isolate/Exclude PID) ou Mix Interno da DAW
        const uint32_t prevBufferHalf = 1 - currentBufferHalf;
        const int16_t* pPrevOut0 = &m_channelMemory[outLeftIdx][prevBufferHalf * m_bufferSize];
        const int16_t* pPrevOut1 = &m_channelMemory[outRightIdx][prevBufferHalf * m_bufferSize];
        int16_t* pLoop0 = &m_channelMemory[loopLeftIdx][currentBufferHalf * m_bufferSize];
        int16_t* pLoop1 = &m_channelMemory[loopRightIdx][currentBufferHalf * m_bufferSize];

        bool useProcessLoopback = (loopMode == 1 || loopMode == 2) && m_processLoopback.IsActive();
        if (useProcessLoopback) {
            m_processLoopback.DrainAndReadStereo(
                m_loopBuf0.data(),
                m_loopBuf1.data(),
                static_cast<size_t>(m_bufferSize)
            );
        }

        if (loopEnabled) {
            for (long s = 0; s < m_bufferSize; ++s) {
                float s0 = 0.0f;
                float s1 = 0.0f;
                if (useProcessLoopback) {
                    // Sampler Loopback por Processo: captura isolada sem feedback da própria DAW!
                    s0 = m_loopBuf0[s] * loopGain;
                    s1 = m_loopBuf1[s] * loopGain;
                } else {
                    s0 = (static_cast<float>(pPrevOut0[s]) / 32768.0f) * loopGain;
                    s1 = (static_cast<float>(pPrevOut1[s]) / 32768.0f) * loopGain;
                }
                if (loopMono) {
                    float monoSum = (s0 + s1) * 0.5f;
                    s0 = monoSum;
                    s1 = monoSum;
                }
                if (loopBroadcast) {
                    float inMix = (m_cableIn0[s] + m_cableIn1[s]) * 0.7071f;
                    s0 += inMix;
                    s1 += inMix;
                }
                pLoop0[s] = m_dsp.Quantize16(s0);
                pLoop1[s] = m_dsp.Quantize16(s1);
            }
        } else {
            std::memset(pLoop0, 0, m_bufferSize * sizeof(int16_t));
            std::memset(pLoop1, 0, m_bufferSize * sizeof(int16_t));
        }

        // Alimenta Afinador Cromático + Detector de 808 (YIN/Mcleod Zero-Allocation)
        // Mesmo com DAW SEND desligado, lê a fonte bruta de loopback quando tunerSrc == 3
        if (tunerSrc != 0) {
            for (long s = 0; s < m_bufferSize; ++s) {
                float tSample = 0.0f;
                if (tunerSrc == 1) tSample = m_cableIn0[s];
                else if (tunerSrc == 2) tSample = m_cableIn1[s];
                else if (tunerSrc == 3) {
                    if (loopEnabled) {
                        tSample = pLoop0[s] / 32768.0f;
                    } else if (useProcessLoopback) {
                        tSample = m_loopBuf0[s] * loopGain;
                    } else {
                        tSample = (static_cast<float>(pPrevOut0[s]) / 32768.0f) * loopGain;
                    }
                }
                m_tuner.PushSample(tSample);
            }
        }

        LARGE_INTEGER qpcBeforeDaw{};
        QueryPerformanceCounter(&qpcBeforeDaw);

        // Dispara Callback ASIO para a DAW com suporte a ASIOTime (bufferSwitchTimeInfo)
        if (m_supportsTimeInfo && m_callbacks && m_callbacks->bufferSwitchTimeInfo) {
            ASIOTime asioTime{};
            uint64_t curNs = (m_qpcFreq.QuadPart > 0)
                ? static_cast<uint64_t>((static_cast<double>(qpcBeforeDaw.QuadPart) * 1e9) / static_cast<double>(m_qpcFreq.QuadPart))
                : 0ULL;

            asioTime.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            asioTime.timeInfo.speed = 1.0;
            asioTime.timeInfo.sampleRate = m_sampleRate;
            asioTime.timeInfo.samplePosition.hi = static_cast<unsigned long>((m_samplePosition >> 32) & 0xFFFFFFFF);
            asioTime.timeInfo.samplePosition.lo = static_cast<unsigned long>(m_samplePosition & 0xFFFFFFFF);
            asioTime.timeInfo.systemTime.hi = static_cast<unsigned long>((curNs >> 32) & 0xFFFFFFFF);
            asioTime.timeInfo.systemTime.lo = static_cast<unsigned long>(curNs & 0xFFFFFFFF);

            m_callbacks->bufferSwitchTimeInfo(&asioTime, currentBufferHalf, ASIOTrue);
        } else if (m_callbacks && m_callbacks->bufferSwitch) {
            m_callbacks->bufferSwitch(currentBufferHalf, ASIOTrue);
        }

        LARGE_INTEGER qpcAfterDaw{};
        QueryPerformanceCounter(&qpcAfterDaw);

        const int16_t* pOut0 = &m_channelMemory[outLeftIdx][currentBufferHalf * m_bufferSize];
        const int16_t* pOut1 = &m_channelMemory[outRightIdx][currentBufferHalf * m_bufferSize];
        FeedCableVirtual(m_bufferSize, (loopEnabled && loopBroadcast), loopGain, gateThresh, m_cableIn0.data(), m_cableIn1.data(), pOut0, pOut1);

        // Sistema Anti-Pirataria / Restricao de Modo Demo
        float demoMultiplier = 1.0f;
        if (!m_isLicensed) {
            m_demoSamplesCount += static_cast<uint64_t>(m_bufferSize);
            uint64_t cycleSamples = static_cast<uint64_t>(m_sampleRate) * 120ULL; // 120s
            uint64_t silenceDuration = static_cast<uint64_t>(m_sampleRate) * 2ULL; // 2s de pausa a cada 2min
            uint64_t pos = m_demoSamplesCount % cycleSamples;
            if (pos >= (cycleSamples - silenceDuration)) {
                uint64_t restrictedPos = pos - (cycleSamples - silenceDuration);
                uint64_t rampSamples = static_cast<uint64_t>(m_sampleRate / 10); // 100ms fade
                if (restrictedPos < rampSamples) {
                    demoMultiplier = 1.0f - (static_cast<float>(restrictedPos) / static_cast<float>(rampSamples));
                } else if (restrictedPos > (silenceDuration - rampSamples)) {
                    uint64_t inPos = restrictedPos - (silenceDuration - rampSamples);
                    demoMultiplier = static_cast<float>(inPos) / static_cast<float>(rampSamples);
                } else {
                    demoMultiplier = 0.0f;
                }
            }
        }

        // DSP Master Out (DIM, Mono, Sub Check, Phone Sim, Crossfeed, Trim, Mute) + Soft-Clipper Musical
        float maxOut1 = 0.0f, maxOut2 = 0.0f;
        float postOutMono[2048]{};
        for (long s = 0; s < m_bufferSize; ++s) {
            float out0Val = (pOut0[s] / 32768.0f);
            float out1Val = (pOut1[s] / 32768.0f);

            // Se MON ONLY estiver ativo, soma o sinal Wet (preamp/comp/de-esser/cab) direto nos fones
            // com latência zero para monitorar a voz processada em tempo real
            if (dspMonOnly) {
                float wetMon = m_cableIn0[s] + (ch2Muted ? 0.0f : m_cableIn1[s] * 0.7071f);
                out0Val += wetMon;
                out1Val += wetMon;
            }

            out0Val *= masterTrim * demoMultiplier;
            out1Val *= masterTrim * demoMultiplier;

            if (masterMono) {
                float monoMix = (out0Val + out1Val) * 0.5f;
                out0Val = monoMix;
                out1Val = monoMix;
            }

            m_masterMon.ProcessStereo(out0Val, out1Val, monSubCheck, monPhoneSim, monCrossfeed);

            if (masterDim) {
                out0Val *= 0.19952623f; // DIM -14dB
                out1Val *= 0.19952623f;
            }
            if (masterMuted) {
                out0Val = 0.0f;
                out1Val = 0.0f;
            }

            out0Val = StudioDspProcessor::SoftClip(out0Val);
            out1Val = StudioDspProcessor::SoftClip(out1Val);

            if (s < 2048) {
                postOutMono[s] = (out0Val + out1Val) * 0.5f;
            }

            maxOut1 = (std::max)(maxOut1, std::abs(out0Val));
            maxOut2 = (std::max)(maxOut2, std::abs(out1Val));

            renFifo[renHead * 2 + 0] = out0Val;
            renFifo[renHead * 2 + 1] = out1Val;
            renHead = (renHead + 1) % FIFO_CAP;
            if (renHead == renTail) {
                renTail = (renTail + 1) % FIFO_CAP;
                if (m_pSharedState) m_pSharedState->xruns++;
            }
        }

        LARGE_INTEGER qpcEnd{};
        QueryPerformanceCounter(&qpcEnd);
        if (m_pTelemetry) {
            UM2RecordBlockTiming(m_pTelemetry, qpcStart.QuadPart, qpcBeforeDaw.QuadPart, qpcAfterDaw.QuadPart, qpcEnd.QuadPart);
        }

        // Telemetria de Picos, GR, Clip, Tuner, CPU % & Osciloscópio Otimizado para Cache
        if (m_pSharedState) {
            float maxIn1 = 0.0f, maxIn2 = 0.0f;
            float maxLoopL = 0.0f, maxLoopR = 0.0f;
            for (long s = 0; s < m_bufferSize; ++s) {
                maxIn1 = (std::max)(maxIn1, std::abs(m_cableIn0[s]));
                maxIn2 = (std::max)(maxIn2, std::abs(m_cableIn1[s]));
                maxLoopL = (std::max)(maxLoopL, std::abs(pLoop0[s]) / 32768.0f);
                maxLoopR = (std::max)(maxLoopR, std::abs(pLoop1[s]) / 32768.0f);
            }

            m_pSharedState->peakIn1 = (std::max)(maxIn1, m_pSharedState->peakIn1 * 0.92f);
            m_pSharedState->peakIn2 = (std::max)(maxIn2, m_pSharedState->peakIn2 * 0.92f);
            m_pSharedState->peakOut1 = (std::max)(maxOut1, m_pSharedState->peakOut1 * 0.92f);
            m_pSharedState->peakOut2 = (std::max)(maxOut2, m_pSharedState->peakOut2 * 0.92f);
            m_pSharedState->peakLoopbackL = (std::max)(maxLoopL, m_pSharedState->peakLoopbackL * 0.92f);
            m_pSharedState->peakLoopbackR = (std::max)(maxLoopR, m_pSharedState->peakLoopbackR * 0.92f);

            m_pSharedState->ch1HwClip = m_clipCh1.IsClipping() ? 1u : 0u;
            m_pSharedState->ch2HwClip = m_clipCh2.IsClipping() ? 1u : 0u;
            m_pSharedState->ch1GainReductionDb = m_compCh1.GetGainReductionDb() + m_deesserCh1.GetGainReductionDb();
            m_pSharedState->ch2GainReductionDb = m_compCh2.GetGainReductionDb();

            if (tunerSrc != 0) {
                m_pSharedState->tunerFreqHz = m_tuner.GetFrequencyHz();
                m_pSharedState->tunerMidiNote = m_tuner.GetMidiNote();
                m_pSharedState->tunerCents = m_tuner.GetCents();
            } else {
                m_pSharedState->tunerFreqHz = 0.0f;
                m_pSharedState->tunerMidiNote = 0;
                m_pSharedState->tunerCents = 0.0f;
            }

            if (m_qpcFreq.QuadPart > 0 && m_sampleRate > 0.0) {
                int64_t dspTicks = (qpcBeforeDaw.QuadPart - qpcStart.QuadPart) + (qpcEnd.QuadPart - qpcAfterDaw.QuadPart);
                double dspSec = static_cast<double>((std::max)(int64_t(0), dspTicks)) / static_cast<double>(m_qpcFreq.QuadPart);
                double blockBudgetSec = static_cast<double>(m_bufferSize) / m_sampleRate;
                float blockCpuPct = (blockBudgetSec > 0.0)
                    ? static_cast<float>((dspSec / blockBudgetSec) * 100.0)
                    : 0.0f;
                smoothedCpuPercent = smoothedCpuPercent * 0.90f + blockCpuPct * 0.10f;
                m_pSharedState->dspCpuLoadPercent = (std::min)(99.9f, smoothedCpuPercent);
            }

            m_scopeSubsample++;
            uint32_t scopeDivisor = (std::max)(1u, static_cast<uint32_t>(m_sampleRate / (m_bufferSize * 120L)));
            if (m_scopeSubsample % scopeDivisor == 0) {
                uint32_t src = m_pSharedState->scopeSource;
                long step = (std::max)(1L, m_bufferSize / 128L);
                bool hasMasterSignal = (maxOut1 + maxOut2) > 0.0005f;
                for (int i = 0; i < 128; ++i) {
                    long idx = (i * step) % m_bufferSize;
                    float val = 0.0f;
                    if (src == 1) val = m_cableIn0[idx];
                    else if (src == 2) val = m_cableIn1[idx];
                    else if (src == 3) val = pLoop0[idx] / 32768.0f;
                    else {
                        val = (hasMasterSignal && idx < 2048) ? postOutMono[idx] : m_cableIn0[idx];
                    }
                    m_pSharedState->scopeBuffer[i] = val;
                }
            }
            m_pSharedState->bufferSwitchCount++;
        }

        m_samplePosition += m_bufferSize;
        currentBufferHalf = 1 - currentBufferHalf;
    };

    if (m_isSharedModeActive) {
        // ====================================================================
        // ENGINE COMPARTILHADA / FLEX (Elastic FIFO - Zero Picotes a 64 Samples)
        // ====================================================================
        const size_t initialRenCushion = static_cast<size_t>((std::min)(
            m_totalRenderFrames > 0 ? m_totalRenderFrames : 1056u,
            m_enginePeriodFrames + static_cast<UINT32>(m_bufferSize * 2L)
        ));
        for (size_t i = 0; i < initialRenCushion; ++i) {
            renFifo[renHead * 2 + 0] = 0.0f;
            renFifo[renHead * 2 + 1] = 0.0f;
            renHead = (renHead + 1) % FIFO_CAP;
        }

        const size_t capCushion = static_cast<size_t>(m_enginePeriodFrames * 2 + m_bufferSize * 2);
        for (size_t i = 0; i < capCushion; ++i) {
            capFifo[capHead * 2 + 0] = 0.0f;
            capFifo[capHead * 2 + 1] = 0.0f;
            capHead = (capHead + 1) % FIFO_CAP;
        }

        if (startStreams()) while (!m_stopThread.load(std::memory_order_relaxed)) {
            bool renderReady = false;
            bool captureReady = false;

            if (m_hRenderEvent && m_hCaptureEvent) {
                HANDLE events[2] = { m_hRenderEvent, m_hCaptureEvent };
                DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, 20);
                if (waitRes == WAIT_OBJECT_0) {
                    renderReady = true;
                    if (WaitForSingleObject(m_hCaptureEvent, 0) == WAIT_OBJECT_0) captureReady = true;
                } else if (waitRes == WAIT_OBJECT_0 + 1) {
                    captureReady = true;
                    if (WaitForSingleObject(m_hRenderEvent, 0) == WAIT_OBJECT_0) renderReady = true;
                } else if (waitRes == WAIT_TIMEOUT) {
                    renderReady = true;
                    captureReady = true;
                }
            } else if (m_hRenderEvent) {
                DWORD waitRes = WaitForSingleObject(m_hRenderEvent, 20);
                renderReady = (waitRes == WAIT_OBJECT_0 || waitRes == WAIT_TIMEOUT);
                captureReady = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                renderReady = true;
                captureReady = true;
            }

            bool isDriverMuted = (m_pSharedState && m_pSharedState->softwareMute != 0);

            // 1. Drena todos os pacotes de entrada se captura foi sinalizada ou se render pronto
            if ((captureReady || renderReady) && m_pCaptureService) {
                BYTE* pCapData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                HRESULT hrCap = S_OK;
                while (SUCCEEDED(hrCap = m_pCaptureService->GetBuffer(&pCapData, &numFrames, &flags, nullptr, nullptr)) && numFrames > 0) {
                    pushCapturePacket(pCapData, numFrames, flags, isDriverMuted);
                    m_pCaptureService->ReleaseBuffer(numFrames);
                }

                if (hrCap == AUDCLNT_E_DEVICE_INVALIDATED) {
                    handleDeviceInvalidated();
                    break;
                }
            }

            // 2. Preenche buffer de renderizacao com Watermark de Baixa Latencia (~4ms)
            if (m_pRenderClient && m_pRenderService) {
                UINT32 padding = 0;
                HRESULT hrPad = m_pRenderClient->GetCurrentPadding(&padding);
                if (hrPad == AUDCLNT_E_DEVICE_INVALIDATED) {
                    handleDeviceInvalidated();
                    break;
                }

                if (SUCCEEDED(hrPad)) {
                    UINT32 targetPadding = (std::min)(m_totalRenderFrames, m_enginePeriodFrames + static_cast<UINT32>(m_bufferSize * 2L));
                    UINT32 needed = (targetPadding > padding) ? (targetPadding - padding) : 0;

                    while (renFifoAvail() < needed && !m_stopThread.load(std::memory_order_relaxed)) {
                        processOneAsioBlock(isDriverMuted);
                    }

                    if (needed > 0 && renFifoAvail() >= needed) {
                        BYTE* pRenData = nullptr;
                        HRESULT hrRen = m_pRenderService->GetBuffer(needed, &pRenData);
                        if (hrRen == AUDCLNT_E_DEVICE_INVALIDATED) {
                            handleDeviceInvalidated();
                            break;
                        }
                        if (SUCCEEDED(hrRen) && pRenData) {
                            auto* pFloats = reinterpret_cast<float*>(pRenData);
                            for (UINT32 i = 0; i < needed; ++i) {
                                pFloats[i * 2 + 0] = renFifo[renTail * 2 + 0];
                                pFloats[i * 2 + 1] = renFifo[renTail * 2 + 1];
                                renTail = (renTail + 1) % FIFO_CAP;
                            }
                            m_pRenderService->ReleaseBuffer(needed, 0);
                        }
                    }
                }
            }
        }
    } else {
        // ====================================================================
        // ENGINE EXCLUSIVA / PRO (Elastic FIFO SPSC - Zero Descarte / WDM-KS)
        // ====================================================================
        const UINT32 targetFrames = (m_exclusiveRenderFrames > 0) ? m_exclusiveRenderFrames : static_cast<UINT32>(m_bufferSize);
        const UINT32 capCushion = (std::max)(targetFrames, m_exclusiveCaptureFrames) + static_cast<UINT32>(m_bufferSize);
        constexpr uint32_t kTrapEvents = 8;  // ~43 ms @256 healthy cadence, ~80 ms trapped
        uint32_t renderDryEvents = 0;
        int64_t lastReprimeQpc = 0;

        for (size_t i = 0; i < static_cast<size_t>(capCushion); ++i) {
            capFifo[capHead * 2 + 0] = 0.0f;
            capFifo[capHead * 2 + 1] = 0.0f;
            capHead = (capHead + 1) % FIFO_CAP;
        }

        if (startStreams()) while (!m_stopThread.load(std::memory_order_relaxed)) {
            bool renderReady = false;
            bool captureReady = false;

            if (m_hRenderEvent && m_hCaptureEvent) {
                HANDLE events[2] = { m_hRenderEvent, m_hCaptureEvent };
                DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, 20);
                if (waitRes == WAIT_OBJECT_0) {
                    renderReady = true;
                    if (WaitForSingleObject(m_hCaptureEvent, 0) == WAIT_OBJECT_0) captureReady = true;
                } else if (waitRes == WAIT_OBJECT_0 + 1) {
                    captureReady = true;
                    if (WaitForSingleObject(m_hRenderEvent, 0) == WAIT_OBJECT_0) renderReady = true;
                }
            } else if (m_hRenderEvent) {
                if (WaitForSingleObject(m_hRenderEvent, 20) == WAIT_OBJECT_0) renderReady = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                renderReady = true;
            }

            bool isDriverMuted = (m_pSharedState && m_pSharedState->softwareMute != 0);

            // 1. Drena captura para dentro do capFifo quando sinalizada
            if (captureReady && m_pCaptureService) {
                BYTE* pCapData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                HRESULT hrCap = S_OK;
                if (!m_captureIsFloat) {
                    hrCap = m_pCaptureService->GetBuffer(&pCapData, &numFrames, &flags, nullptr, nullptr);
                    if (SUCCEEDED(hrCap)) {
                        pushCapturePacket(pCapData, numFrames, flags, isDriverMuted);
                        m_pCaptureService->ReleaseBuffer(numFrames);
                    }
                } else {
                    while (SUCCEEDED(hrCap = m_pCaptureService->GetBuffer(&pCapData, &numFrames, &flags, nullptr, nullptr)) && numFrames > 0) {
                        pushCapturePacket(pCapData, numFrames, flags, isDriverMuted);
                        m_pCaptureService->ReleaseBuffer(numFrames);
                    }
                }

                if (hrCap == AUDCLNT_E_DEVICE_INVALIDATED) {
                    handleDeviceInvalidated();
                    break;
                }
            }

            // 2. Produção da DAW para renFifo e alimentação do Render Exclusivo
            if (renderReady && m_pRenderClient && m_pRenderService) {
                // Render-trap watchdog. Healthy: one period still queued when the event fires. Trapped: the queue
                // is empty at every event and only one packet is ever in flight (exclusive event mode lets us
                // write one period per event, so it never refills). After kTrapEvents empty events in a row,
                // re-prime exactly like the start: Stop, Reset, one period of silence, Start. Exceptional path
                // (a few WASAPI calls, at most once per second); nothing is logged here.
                UINT32 renPadNow = 0;
                if (SUCCEEDED(m_pRenderClient->GetCurrentPadding(&renPadNow)) && renPadNow == 0) {
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    if (++renderDryEvents >= kTrapEvents && now.QuadPart - lastReprimeQpc >= m_qpcFreq.QuadPart) {
                        m_pRenderClient->Stop();
                        m_pRenderClient->Reset();
                        BYTE* pPre = nullptr;
                        if (SUCCEEDED(m_pRenderService->GetBuffer(targetFrames, &pPre)) && pPre)
                            m_pRenderService->ReleaseBuffer(targetFrames, AUDCLNT_BUFFERFLAGS_SILENT);
                        m_pRenderClient->Start();
                        lastReprimeQpc = now.QuadPart;
                        renderDryEvents = 0;
                        m_renderReprimes.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                } else {
                    renderDryEvents = 0;
                }
                while (renFifoAvail() < targetFrames && !m_stopThread.load(std::memory_order_relaxed)) {
                    processOneAsioBlock(isDriverMuted);
                }

                // Escreve exatamente targetFrames (16-bit PCM) com Dithering TPDF no dispositivo
                if (renFifoAvail() >= targetFrames) {
                    BYTE* pRenData = nullptr;
                    HRESULT hrRen = m_pRenderService->GetBuffer(targetFrames, &pRenData);
                    if (hrRen == AUDCLNT_E_DEVICE_INVALIDATED) {
                        handleDeviceInvalidated();
                        break;
                    }
                    if (SUCCEEDED(hrRen) && pRenData) {
                        auto* pDst = reinterpret_cast<int16_t*>(pRenData);
                        for (UINT32 i = 0; i < targetFrames; ++i) {
                            float out0 = renFifo[renTail * 2 + 0];
                            float out1 = renFifo[renTail * 2 + 1];
                            renTail = (renTail + 1) % FIFO_CAP;
                            pDst[i * 2 + 0] = m_dsp.Quantize16(out0);
                            pDst[i * 2 + 1] = m_dsp.Quantize16(out1);
                        }
                        m_pRenderService->ReleaseBuffer(targetFrames, 0);
                    }
                }
            }
        }
    }

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    if (SUCCEEDED(hrCoInit)) CoUninitialize();
}
