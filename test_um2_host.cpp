#include <windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include "src/UM2Ipc.h"
#include "src/UM2Dsp.h"
#include <cmath>

// CLSID: {77DBBCCD-91C6-4856-A900-FC8D007D161C}
static const GUID CLSID_BehringerUM2AsioDriver = 
    { 0x77dbbccd, 0x91c6, 0x4856, { 0xa9, 0x00, 0xfc, 0x8d, 0x00, 0x7d, 0x16, 0x1c } };

static const GUID IID_IASIO = 
    { 0x933923e0, 0x01fe, 0x11d2, { 0xbc, 0x7b, 0x00, 0x80, 0xc7, 0x79, 0xb4, 0x3e } };

static long s_bufferSwitchCount = 0;
static long s_timeInfoCount = 0;
static bool s_timeInfoValid = false;

void BufferSwitch(long doubleBufferIndex, ASIOBool directProcess) {
    InterlockedIncrement(&s_bufferSwitchCount);
}

void SampleRateChanged(ASIOSampleRate sRate) {}
long AsioMessage(long selector, long value, void* message, double* opt) {
    if (selector == kAsioSupportsTimeInfo) {
        return 1;
    }
    if (selector == kAsioResetRequest) {
        std::cout << "\n>>> [HOST CALLBACK] kAsioResetRequest RECEBIDO DO DRIVER COM SUCESSO!\n";
    }
    return 0;
}
ASIOTime* BufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess) {
    InterlockedIncrement(&s_bufferSwitchCount);
    InterlockedIncrement(&s_timeInfoCount);
    if (params && (params->timeInfo.flags & kSystemTimeValid) && (params->timeInfo.flags & kSamplePositionValid)) {
        s_timeInfoValid = true;
    }
    return nullptr;
}

static ASIOCallbacks s_callbacks = {
    BufferSwitch,
    SampleRateChanged,
    AsioMessage,
    BufferSwitchTimeInfo
};

static void SetDriverRegistryConfig(int mode, long bufferSize) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD dwMode = static_cast<DWORD>(mode);
        DWORD dwSize = static_cast<DWORD>(bufferSize);
        RegSetValueExW(hKey, L"DriverMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwMode), sizeof(dwMode));
        RegSetValueExW(hKey, L"BufferSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwSize), sizeof(dwSize));
        RegCloseKey(hKey);
    }
}

static bool RunAsioTest(HMODULE hDll, long testBuffer, int mode) {
    SetDriverRegistryConfig(mode, testBuffer);

    std::cout << "\n============================================================\n";
    std::cout << "TESTE MODO: " << (mode == 1 ? "SharedFlex (1)" : "ExclusivePro (0)") 
              << " | BUFFER: " << testBuffer << " SAMPLES\n";
    std::cout << "============================================================\n";

    typedef HRESULT (STDAPICALLTYPE *pfnDllGetClassObject)(REFCLSID, REFIID, LPVOID*);
    auto DllGetClassObject = (pfnDllGetClassObject)GetProcAddress(hDll, "DllGetClassObject");
    if (!DllGetClassObject) {
        std::cout << "[FALHA] GetProcAddress(DllGetClassObject) falhou\n";
        return false;
    }

    IClassFactory* pFactory = nullptr;
    HRESULT hr = DllGetClassObject(CLSID_BehringerUM2AsioDriver, IID_IClassFactory, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        std::cout << "[FALHA] DllGetClassObject falhou: 0x" << std::hex << hr << "\n";
        return false;
    }

    IASIO* pAsio = nullptr;
    hr = pFactory->CreateInstance(nullptr, IID_IASIO, (void**)&pAsio);
    pFactory->Release();
    if (FAILED(hr) || !pAsio) {
        std::cout << "[FALHA] CreateInstance falhou: 0x" << std::hex << hr << "\n";
        return false;
    }

    if (!pAsio->init(nullptr)) {
        std::cout << "[FALHA] pAsio->init() retornou FALSE\n";
        pAsio->Release();
        return false;
    }

    char driverName[64]{};
    pAsio->getDriverName(driverName);
    long drvVer = pAsio->getDriverVersion();
    std::cout << "[OK] Driver Name: " << driverName << " (v" << drvVer << ")\n";

    long numInputs = 0, numOutputs = 0;
    pAsio->getChannels(&numInputs, &numOutputs);
    std::cout << "[OK] Canais: " << numInputs << " In / " << numOutputs << " Out\n";

    const long totalChannels = numInputs + numOutputs;
    std::vector<ASIOBufferInfo> bufferInfos(totalChannels);
    for (long i = 0; i < numInputs; ++i) {
        bufferInfos[i].isInput = ASIOTrue;
        bufferInfos[i].channelNum = i;
    }
    for (long i = 0; i < numOutputs; ++i) {
        bufferInfos[numInputs + i].isInput = ASIOFalse;
        bufferInfos[numInputs + i].channelNum = i;
    }

    hr = pAsio->createBuffers(bufferInfos.data(), totalChannels, testBuffer, &s_callbacks);
    if (hr != ASE_OK) {
        std::cout << "[FALHA] createBuffers(" << testBuffer << ") falhou: " << hr << "\n";
        pAsio->Release();
        return false;
    }
    long inLat = 0, outLat = 0;
    pAsio->getLatencies(&inLat, &outLat);
    std::cout << "[OK] Buffers alocados (" << totalChannels << " ch) | Latencia PDC reportada: In="
              << inLat << " smp, Out=" << outLat << " smp\n";

    hr = pAsio->start();
    if (hr != ASE_OK) {
        std::cout << "[FALHA] pAsio->start() falhou: " << hr << "\n";
        pAsio->disposeBuffers();
        pAsio->Release();
        return false;
    }
    std::cout << "[OK] Engine rodando! Monitorando callbacks por 1.5s...\n";

    s_bufferSwitchCount = 0;
    s_timeInfoCount = 0;
    s_timeInfoValid = false;
    auto startT = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    auto endT = std::chrono::steady_clock::now();

    long callbacks = s_bufferSwitchCount;
    long timeInfos = s_timeInfoCount;
    double elapsedSec = std::chrono::duration<double>(endT - startT).count();
    double expected = (48000.0 / testBuffer) * elapsedSec;

    std::cout << "-> Callbacks: " << callbacks << " (TimeInfo: " << timeInfos << " valid=" << (s_timeInfoValid ? "SIM" : "NAO") << ") em " << elapsedSec << "s ("
              << (callbacks / elapsedSec) << " Hz | Esperado: ~" << (48000.0 / testBuffer) << " Hz)\n";

    bool pass = (callbacks >= (expected * 0.80)) && (timeInfos > 0) && s_timeInfoValid;
    std::cout << "-> STATUS: " << (pass ? "APROVADO (OK)" : "REPROVADO (Taxa baixa)") << "\n";

    pAsio->stop();
    pAsio->disposeBuffers();
    pAsio->Release();
    return pass;
}

static bool RunDspUnitTests() {
    std::cout << "\n============================================================\n";
    std::cout << "SUITE DE TESTES DSP v3.0 & IPC LAYOUT\n";
    std::cout << "============================================================\n";
    std::cout << "[INFO] sizeof(UM2SharedState) = " << sizeof(UM2SharedState) << " bytes\n";

    const float sr = 48000.0f;
    ChromaticPitchTuner tuner;
    tuner.Setup(sr);

    // 1. Teste Afinador 440 Hz (A4 = MIDI 69)
    for (int n = 0; n < 4800; ++n) {
        float s = 0.5f * std::sin(2.0f * 3.14159265358979323846f * 440.0f * static_cast<float>(n) / sr);
        tuner.PushSample(s);
    }
    float f440 = tuner.GetFrequencyHz();
    int32_t m440 = tuner.GetMidiNote();
    float c440 = tuner.GetCents();
    bool ok440 = (m440 == 69) && (std::abs(f440 - 440.0f) < 2.5f) && (std::abs(c440) < 10.0f);
    std::cout << (ok440 ? "[OK]" : "[FALHA]") << " Tuner 440Hz -> Freq=" << f440 << "Hz, MIDI=" << m440 << ", Cents=" << c440 << "\n";

    // 2. Teste Detector de 808 Sub 55 Hz (A1 = MIDI 33)
    tuner.Setup(sr);
    for (int n = 0; n < 9600; ++n) {
        float s = 0.6f * std::sin(2.0f * 3.14159265358979323846f * 55.0f * static_cast<float>(n) / sr);
        tuner.PushSample(s);
    }
    float f55 = tuner.GetFrequencyHz();
    int32_t m55 = tuner.GetMidiNote();
    float c55 = tuner.GetCents();
    bool ok55 = (m55 == 33) && (std::abs(f55 - 55.0f) < 1.5f) && (std::abs(c55) < 15.0f);
    std::cout << (ok55 ? "[OK]" : "[FALHA]") << " Detector 808 55Hz -> Freq=" << f55 << "Hz, MIDI=" << m55 << ", Cents=" << c55 << "\n";

    // 3. Teste Unison Preamp + FET Compressor + De-Esser + Cab 4x12 + Master Monitor
    AnalogPreampProcessor preamp;
    StudioFetCompressor comp;
    StudioDeEsser deess;
    GuitarCabSimulator cab;
    MasterMonitorProcessor mon;
    AdcClipGuard clip;

    preamp.Setup(sr);
    comp.Setup(sr);
    deess.Setup(sr);
    cab.Setup(sr);
    mon.Setup(sr);
    clip.Setup(sr);

    bool allFinite = true;
    for (int n = 0; n < 4800; ++n) {
        // Sinal misto: vocal 220Hz + sibilância 7200Hz
        float s = 0.55f * std::sin(2.0f * 3.14159265f * 220.0f * n / sr)
                + 0.35f * std::sin(2.0f * 3.14159265f * 7200.0f * n / sr);
        float p73 = preamp.Process(s, 1);
        float dOut = deess.Process(p73, 0.8f);
        float cOut = comp.Process(dOut, 0.8f);
        float cabOut = cab.Process(cOut, true);
        float rOut = cOut;
        mon.ProcessStereo(cabOut, rOut, true, true, true);
        if (!std::isfinite(cabOut) || !std::isfinite(rOut)) {
            allFinite = false;
            break;
        }
    }
    bool compActive = comp.GetGainReductionDb() > 0.5f;
    bool deessActive = deess.GetGainReductionDb() > 0.2f;
    std::cout << (allFinite && compActive && deessActive ? "[OK]" : "[FALHA]")
              << " Unison Chain Finite=" << (allFinite ? "SIM" : "NAO")
              << " | Comp GR=" << comp.GetGainReductionDb() << " dB"
              << " | DeEsser GR=" << deess.GetGainReductionDb() << " dB\n";

    // 4. Teste AdcClipGuard (3 amostras consecutivas >= 0.999f)
    clip.CheckSample(0.5f);
    clip.CheckSample(0.9995f);
    clip.CheckSample(0.9995f);
    bool clipBefore3 = clip.IsClipping();
    clip.CheckSample(0.9995f);
    bool clipAt3 = clip.IsClipping();
    bool okClip = (!clipBefore3 && clipAt3);
    std::cout << (okClip ? "[OK]" : "[FALHA]") << " AdcClipGuard: pre-3=" << clipBefore3 << ", post-3=" << clipAt3 << "\n";

    return ok440 && ok55 && allFinite && compActive && deessActive && okClip;
}

int main(int argc, char* argv[]) {
    CoInitialize(nullptr);

    bool dspOk = RunDspUnitTests();
    if (argc >= 2 && std::string(argv[1]) == "--dsp-only") {
        CoUninitialize();
        return dspOk ? 0 : 1;
    }

    // Load the driver built next to this executable (build/Release), not a fixed path.
    wchar_t dllPath[MAX_PATH] = {};
    DWORD pathLen = GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH) {
        std::cout << "[FALHA] GetModuleFileNameW falhou. Erro: " << GetLastError() << "\n";
        return 1;
    }
    if (wchar_t* slash = wcsrchr(dllPath, L'\\')) slash[1] = L'\0';
    wcscat_s(dllPath, L"BehringerUM2ASIODriver.dll");
    std::wcout << L"[INFO] Driver: " << dllPath << L"\n";
    HMODULE hDll = LoadLibraryW(dllPath);
    if (!hDll) {
        std::cout << "[FALHA] Nao foi possivel carregar BehringerUM2ASIODriver.dll. Erro: " << GetLastError() << "\n";
        return 1;
    }

    if (argc >= 3) {
        long buf = std::atol(argv[1]);
        int mode = std::atoi(argv[2]);
        bool ok = RunAsioTest(hDll, buf, mode);
        FreeLibrary(hDll);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    std::vector<std::pair<int, long>> tests = {
        { 1, 64 },
        { 1, 144 },
        { 1, 256 },
        { 1, 1024 },
        { 0, 64 },
        { 0, 144 },
        { 0, 256 },
        { 0, 1024 }
    };

    int passed = 0;
    for (const auto& t : tests) {
        if (RunAsioTest(hDll, t.second, t.first)) {
            passed++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n============================================================\n";
    std::cout << "RESUMO: " << passed << " / " << tests.size() << " TESTES PASSARAM\n";
    std::cout << "============================================================\n";

    FreeLibrary(hDll);
    CoUninitialize();
    return (passed == static_cast<int>(tests.size())) ? 0 : 2;
}
