#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <array>
#include <cstring>
#include <algorithm>

// Estruturas oficiais do Windows 10 Build 20348+ / Windows 11 para Process Loopback
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"

enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
};

struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};

enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
};

struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
};
#endif

namespace UM2Loopback {

// Handler COM Agile para ActivateAudioInterfaceAsync
class ProcessLoopbackActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ProcessLoopbackActivationHandler() : m_refCount(1) {
        m_hDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~ProcessLoopbackActivationHandler() {
        if (m_hDoneEvent) {
            CloseHandle(m_hDoneEvent);
            m_hDoneEvent = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = InterlockedDecrement(&m_refCount);
        if (c == 0) delete this;
        return c;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        if (operation) {
            HRESULT hrActivate = E_FAIL;
            IUnknown* pUnk = nullptr;
            if (SUCCEEDED(operation->GetActivateResult(&hrActivate, &pUnk)) && SUCCEEDED(hrActivate) && pUnk) {
                pUnk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&m_pAudioClient));
            }
            if (pUnk) pUnk->Release();
            m_hrResult = hrActivate;
        }
        if (m_hDoneEvent) SetEvent(m_hDoneEvent);
        return S_OK;
    }

    bool Wait(DWORD timeoutMs = 1500) const {
        if (!m_hDoneEvent) return false;
        return WaitForSingleObject(m_hDoneEvent, timeoutMs) == WAIT_OBJECT_0;
    }

    IAudioClient* DetachClient() noexcept {
        IAudioClient* p = m_pAudioClient;
        m_pAudioClient = nullptr;
        return p;
    }

private:
    LONG m_refCount{ 1 };
    HANDLE m_hDoneEvent{ nullptr };
    HRESULT m_hrResult{ E_FAIL };
    IAudioClient* m_pAudioClient{ nullptr };
};

// Capturador Lock-Free de Process Loopback (Isolamento de App por PID ou Exclusão Anti-Feedback)
class ProcessLoopbackCapturer {
public:
    static constexpr size_t FIFO_CAP = 16384; // Frames estéreo pré-alocados

    ProcessLoopbackCapturer() = default;
    ~ProcessLoopbackCapturer() {
        Shutdown();
    }

    void Shutdown() {
        m_ready.store(false, std::memory_order_release);
        while (m_rtBusy.test_and_set(std::memory_order_acquire)) {
            Sleep(1);
        }
        ReleaseClientInternal();
        m_currentMode = 0;
        m_currentPid = 0;
        m_currentSampleRate = 0;
        m_rtBusy.clear(std::memory_order_release);
    }

    // Chamado APENAS da thread de controle (IpcWatcherLoop ou UM2Bridge Init), nunca da thread RT
    void SyncConfiguration(int32_t mode, uint32_t targetPid, uint32_t sampleRate) {
        if (sampleRate == 0) sampleRate = 48000;
        uint32_t effectivePid = targetPid;
        if (mode == 2 && effectivePid == 0) {
            effectivePid = GetCurrentProcessId();
        }
        if (mode == 1 && effectivePid == 0) {
            // Modo 1 (Isolar PID) sem PID selecionado -> desativa captura de processo
            mode = 0;
        }

        if (mode == m_currentMode && effectivePid == m_currentPid && sampleRate == m_currentSampleRate) {
            return;
        }

        m_ready.store(false, std::memory_order_release);
        while (m_rtBusy.test_and_set(std::memory_order_acquire)) {
            Sleep(1);
        }

        ReleaseClientInternal();
        m_head = 0;
        m_tail = 0;
        m_fifo.fill(0.0f);

        m_currentMode = mode;
        m_currentPid = effectivePid;
        m_currentSampleRate = sampleRate;

        if (mode == 1 || mode == 2) {
            bool ok = InitProcessLoopbackInternal(mode, effectivePid, sampleRate);
            m_ready.store(ok, std::memory_order_release);
        }

        m_rtBusy.clear(std::memory_order_release);
    }

    bool IsProcessCaptureActive() const noexcept {
        return m_ready.load(std::memory_order_acquire);
    }

    bool IsActive() const noexcept {
        return m_ready.load(std::memory_order_acquire);
    }

    // Chamado na thread RT de áudio: non-blocking (usa try_lock via test_and_set)
    bool DrainAndReadStereo(float* outL, float* outR, size_t framesNeeded) noexcept {
        if (!m_ready.load(std::memory_order_acquire)) {
            return false;
        }
        if (m_rtBusy.test_and_set(std::memory_order_acquire)) {
            return false; // Em reconfiguração pela thread IPC, não bloqueia thread RT
        }

        if (!m_pCaptureService) {
            m_rtBusy.clear(std::memory_order_release);
            return false;
        }

        BYTE* pData = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        while (SUCCEEDED(m_pCaptureService->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr)) && numFrames > 0) {
            bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !pData;
            const float* pFloats = reinterpret_cast<const float*>(pData);
            for (UINT32 i = 0; i < numFrames; ++i) {
                float sL = isSilent ? 0.0f : pFloats[i * 2 + 0];
                float sR = isSilent ? 0.0f : pFloats[i * 2 + 1];
                m_fifo[m_head * 2 + 0] = sL;
                m_fifo[m_head * 2 + 1] = sR;
                m_head = (m_head + 1) % FIFO_CAP;
                if (m_head == m_tail) {
                    m_tail = (m_tail + 1) % FIFO_CAP;
                }
            }
            m_pCaptureService->ReleaseBuffer(numFrames);
        }

        // Controle de deriva: se acumular mais de 4096 frames (~85ms), avança tail para manter latência baixa
        size_t avail = (m_head >= m_tail) ? (m_head - m_tail) : (FIFO_CAP - m_tail + m_head);
        if (avail > 4096) {
            size_t drop = avail - 2048;
            m_tail = (m_tail + drop) % FIFO_CAP;
        }

        for (size_t s = 0; s < framesNeeded; ++s) {
            if (m_head != m_tail) {
                outL[s] = m_fifo[m_tail * 2 + 0];
                outR[s] = m_fifo[m_tail * 2 + 1];
                m_tail = (m_tail + 1) % FIFO_CAP;
            } else {
                outL[s] = 0.0f;
                outR[s] = 0.0f;
            }
        }

        m_rtBusy.clear(std::memory_order_release);
        return true;
    }

private:
    using PFN_ActivateAudioInterfaceAsync = HRESULT(WINAPI*)(
        LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);

    bool InitProcessLoopbackInternal(int32_t mode, uint32_t pid, uint32_t sampleRate) {
        HMODULE hMmdev = GetModuleHandleW(L"mmdevapi.dll");
        if (!hMmdev) hMmdev = LoadLibraryW(L"mmdevapi.dll");
        if (!hMmdev) return false;

        auto pfnActivate = reinterpret_cast<PFN_ActivateAudioInterfaceAsync>(
            GetProcAddress(hMmdev, "ActivateAudioInterfaceAsync"));
        if (!pfnActivate) return false;

        AUDIOCLIENT_ACTIVATION_PARAMS blob{};
        blob.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        blob.ProcessLoopbackParams.TargetProcessId = pid;
        blob.ProcessLoopbackParams.ProcessLoopbackMode = (mode == 1)
            ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
            : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activateParams{};
        PropVariantInit(&activateParams);
        activateParams.vt = VT_BLOB;
        activateParams.blob.cbSize = sizeof(blob);
        activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&blob);

        auto* pHandler = new ProcessLoopbackActivationHandler();
        IActivateAudioInterfaceAsyncOperation* pAsyncOp = nullptr;

        HRESULT hr = pfnActivate(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient),
            &activateParams,
            pHandler,
            &pAsyncOp
        );

        if (SUCCEEDED(hr) && pHandler->Wait(1500)) {
            m_pClient = pHandler->DetachClient();
        }

        if (pAsyncOp) pAsyncOp->Release();
        pHandler->Release();

        if (!m_pClient) return false;

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = sampleRate;
        wfx.wBitsPerSample = 32;
        wfx.nBlockAlign = 8;
        wfx.nAvgBytesPerSec = sampleRate * 8;

        hr = m_pClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            200000, // 20ms buffer
            0,
            &wfx,
            nullptr
        );

        if (FAILED(hr)) {
            ReleaseClientInternal();
            return false;
        }

        hr = m_pClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&m_pCaptureService));
        if (FAILED(hr) || !m_pCaptureService) {
            ReleaseClientInternal();
            return false;
        }

        hr = m_pClient->Start();
        if (FAILED(hr)) {
            ReleaseClientInternal();
            return false;
        }

        return true;
    }

    void ReleaseClientInternal() {
        if (m_pClient) {
            m_pClient->Stop();
        }
        if (m_pCaptureService) {
            m_pCaptureService->Release();
            m_pCaptureService = nullptr;
        }
        if (m_pClient) {
            m_pClient->Release();
            m_pClient = nullptr;
        }
    }

    IAudioClient* m_pClient{ nullptr };
    IAudioCaptureClient* m_pCaptureService{ nullptr };
    std::atomic<bool> m_ready{ false };
    std::atomic_flag m_rtBusy = ATOMIC_FLAG_INIT;

    int32_t m_currentMode{ 0 };
    uint32_t m_currentPid{ 0 };
    uint32_t m_currentSampleRate{ 0 };

    std::array<float, FIFO_CAP * 2> m_fifo{};
    size_t m_head{ 0 };
    size_t m_tail{ 0 };
};

} // namespace UM2Loopback
