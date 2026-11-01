#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include "UM2License.h"
#include "UM2Dsp.h"
#include "UM2Loopback.h"
#include "UM2Telemetry.h"
#include "UM2Diag.h"

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

extern const GUID CLSID_BehringerUM2AsioDriver;
extern const GUID IID_IASIO;

class BehringerUM2AsioDriver final : public IASIO {
public:
    BehringerUM2AsioDriver();
    virtual ~BehringerUM2AsioDriver();

    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    virtual ULONG STDMETHODCALLTYPE AddRef() override;
    virtual ULONG STDMETHODCALLTYPE Release() override;

    // IASIO Core Interface
    virtual ASIOBool init(void* sysHandle) override;
    virtual void getDriverName(char* name) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char* string) override;
    virtual ASIOError start() override;
    virtual ASIOError stop() override;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    virtual ASIOError setClockSource(long reference) override;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    virtual ASIOError disposeBuffers() override;
    virtual ASIOError controlPanel() override;
    virtual ASIOError future(long selector, void* opt) override;
    virtual ASIOError outputReady() override;

private:
    void AudioThreadLoop();
    bool FindBehringerEndpoints();
    long QueryExclusiveMinBufferFrames();
    void ReleaseEndpoints();

private:
    std::atomic<LONG> m_refCount{ 1 };
    void* m_sysHandle{ nullptr };

    // PCM2902: 2 canais fisicos In + 2 canais virtuais Loopback = 4
    static constexpr long NUM_INPUTS = 4;
    static constexpr long NUM_OUTPUTS = 2;
    static constexpr double DEFAULT_SAMPLE_RATE = 48000.0;
    static constexpr long DEFAULT_BUFFER_SIZE = 128; // Padrao rapido e estavel

    double m_sampleRate{ DEFAULT_SAMPLE_RATE };
    double m_hwSampleRate{ DEFAULT_SAMPLE_RATE };
    long m_bufferSize{ DEFAULT_BUFFER_SIZE };
    long m_preferredBufferSize{ DEFAULT_BUFFER_SIZE };
    long m_hwBufferSize{ DEFAULT_BUFFER_SIZE };
    bool m_active{ false };
    bool m_buffersCreated{ false };
    bool m_isSharedModeActive{ false };
    bool m_supportsTimeInfo{ false };
    LARGE_INTEGER m_qpcFreq{};
    UINT32 m_totalRenderFrames{ 0 };
    UINT32 m_exclusiveRenderFrames{ 0 };
    UINT32 m_exclusiveCaptureFrames{ 0 };

public:
    enum class DriverEngineMode : uint32_t {
        ExclusivePro = 0,
        SharedFlex = 1
    };

private:
    DriverEngineMode m_driverMode{ DriverEngineMode::SharedFlex };

    void LoadSavedSettings();
    void SaveCurrentSettings();

    ASIOCallbacks* m_callbacks{ nullptr };
    std::vector<ASIOBufferInfo> m_bufferInfos;
    std::vector<std::vector<int16_t>> m_channelMemory;

    std::thread m_audioThread;
    std::atomic<bool> m_stopThread{ false };
    HANDLE m_hRenderEvent{ nullptr };
    HANDLE m_hCaptureEvent{ nullptr };
    int64_t m_samplePosition{ 0 };

    // Hardware Endpoints e Audio Clients
    IMMDevice* m_pRenderDevice{ nullptr };
    IMMDevice* m_pCaptureDevice{ nullptr };
    IAudioClient* m_pRenderClient{ nullptr };
    IAudioClient* m_pCaptureClient{ nullptr };
    IAudioRenderClient* m_pRenderService{ nullptr };
    IAudioCaptureClient* m_pCaptureService{ nullptr };

    // Saída Discord / Cabo Virtual (VB-Audio Virtual Cable)
    IMMDevice* m_pCableDevice{ nullptr };
    IAudioClient* m_pCableClient{ nullptr };
    IAudioRenderClient* m_pCableRenderService{ nullptr };
    UINT32 m_cableChannels{ 2 };
    float m_lastCableL{ 0.0f };
    float m_lastCableR{ 0.0f };
    std::vector<float> m_cableIn0;
    std::vector<float> m_cableIn1;
    StudioNoiseGate m_cableGateCh2;
    void InitCableEndpoint();
    void ReleaseCableEndpoint();
    void FeedCableVirtual(long frames, bool loopBroadcast, float loopGain, float gateThresh, const float* pIn0, const float* pIn1, const int16_t* pOut0, const int16_t* pOut1);

    // Behringer UM2 Shared Memory IPC
    HANDLE m_hSharedMem{ nullptr };
    struct UM2SharedState* m_pSharedState{ nullptr };

    // M5 telemetry (separate shared section, see UM2Telemetry.h)
    HANDLE m_hTelemetry{ nullptr };
    UM2Telemetry* m_pTelemetry{ nullptr };
    void OpenTelemetry();
    void CloseTelemetry();

    // Engine-loop diagnostics (UM2Diag.h): off unless DiagCounters=1, decided in start()
    HANDLE m_hDiag{ nullptr };
    UM2Diag* m_pDiag{ nullptr };
    bool m_diagEnabled{ false };
    void OpenDiag();
    void CloseDiag();
    void InitSharedMemory();
    void CloseSharedMemory();

    // Hot-reload e IPC Watcher Thread (sincronizacao em tempo real com DAW)
    std::thread m_ipcThread;
    std::atomic<bool> m_stopIpcThread{ false };
    std::atomic<bool> m_handlingReload{ false };
    void IpcWatcherLoop();
    void CheckAndHandleReloadRequest();

    // Detecção e Formato de Endpoint
    bool m_isPhysicalUM2{ false };
    int32_t m_detectedHardwareModel{ 0 }; // 0 = UM2 (XENYX), 1 = UMC22 (MIDAS)
    int32_t m_pdcOffsetSamples{ 0 };
    UINT32 m_captureChannels{ 2 };
    bool m_captureIsFloat{ true };
    UINT32 m_enginePeriodFrames{ 480 };

    // Motor DSP v3.0 Zero-Allocation (64-bit Core)
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
    ChromaticPitchTuner m_tuner;
    MasterMonitorProcessor m_masterMon;

    // Captura de Loopback por Processo (Windows 10/11 Process Loopback API)
    UM2Loopback::ProcessLoopbackCapturer m_processLoopback;
    std::vector<float> m_loopBuf0;
    std::vector<float> m_loopBuf1;

    void SetupAllDsp(float sr);
    void UpdateLatencyTelemetry();

    // Hot-Reconnect de USB e Otimização de Telemetria
    std::atomic<bool> m_deviceInvalidated{ false };

    // ADR 0004: DSP no caminho da DAW e opt-in por canal (padrao: captura ASIO bit-perfect).
    // Lido do registro (HKCU\Software\BehringerUM2\ASIO DawDspCh1/DawDspCh2) fora da thread de audio.
    std::atomic<bool> m_dawDspCh1{ false };
    std::atomic<bool> m_dawDspCh2{ false };
    void ReadDawDspFlags();
    uint32_t m_scopeSubsample{ 0 };

    // Sistema de Licenciamento & Modo Demo Anti-Pirataria
    bool m_isLicensed{ false };
    std::string m_licenseTier{ "DEMO" };
    uint64_t m_demoSamplesCount{ 0 };
    void CheckLicenseStatus();
};
