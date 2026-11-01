#define BEHRINGER_BRIDGE_EXPORTS
#include "UM2Bridge.h"
#include "UM2Ipc.h"
#include "UM2Dsp.h"
#include "UM2Loopback.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace {

class StandaloneBridgeEngine {
public:
    StandaloneBridgeEngine() = default;
    ~StandaloneBridgeEngine() {
        Stop();
    }

    bool Start() {
        if (m_running.load()) return true;
        m_stopRequested = false;
        m_workerThread = std::thread(&StandaloneBridgeEngine::ThreadProc, this);
        return true;
    }

    void Stop() {
        if (!m_running.load() && !m_workerThread.joinable()) return;
        m_stopRequested = true;
        if (m_hCaptureEvent) {
            SetEvent(m_hCaptureEvent);
        }
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
    }

    bool IsCableDetected() const {
        return m_cableDetected.load();
    }

    bool IsStandaloneActive() const {
        return m_standaloneActive.load();
    }

private:
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::atomic<bool> m_cableDetected{ false };
    std::atomic<bool> m_standaloneActive{ false };
    std::thread m_workerThread;

    HANDLE m_hCaptureEvent{ nullptr };
    HANDLE m_hSharedMem{ nullptr };
    UM2SharedState* m_pSharedState{ nullptr };

    // DSP Engine v3.0 (Zero-Allocation)
    StudioDspProcessor m_dsp;
    StudioNoiseGate m_gateCh1;
    StudioNoiseGate m_gateCh2;
    BiquadFilter m_hpfCh1;
    AdcClipGuard m_clipCh1;
    AdcClipGuard m_clipCh2;
    AnalogPreampProcessor m_preampCh1;
    AnalogPreampProcessor m_preampCh2;
    StudioDeEsser m_deesserCh1;
    StudioFetCompressor m_compCh1;
    StudioFetCompressor m_compCh2;
    GuitarCabSimulator m_cabCh2;
    MasterMonitorProcessor m_masterMon;
    ChromaticPitchTuner m_tuner;
    UM2Loopback::ProcessLoopbackCapturer m_processLoopback;

    static constexpr size_t MAX_STANDALONE_FRAMES = 8192;
    std::array<float, MAX_STANDALONE_FRAMES> m_loopBufL{};
    std::array<float, MAX_STANDALONE_FRAMES> m_loopBufR{};
    std::array<float, MAX_STANDALONE_FRAMES> m_rawLoopL{};
    std::array<float, MAX_STANDALONE_FRAMES> m_procBufL{};
    std::array<float, MAX_STANDALONE_FRAMES> m_procBufR{};
    std::array<float, MAX_STANDALONE_FRAMES> m_monOutL{};
    std::array<float, MAX_STANDALONE_FRAMES> m_monOutR{};

    void SetupStandaloneDsp(float sr) {
        if (sr < 8000.0f) sr = 48000.0f;
        m_gateCh1.Setup(sr);
        m_gateCh2.Setup(sr);
        m_hpfCh1.SetupHighPass(sr, 80.0f);
        m_clipCh1.Setup(sr);
        m_clipCh2.Setup(sr);
        m_preampCh1.Setup(sr);
        m_preampCh2.Setup(sr);
        m_deesserCh1.Setup(sr);
        m_compCh1.Setup(sr);
        m_compCh2.Setup(sr);
        m_cabCh2.Setup(sr);
        m_masterMon.Setup(sr);
        m_tuner.Setup(sr);
    }

    void InitSharedMemory() {
        if (m_pSharedState) return;
        m_hSharedMem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, UM2_SHARED_MEM_NAME);
        if (!m_hSharedMem) {
            m_hSharedMem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(UM2SharedState), UM2_SHARED_MEM_NAME);
        }
        if (m_hSharedMem) {
            m_pSharedState = static_cast<UM2SharedState*>(MapViewOfFile(m_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UM2SharedState)));
        }
    }

    void CloseSharedMemory() {
        if (m_pSharedState) {
            UnmapViewOfFile(m_pSharedState);
            m_pSharedState = nullptr;
        }
        if (m_hSharedMem) {
            CloseHandle(m_hSharedMem);
            m_hSharedMem = nullptr;
        }
    }

    bool FindDeviceByKeywords(IMMDeviceEnumerator* pEnum, EDataFlow flow, const std::vector<std::wstring>& keywords, IMMDevice** ppDevice) {
        *ppDevice = nullptr;
        if (!pEnum) return false;

        IMMDeviceCollection* pCol = nullptr;
        HRESULT hr = pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol);
        if (FAILED(hr) || !pCol) return false;

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
                        PropVariantClear(&pv);

                        if (flow == eRender && name.find(L"16 ch") != std::wstring::npos) {
                            pStore->Release();
                            pDev->Release();
                            continue;
                        }

                        for (const auto& kw : keywords) {
                            if (name.find(kw) != std::wstring::npos) {
                                pStore->Release();
                                pCol->Release();
                                *ppDevice = pDev;
                                return true;
                            }
                        }
                    }
                    PropVariantClear(&pv);
                    pStore->Release();
                }
                pDev->Release();
            }
        }

        pCol->Release();
        return false;
    }

    void ThreadProc() {
        m_running = true;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        InitSharedMemory();

        SetupStandaloneDsp(48000.0f);

        IMMDeviceEnumerator* pEnum = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));

        IMMDevice* pMicDevice = nullptr;
        IMMDevice* pCableDevice = nullptr;
        IMMDevice* pLoopDevice = nullptr;
        IAudioClient* pCaptureClient = nullptr;
        IAudioCaptureClient* pCaptureService = nullptr;
        IAudioClient* pRenderClient = nullptr;
        IAudioRenderClient* pRenderService = nullptr;
        IAudioClient* pLoopClient = nullptr;
        IAudioCaptureClient* pLoopService = nullptr;
        IAudioClient* pMonClient = nullptr;
        IAudioRenderClient* pMonService = nullptr;
        UINT32 monChannels = 2;

        LARGE_INTEGER qpcFreq{};
        QueryPerformanceFrequency(&qpcFreq);
        float smoothedCpuPct = 0.0f;
        float currentSampleRate = 48000.0f;

        auto ReleaseLoopback = [&]() {
            m_processLoopback.Shutdown();
            if (pMonClient) {
                pMonClient->Stop();
                pMonClient->Release();
                pMonClient = nullptr;
            }
            if (pMonService) {
                pMonService->Release();
                pMonService = nullptr;
            }
            if (pLoopClient) {
                pLoopClient->Stop();
                pLoopClient->Release();
                pLoopClient = nullptr;
            }
            if (pLoopService) {
                pLoopService->Release();
                pLoopService = nullptr;
            }
            if (pLoopDevice) {
                pLoopDevice->Release();
                pLoopDevice = nullptr;
            }
        };

        auto ReleaseCapture = [&]() {
            if (pCaptureClient) {
                pCaptureClient->Stop();
                pCaptureClient->Release();
                pCaptureClient = nullptr;
            }
            if (pCaptureService) {
                pCaptureService->Release();
                pCaptureService = nullptr;
            }
            if (pMicDevice) {
                pMicDevice->Release();
                pMicDevice = nullptr;
            }
            if (m_hCaptureEvent) {
                CloseHandle(m_hCaptureEvent);
                m_hCaptureEvent = nullptr;
            }
            ReleaseLoopback();
            m_standaloneActive = false;
        };

        auto ReleaseRender = [&]() {
            if (pRenderClient) {
                pRenderClient->Stop();
                pRenderClient->Release();
                pRenderClient = nullptr;
            }
            if (pRenderService) {
                pRenderService->Release();
                pRenderService = nullptr;
            }
            if (pCableDevice) {
                pCableDevice->Release();
                pCableDevice = nullptr;
            }
        };

        std::vector<std::wstring> micKeywords = { L"usb audio", L"u-phoria", L"um2", L"umc22", L"behringer" };
        std::vector<std::wstring> cableKeywords = { L"cable input", L"vb-audio", L"voicemeeter", L"virtual audio" };

        uint32_t scopeSubsample = 0;
        UINT32 micChannels = 2;
        bool micIsFloat = true;
        UINT32 cableChannels = 2;
        int dawStoppedTicks = 15;
        int staleDawTicks = 0;
        uint64_t lastSwitchCount = 0;
        int cableCheckCounter = 0;
        float lastCableL = 0.0f;
        float lastCableR = 0.0f;

        while (!m_stopRequested.load()) {
            if (!m_pSharedState) {
                InitSharedMemory();
            }

            // Watchdog de batimento cardíaco da DAW: se isStreaming == 1 mas bufferSwitchCount congelou por ~1s, limpa estado órfão
            bool isDawStreaming = (m_pSharedState && m_pSharedState->isStreaming != 0);
            if (isDawStreaming && m_pSharedState) {
                uint64_t curSwitch = m_pSharedState->bufferSwitchCount;
                if (curSwitch == lastSwitchCount) {
                    staleDawTicks++;
                    if (staleDawTicks >= 25) {
                        m_pSharedState->isStreaming = 0;
                        m_pSharedState->driverStatus = 0;
                        m_pSharedState->peakIn1 = 0.0f;
                        m_pSharedState->peakIn2 = 0.0f;
                        m_pSharedState->peakLoopbackL = 0.0f;
                        m_pSharedState->peakLoopbackR = 0.0f;
                        m_pSharedState->peakOut1 = 0.0f;
                        m_pSharedState->peakOut2 = 0.0f;
                        isDawStreaming = false;
                        staleDawTicks = 0;
                    }
                } else {
                    lastSwitchCount = curSwitch;
                    staleDawTicks = 0;
                }
            } else {
                staleDawTicks = 0;
            }

            if (isDawStreaming) {
                dawStoppedTicks = 0;
                if (pCaptureClient) {
                    ReleaseCapture();
                }
                if (pRenderClient) {
                    ReleaseRender();
                }
                m_standaloneActive = false;

                // Verifica presença física do VB-Cable sem abrir stream concorrente
                if (cableCheckCounter % 50 == 0) {
                    if (!pEnum) {
                        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
                    }
                    IMMDevice* pTmpCable = nullptr;
                    if (pEnum && FindDeviceByKeywords(pEnum, eRender, cableKeywords, &pTmpCable)) {
                        m_cableDetected = true;
                        pTmpCable->Release();
                    } else {
                        m_cableDetected = false;
                    }
                }
                cableCheckCounter++;

                Sleep(40);
                continue;
            }

            // Aguarda ~600ms de inatividade contínua após a DAW parar para não colidir com kAsioResetRequest
            if (dawStoppedTicks < 15) {
                dawStoppedTicks++;
                Sleep(40);
                continue;
            }

            // Modo Standalone: DAW fechada (isStreaming == 0)
            // 1. Inicializa Microfone Behringer em Shared Mode se necessário
            if (!pCaptureClient) {
                if (!pEnum) {
                    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
                }

                if (pEnum && FindDeviceByKeywords(pEnum, eCapture, micKeywords, &pMicDevice)) {
                    hr = pMicDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pCaptureClient));
                    if (SUCCEEDED(hr) && pCaptureClient) {
                        WAVEFORMATEX* pMixFormat = nullptr;
                        hr = pCaptureClient->GetMixFormat(&pMixFormat);
                        if (SUCCEEDED(hr) && pMixFormat) {
                            micChannels = (std::max)(1u, static_cast<UINT32>(pMixFormat->nChannels));
                            micIsFloat = (pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                (pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pMixFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                            float sr = static_cast<float>(pMixFormat->nSamplesPerSec > 0 ? pMixFormat->nSamplesPerSec : 48000);
                            currentSampleRate = sr;
                            SetupStandaloneDsp(sr);

                            REFERENCE_TIME hnsBuffer = 100000; // 10ms
                            hr = pCaptureClient->Initialize(
                                AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                hnsBuffer,
                                0,
                                pMixFormat,
                                nullptr
                            );

                            if (SUCCEEDED(hr)) {
                                m_hCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                                pCaptureClient->SetEventHandle(m_hCaptureEvent);
                                pCaptureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pCaptureService));
                                pCaptureClient->Start();
                                m_standaloneActive = true;
                            } else {
                                ReleaseCapture();
                            }
                            CoTaskMemFree(pMixFormat);
                        } else {
                            ReleaseCapture();
                        }
                    } else {
                        ReleaseCapture();
                    }
                }
            }

            // 2. Inicializa Loopback do Desktop + Monitor Direto nos Fones (Alto-falantes Behringer) em modo Standalone
            if (!pLoopClient && pEnum) {
                if (FindDeviceByKeywords(pEnum, eRender, micKeywords, &pLoopDevice)) {
                    if (SUCCEEDED(pLoopDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pLoopClient))) && pLoopClient) {
                        WAVEFORMATEX* pLoopFmt = nullptr;
                        if (SUCCEEDED(pLoopClient->GetMixFormat(&pLoopFmt)) && pLoopFmt) {
                            if (SUCCEEDED(pLoopClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, pLoopFmt, nullptr))) {
                                pLoopClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pLoopService));
                                pLoopClient->Start();
                            } else {
                                ReleaseLoopback();
                            }
                            CoTaskMemFree(pLoopFmt);
                        } else {
                            ReleaseLoopback();
                        }
                    }
                    // Cliente de Render compartilhado de baixa latência para audição imediata do DSP nos fones (MON ONLY)
                    if (pLoopDevice && !pMonClient) {
                        if (SUCCEEDED(pLoopDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pMonClient))) && pMonClient) {
                            WAVEFORMATEX* pMonFmt = nullptr;
                            if (SUCCEEDED(pMonClient->GetMixFormat(&pMonFmt)) && pMonFmt) {
                                monChannels = (std::max)(1u, static_cast<UINT32>(pMonFmt->nChannels));
                                if (SUCCEEDED(pMonClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 250000, 0, pMonFmt, nullptr))) {
                                    pMonClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pMonService));
                                    pMonClient->Start();
                                } else {
                                    pMonClient->Release();
                                    pMonClient = nullptr;
                                }
                                CoTaskMemFree(pMonFmt);
                            }
                        }
                    }
                }
            }

            // 3. Inicializa Cabo Virtual de Render (CABLE Input) se necessário
            if (!pRenderClient && pEnum) {
                if (FindDeviceByKeywords(pEnum, eRender, cableKeywords, &pCableDevice)) {
                    m_cableDetected = true;
                    hr = pCableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pRenderClient));
                    if (SUCCEEDED(hr) && pRenderClient) {
                        WAVEFORMATEX* pRenderFormat = nullptr;
                        hr = pRenderClient->GetMixFormat(&pRenderFormat);
                        if (SUCCEEDED(hr) && pRenderFormat) {
                            cableChannels = (std::max)(1u, static_cast<UINT32>(pRenderFormat->nChannels));
                            DWORD origCableRate = pRenderFormat->nSamplesPerSec;
                            DWORD targetRate = static_cast<DWORD>(currentSampleRate > 8000.0f ? currentSampleRate : 48000.0f);
                            pRenderFormat->nSamplesPerSec = targetRate;
                            pRenderFormat->nAvgBytesPerSec = pRenderFormat->nSamplesPerSec * pRenderFormat->nBlockAlign;

                            REFERENCE_TIME hnsBuffer = 600000; // 60ms
                            hr = pRenderClient->Initialize(
                                AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                hnsBuffer,
                                0,
                                pRenderFormat,
                                nullptr
                            );
                            if (FAILED(hr)) {
                                pRenderFormat->nSamplesPerSec = origCableRate;
                                pRenderFormat->nAvgBytesPerSec = pRenderFormat->nSamplesPerSec * pRenderFormat->nBlockAlign;
                                pRenderClient->Release();
                                pRenderClient = nullptr;
                                if (SUCCEEDED(pCableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pRenderClient))) && pRenderClient) {
                                    hr = pRenderClient->Initialize(
                                        AUDCLNT_SHAREMODE_SHARED,
                                        0,
                                        hnsBuffer,
                                        0,
                                        pRenderFormat,
                                        nullptr
                                    );
                                }
                            }
                            if (SUCCEEDED(hr)) {
                                pRenderClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pRenderService));
                                lastCableL = 0.0f;
                                lastCableR = 0.0f;
                                if (pRenderService) {
                                    UINT32 cableBufSize = 0;
                                    pRenderClient->GetBufferSize(&cableBufSize);
                                    UINT32 preRoll = (std::min)(1200u, cableBufSize / 2u);
                                    BYTE* pPre = nullptr;
                                    if (preRoll > 0 && SUCCEEDED(pRenderService->GetBuffer(preRoll, &pPre)) && pPre) {
                                        pRenderService->ReleaseBuffer(preRoll, AUDCLNT_BUFFERFLAGS_SILENT);
                                    }
                                }
                                pRenderClient->Start();
                            } else {
                                ReleaseRender();
                            }
                            CoTaskMemFree(pRenderFormat);
                        } else {
                            ReleaseRender();
                        }
                    }
                } else {
                    m_cableDetected = false;
                }
            }

            if (!pCaptureClient || !m_hCaptureEvent) {
                Sleep(50);
                continue;
            }

            // Aguarda novo bloco de áudio do microfone Behringer
            DWORD waitRes = WaitForSingleObject(m_hCaptureEvent, 20);
            if (waitRes == WAIT_OBJECT_0 && pCaptureService) {
                // Drena pacotes do Loopback do PC (se ativo) sem alocação dinâmica
                size_t loopCount = 0;
                float maxLoopL = 0.0f, maxLoopR = 0.0f;
                bool loopEnabled   = (m_pSharedState ? (m_pSharedState->loopbackEnabled != 0) : true);
                bool loopMono      = (m_pSharedState && m_pSharedState->loopbackMono != 0);
                bool loopBroadcast = (m_pSharedState && m_pSharedState->loopbackBroadcastMix != 0);
                float loopGain     = (m_pSharedState ? m_pSharedState->loopbackGain : 1.0f);
                uint32_t loopMode  = (m_pSharedState ? m_pSharedState->loopbackMode : 0);
                uint32_t loopPid   = (m_pSharedState ? m_pSharedState->loopbackTargetPid : 0);

                if (loopMode == 1 || loopMode == 2) {
                    m_processLoopback.SyncConfiguration(loopMode, loopPid, 48000);
                } else {
                    m_processLoopback.Shutdown();
                }

                if ((loopMode == 1 || loopMode == 2) && m_processLoopback.IsActive()) {
                    // Drain any system loopback to prevent buffer buildup
                    if (pLoopService) {
                        BYTE* pLData = nullptr;
                        UINT32 lFrames = 0;
                        DWORD lFlags = 0;
                        while (SUCCEEDED(pLoopService->GetBuffer(&pLData, &lFrames, &lFlags, nullptr, nullptr)) && lFrames > 0) {
                            pLoopService->ReleaseBuffer(lFrames);
                        }
                    }
                    constexpr size_t PROC_CHUNK = 480;
                    m_processLoopback.DrainAndReadStereo(m_loopBufL.data(), m_loopBufR.data(), PROC_CHUNK);
                    loopCount = PROC_CHUNK;
                    for (size_t i = 0; i < loopCount; ++i) {
                        float rawL = m_loopBufL[i] * loopGain;
                        float rawR = m_loopBufR[i] * loopGain;
                        m_rawLoopL[i] = (rawL + rawR) * 0.5f;
                        float sL = loopEnabled ? rawL : 0.0f;
                        float sR = loopEnabled ? rawR : 0.0f;
                        if (loopMono) {
                            float m = (sL + sR) * 0.5f;
                            sL = m; sR = m;
                        }
                        m_loopBufL[i] = sL;
                        m_loopBufR[i] = sR;
                        maxLoopL = (std::max)(maxLoopL, std::abs(sL));
                        maxLoopR = (std::max)(maxLoopR, std::abs(sR));
                    }
                } else if (pLoopService) {
                    BYTE* pLData = nullptr;
                    UINT32 lFrames = 0;
                    DWORD lFlags = 0;
                    while (SUCCEEDED(pLoopService->GetBuffer(&pLData, &lFrames, &lFlags, nullptr, nullptr)) && lFrames > 0) {
                        bool hwSilent = (lFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !pLData;
                        const float* pLFloats = reinterpret_cast<const float*>(pLData);
                        for (UINT32 i = 0; i < lFrames && loopCount < MAX_STANDALONE_FRAMES; ++i) {
                            float rawL = hwSilent ? 0.0f : (pLFloats[i * 2 + 0] * loopGain);
                            float rawR = hwSilent ? 0.0f : (pLFloats[i * 2 + 1] * loopGain);
                            m_rawLoopL[loopCount] = (rawL + rawR) * 0.5f;
                            float sL = loopEnabled ? rawL : 0.0f;
                            float sR = loopEnabled ? rawR : 0.0f;
                            if (loopMono) {
                                float m = (sL + sR) * 0.5f;
                                sL = m; sR = m;
                            }
                            m_loopBufL[loopCount] = sL;
                            m_loopBufR[loopCount] = sR;
                            loopCount++;
                            maxLoopL = (std::max)(maxLoopL, std::abs(sL));
                            maxLoopR = (std::max)(maxLoopR, std::abs(sR));
                        }
                        pLoopService->ReleaseBuffer(lFrames);
                    }
                }
                if (m_pSharedState) {
                    if (loopEnabled) {
                        m_pSharedState->peakLoopbackL = (std::max)(maxLoopL, m_pSharedState->peakLoopbackL * 0.90f);
                        m_pSharedState->peakLoopbackR = (std::max)(maxLoopR, m_pSharedState->peakLoopbackR * 0.90f);
                    } else {
                        m_pSharedState->peakLoopbackL = 0.0f;
                        m_pSharedState->peakLoopbackR = 0.0f;
                    }
                }

                BYTE* pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;

                while (SUCCEEDED(pCaptureService->GetBuffer(&pData, &numFramesAvailable, &flags, &devPos, &qpcPos)) && numFramesAvailable > 0) {
                    LARGE_INTEGER qpcBlockStart{};
                    QueryPerformanceCounter(&qpcBlockStart);

                    const float* pFloats = reinterpret_cast<const float*>(pData);
                    const int16_t* pInts = reinterpret_cast<const int16_t*>(pData);
                    bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !pData;

                    // Lê configurações DSP v3.0 do IPC
                    int32_t hwModel    = (m_pSharedState ? static_cast<int32_t>(m_pSharedState->hardwareModel) : 0);
                    bool ch1HpfOn      = (m_pSharedState && m_pSharedState->ch1Hpf != 0);
                    bool ch1GateOn     = (m_pSharedState && m_pSharedState->ch1Gate != 0);
                    bool ch1PhaseInv   = (m_pSharedState && m_pSharedState->ch1Phase != 0);
                    bool ch1Muted      = (m_pSharedState && m_pSharedState->ch1Mute != 0);
                    float ch1Trim      = (m_pSharedState ? m_pSharedState->ch1Trim : 1.0f);
                    uint32_t ch1Preamp = (m_pSharedState ? m_pSharedState->ch1PreampMode : 0);
                    float ch1CompAmt   = (m_pSharedState ? m_pSharedState->ch1CompAmount : 0.0f);
                    float ch1DeessAmt  = (m_pSharedState ? m_pSharedState->ch1DeesserAmount : 0.0f);

                    bool ch2PhaseInv   = (m_pSharedState && m_pSharedState->ch2Phase != 0);
                    bool ch2PadOn      = (m_pSharedState && m_pSharedState->ch2Pad != 0);
                    bool ch2GateOn     = (m_pSharedState ? (m_pSharedState->ch2Gate != 0) : true);
                    bool ch2Muted      = (m_pSharedState && m_pSharedState->ch2Mute != 0);
                    float ch2Trim      = (m_pSharedState ? m_pSharedState->ch2Trim : 1.0f);
                    uint32_t ch2Preamp = (m_pSharedState ? m_pSharedState->ch2PreampMode : 0);
                    float ch2CompAmt   = (m_pSharedState ? m_pSharedState->ch2CompAmount : 0.0f);
                    bool ch2CabSimOn   = (m_pSharedState && m_pSharedState->ch2CabSim != 0);

                    bool dspMonOnly    = (m_pSharedState && m_pSharedState->dspMonitorOnly != 0);
                    bool monSubCheck   = (m_pSharedState && m_pSharedState->monSubCheck != 0);
                    bool monPhoneSim   = (m_pSharedState && m_pSharedState->monPhoneSim != 0);
                    bool monCrossfeed  = (m_pSharedState && m_pSharedState->monCrossfeed != 0);
                    bool masterDim     = (m_pSharedState && m_pSharedState->masterDim != 0);
                    bool masterMono    = (m_pSharedState && m_pSharedState->masterMono != 0);
                    bool masterMuted   = (m_pSharedState && m_pSharedState->masterMute != 0);
                    float masterTrim   = (m_pSharedState ? m_pSharedState->masterTrim : 1.0f);

                    float gateThresh   = (m_pSharedState ? m_pSharedState->gateThresholdDb : -58.0f);
                    if (gateThresh > -53.0f && gateThresh < -51.0f) {
                        gateThresh = -58.0f;
                    }
                    bool swMuted       = (m_pSharedState && m_pSharedState->softwareMute != 0);
                    uint32_t tunerSrc  = (m_pSharedState ? m_pSharedState->tunerSource : 0);

                    const UINT32 safeFrames = (std::min)(numFramesAvailable, static_cast<UINT32>(MAX_STANDALONE_FRAMES));

                    float maxIn1 = 0.0f;
                    float maxIn2 = 0.0f;
                    float maxOut1 = 0.0f;
                    float maxOut2 = 0.0f;

                    for (UINT32 s = 0; s < safeFrames; ++s) {
                        float in0 = 0.0f;
                        float in1 = 0.0f;
                        if (!silent) {
                            if (micChannels == 1) {
                                in0 = micIsFloat ? pFloats[s] : (pInts[s] / 32768.0f);
                                in1 = 0.0f;
                            } else {
                                in0 = micIsFloat ? pFloats[s * micChannels + 0] : (pInts[s * micChannels + 0] / 32768.0f);
                                in1 = micIsFloat ? pFloats[s * micChannels + 1] : (pInts[s * micChannels + 1] / 32768.0f);
                            }
                        }

                        m_clipCh1.CheckSample(in0);
                        m_clipCh2.CheckSample(in1);

                        // DSP CH 1 (Mic XLR)
                        in0 = m_hpfCh1.Process(in0, ch1HpfOn);
                        float effGateThresh1 = ch1HpfOn ? (gateThresh - 3.0f) : gateThresh;
                        in0 = m_gateCh1.Process(in0, ch1GateOn, effGateThresh1);
                        if (ch1PhaseInv) in0 = -in0;
                        in0 *= ch1Trim;
                        if (ch1Muted || swMuted) in0 = 0.0f;
                        in0 = m_preampCh1.Process(in0, static_cast<int32_t>(ch1Preamp), hwModel);
                        in0 = m_deesserCh1.Process(in0, ch1DeessAmt);
                        in0 = m_compCh1.Process(in0, ch1CompAmt);
                        in0 = StudioDspProcessor::SoftClip(in0);

                        // DSP CH 2 (Inst Hi-Z)
                        in1 = m_gateCh2.Process(in1, ch2GateOn, gateThresh);
                        if (ch2PhaseInv) in1 = -in1;
                        if (ch2PadOn) in1 *= 0.31622776f; // -10dB
                        in1 *= ch2Trim;
                        if (ch2Muted || swMuted) in1 = 0.0f;
                        in1 = m_preampCh2.Process(in1, static_cast<int32_t>(ch2Preamp), hwModel);
                        in1 = m_compCh2.Process(in1, ch2CompAmt);
                        in1 = m_cabCh2.Process(in1, ch2CabSimOn);
                        in1 = StudioDspProcessor::SoftClip(in1);

                        m_procBufL[s] = in0;
                        m_procBufR[s] = in1;

                        // Processamento Master Out & Mix Check (808 SUB, PHONE, X-FEED, DIM, MONO, MUTE, TRIM)
                        float out0Val = (in0 + in1) * masterTrim;
                        float out1Val = (in0 + in1) * masterTrim;
                        if (loopEnabled && loopCount > 0) {
                            size_t lIdx = s % loopCount;
                            out0Val += m_loopBufL[lIdx] * masterTrim;
                            out1Val += m_loopBufR[lIdx] * masterTrim;
                        }
                        if (masterMono) {
                            float mMix = (out0Val + out1Val) * 0.5f;
                            out0Val = mMix;
                            out1Val = mMix;
                        }
                        m_masterMon.ProcessStereo(out0Val, out1Val, monSubCheck, monPhoneSim, monCrossfeed);
                        if (masterDim) {
                            out0Val *= 0.19952623f;
                            out1Val *= 0.19952623f;
                        }
                        if (masterMuted) {
                            out0Val = 0.0f;
                            out1Val = 0.0f;
                        }
                        out0Val = StudioDspProcessor::SoftClip(out0Val);
                        out1Val = StudioDspProcessor::SoftClip(out1Val);
                        m_monOutL[s] = out0Val;
                        m_monOutR[s] = out1Val;

                        if (tunerSrc != 0) {
                            float tSample = 0.0f;
                            if (tunerSrc == 1) tSample = in0;
                            else if (tunerSrc == 2) tSample = in1;
                            else if (tunerSrc == 3 && loopCount > 0) tSample = m_rawLoopL[s % loopCount];
                            m_tuner.PushSample(tSample);
                        }

                        maxIn1 = (std::max)(maxIn1, std::abs(in0));
                        maxIn2 = (std::max)(maxIn2, std::abs(in1));
                        maxOut1 = (std::max)(maxOut1, std::abs(out0Val));
                        maxOut2 = (std::max)(maxOut2, std::abs(out1Val));
                    }

                    LARGE_INTEGER qpcBlockEnd{};
                    QueryPerformanceCounter(&qpcBlockEnd);

                    // Atualiza Memória Compartilhada e Osciloscópio (60 FPS)
                    if (m_pSharedState && safeFrames > 0) {
                        m_pSharedState->peakIn1 = (std::max)(maxIn1, m_pSharedState->peakIn1 * 0.90f);
                        m_pSharedState->peakIn2 = (std::max)(maxIn2, m_pSharedState->peakIn2 * 0.90f);
                        m_pSharedState->peakOut1 = (std::max)(maxOut1, m_pSharedState->peakOut1 * 0.90f);
                        m_pSharedState->peakOut2 = (std::max)(maxOut2, m_pSharedState->peakOut2 * 0.90f);
                        m_pSharedState->ch1HwClip = m_clipCh1.IsClipping() ? 1u : 0u;
                        m_pSharedState->ch2HwClip = m_clipCh2.IsClipping() ? 1u : 0u;
                        m_pSharedState->ch1GainReductionDb = m_compCh1.GetGainReductionDb() + m_deesserCh1.GetGainReductionDb();
                        m_pSharedState->ch2GainReductionDb = m_compCh2.GetGainReductionDb();

                        if (qpcFreq.QuadPart > 0 && currentSampleRate > 0.0f) {
                            int64_t dspTicks = qpcBlockEnd.QuadPart - qpcBlockStart.QuadPart;
                            double dspSec = static_cast<double>((std::max)(int64_t(0), dspTicks)) / static_cast<double>(qpcFreq.QuadPart);
                            double budgetSec = static_cast<double>(safeFrames) / static_cast<double>(currentSampleRate);
                            float blockPct = (budgetSec > 0.0) ? static_cast<float>((dspSec / budgetSec) * 100.0) : 0.0f;
                            smoothedCpuPct = smoothedCpuPct * 0.88f + blockPct * 0.12f;
                            m_pSharedState->dspCpuLoadPercent = (std::min)(99.9f, (std::max)(0.1f, smoothedCpuPct));
                        }

                        if (tunerSrc != 0) {
                            m_pSharedState->tunerFreqHz = m_tuner.GetFrequencyHz();
                            m_pSharedState->tunerMidiNote = m_tuner.GetMidiNote();
                            m_pSharedState->tunerCents = m_tuner.GetCents();
                        } else {
                            m_pSharedState->tunerFreqHz = 0.0f;
                            m_pSharedState->tunerMidiNote = 0;
                            m_pSharedState->tunerCents = 0.0f;
                        }

                        scopeSubsample++;
                        if (scopeSubsample % 2 == 0) {
                            uint32_t src = m_pSharedState->scopeSource;
                            UINT32 step = (std::max)(1u, safeFrames / 128u);
                            for (int i = 0; i < 128; ++i) {
                                UINT32 idx = (i * step) % safeFrames;
                                float val = 0.0f;
                                if (src == 1) val = m_procBufL[idx];
                                else if (src == 2) val = m_procBufR[idx];
                                else if (src == 3 && loopCount > 0) val = m_rawLoopL[idx % loopCount];
                                else val = (m_monOutL[idx] + m_monOutR[idx]) * 0.5f;
                                m_pSharedState->scopeBuffer[i] = val;
                            }
                        }
                    }

                    // Monitoração Direta nos Fones da Behringer UM2 em Standalone quando [MON ONLY] está ativo
                    if (pMonService && pMonClient && safeFrames > 0) {
                        UINT32 monPad = 0;
                        if (SUCCEEDED(pMonClient->GetCurrentPadding(&monPad))) {
                            UINT32 monBufSize = 0;
                            pMonClient->GetBufferSize(&monBufSize);
                            UINT32 monAvail = (monBufSize > monPad) ? (monBufSize - monPad) : 0;
                            UINT32 monWrite = (std::min)(safeFrames, monAvail);
                            if (monWrite > 0 && monPad < 2400) {
                                BYTE* pMonDst = nullptr;
                                if (SUCCEEDED(pMonService->GetBuffer(monWrite, &pMonDst)) && pMonDst) {
                                    if (dspMonOnly && !loopBroadcast && !masterMuted) {
                                        float* pMonFloats = reinterpret_cast<float*>(pMonDst);
                                        const UINT32 nMonCh = (std::max)(1u, monChannels);
                                        for (UINT32 s = 0; s < monWrite; ++s) {
                                            pMonFloats[s * nMonCh + 0] = m_monOutL[s];
                                            if (nMonCh >= 2) pMonFloats[s * nMonCh + 1] = m_monOutR[s];
                                            for (UINT32 ch = 2; ch < nMonCh; ++ch) pMonFloats[s * nMonCh + ch] = 0.0f;
                                        }
                                        pMonService->ReleaseBuffer(monWrite, 0);
                                    } else {
                                        pMonService->ReleaseBuffer(monWrite, AUDCLNT_BUFFERFLAGS_SILENT);
                                    }
                                }
                            }
                        }
                    }

                    // Envia para o Cabo Virtual (Discord / OBS / Jogos) com colchão elástico e centralização estéreo
                    if (pRenderService && pRenderClient && safeFrames > 0) {
                        UINT32 padding = 0;
                        if (SUCCEEDED(pRenderClient->GetCurrentPadding(&padding))) {
                            UINT32 bufferSize = 0;
                            pRenderClient->GetBufferSize(&bufferSize);
                            const UINT32 nCh = (std::max)(1u, cableChannels);

                            if (padding < 480 && (bufferSize - padding) >= (safeFrames + 720)) {
                                BYTE* pPadDst = nullptr;
                                const UINT32 cushionFrames = 720;
                                if (SUCCEEDED(pRenderService->GetBuffer(cushionFrames, &pPadDst)) && pPadDst) {
                                    float* pPadFloats = reinterpret_cast<float*>(pPadDst);
                                    for (UINT32 i = 0; i < cushionFrames; ++i) {
                                        float decay = 1.0f - (static_cast<float>(i) / static_cast<float>(cushionFrames));
                                        pPadFloats[i * nCh + 0] = lastCableL * decay;
                                        if (nCh >= 2) pPadFloats[i * nCh + 1] = lastCableR * decay;
                                        for (UINT32 ch = 2; ch < nCh; ++ch) pPadFloats[i * nCh + ch] = 0.0f;
                                    }
                                    pRenderService->ReleaseBuffer(cushionFrames, 0);
                                    lastCableL = 0.0f;
                                    lastCableR = 0.0f;
                                    padding += cushionFrames;
                                }
                            }

                            UINT32 writeFrames = safeFrames;
                            if (padding > 2160 && safeFrames > 4) {
                                writeFrames = safeFrames - 1;
                            }

                            UINT32 available = bufferSize - padding;
                            if (available >= writeFrames) {
                                BYTE* pOutData = nullptr;
                                if (SUCCEEDED(pRenderService->GetBuffer(writeFrames, &pOutData)) && pOutData) {
                                    float* pOutFloats = reinterpret_cast<float*>(pOutData);
                                    for (UINT32 s = 0; s < writeFrames; ++s) {
                                        float micMix = m_procBufL[s] + m_procBufR[s];
                                        float outL = micMix;
                                        float outR = micMix;
                                        if (loopEnabled && loopBroadcast && loopCount > 0) {
                                            size_t lIdx = (std::min)(static_cast<size_t>(s), loopCount - 1);
                                            outL = m_loopBufL[lIdx] + micMix * 0.7071f;
                                            outR = m_loopBufR[lIdx] + micMix * 0.7071f;
                                        }
                                        outL = StudioDspProcessor::SoftClip(outL);
                                        outR = StudioDspProcessor::SoftClip(outR);
                                        pOutFloats[s * nCh + 0] = outL;
                                        if (nCh >= 2) pOutFloats[s * nCh + 1] = outR;
                                        for (UINT32 ch = 2; ch < nCh; ++ch) {
                                            pOutFloats[s * nCh + ch] = 0.0f;
                                        }
                                        lastCableL = outL;
                                        lastCableR = outR;
                                    }
                                    pRenderService->ReleaseBuffer(writeFrames, 0);
                                }
                            }
                        }
                    }

                    pCaptureService->ReleaseBuffer(numFramesAvailable);
                }
            }
        }

        ReleaseCapture();
        ReleaseRender();

        if (pEnum) {
            pEnum->Release();
            pEnum = nullptr;
        }

        CloseSharedMemory();

        if (hAvrt) {
            AvRevertMmThreadCharacteristics(hAvrt);
        }
        CoUninitialize();
        m_running = false;
    }
};

StandaloneBridgeEngine g_bridgeEngine;

} // namespace

extern "C" {

BRIDGE_API bool __cdecl UM2Bridge_Start() {
    return g_bridgeEngine.Start();
}

BRIDGE_API void __cdecl UM2Bridge_Stop() {
    g_bridgeEngine.Stop();
}

BRIDGE_API bool __cdecl UM2Bridge_IsCableDetected() {
    return g_bridgeEngine.IsCableDetected();
}

BRIDGE_API bool __cdecl UM2Bridge_IsStandaloneActive() {
    return g_bridgeEngine.IsStandaloneActive();
}

}
