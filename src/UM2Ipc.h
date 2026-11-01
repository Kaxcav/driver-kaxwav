#pragma once
#include <cstdint>

// Magic: "UM20" (0x554D3230)
constexpr uint32_t UM2_IPC_MAGIC = 0x554D3230;
constexpr wchar_t UM2_SHARED_MEM_NAME[] = L"Local\\BehringerUM2SharedMemory";

#pragma pack(push, 1)
struct UM2SharedState {
    uint32_t magic;                 // UM2_IPC_MAGIC (0x554D3230)
    uint32_t version;               // 3 (Fase 2: Licenciamento & HWID)
    uint32_t isStreaming;           // 0 = parado, 1 = streaming ativo
    uint32_t sampleRate;            // Ex: 48000
    uint32_t bufferSize;            // Buffer ativo no driver (ex: 64, 128, etc)
    uint32_t inputChannels;         // 4 (2 fisicos + 2 loopback)
    uint32_t outputChannels;        // 2 (Master L/R)
    uint64_t bufferSwitchCount;     // Contador total de callbacks entregues a DAW
    uint32_t xruns;                 // Contador acumulado de XRUNs (descontinuidades / glitches)
    uint32_t isPhysicalHardware;    // 1 = Conectado ao chip TI PCM2902, 0 = Fallback Windows

    // Telemetria de Picos Lineares (0.0f a 1.0f)
    float peakIn1;                  // Mic/Line 1 (XLR)
    float peakIn2;                  // Inst 2 (Hi-Z P10)
    float peakLoopbackL;            // Loopback Virtual L
    float peakLoopbackR;            // Loopback Virtual R
    float peakOut1;                 // Master Out L
    float peakOut2;                 // Master Out R

    // Controles de Hardware & Roteamento
    uint32_t loopbackEnabled;       // 1 = ativado, 0 = desativado
    float loopbackGain;             // 1.0f = ganho unitario (0.0 dB)
    uint32_t softwareMute;          // 1 = mutado no driver, 0 = normal

    // Sincronizacao em Tempo Real com a DAW (Hot-Reload)
    uint32_t requestedBufferSize;   // Novo buffer solicitado pelo painel
    uint32_t reloadFlag;            // 1 = painel solicitou reload -> driver dispara kAsioResetRequest
    uint32_t driverStatus;          // 0 = fechado, 1 = standby, 2 = streaming
    uint32_t driverMode;            // 0 = Exclusive Pro, 1 = Shared Flex
    uint32_t requestedDriverMode;   // Novo modo solicitado

    // --- DSP EM TEMPO REAL: CANAL 1 (MIC/LINE 1) ---
    int32_t ch1Phase;               // 1 = Fase invertida (amostra * -1), 0 = normal
    int32_t ch1Hpf;                 // 1 = Filtro Passa-Altas 80Hz ativo, 0 = bypass
    int32_t ch1Mute;                // 1 = Mute canal 1, 0 = ativo
    float   ch1Trim;                // Ganho digital linear (0.0f a 4.0f, padrao 1.0f = 0.0dB)

    // --- DSP EM TEMPO REAL: CANAL 2 (INST 2 HI-Z) ---
    int32_t ch2Phase;               // 1 = Fase invertida, 0 = normal
    int32_t ch2Pad;                 // 1 = Atenuador PAD -10dB ativo (0.3162f), 0 = 0dB (1.0f)
    int32_t ch2Mute;                // 1 = Mute canal 2, 0 = ativo
    float   ch2Trim;                // Ganho digital linear (0.0f a 4.0f, padrao 1.0f = 0.0dB)

    // --- DSP EM TEMPO REAL: CANAL 3-4 (LOOPBACK) ---
    int32_t loopbackMono;           // 1 = Somar L+R em Mono, 0 = Estereo

    // --- DSP EM TEMPO REAL: SAIDA MASTER (OUT 1-2) ---
    int32_t masterDim;              // 1 = DIM -14dB ativado (0.1995f), 0 = 0dB
    int32_t masterMono;             // 1 = Somar saida para Mono (L+R)/2, 0 = Estereo
    int32_t masterMute;             // 1 = Mute master, 0 = ativo
    float   masterTrim;             // Ganho master digital linear (0.0f a 2.0f, padrao 1.0f)

    // --- OSCILOSCOPIO & ANALISADOR RTA EM TEMPO REAL ---
    uint32_t scopeSource;           // 0 = Master, 1 = Ch1, 2 = Ch2, 3 = Loopback
    float    scopeBuffer[128];      // 128 amostras reais de sinal (-1.0f a +1.0f)

    // --- SISTEMA DE LICENCIAMENTO & ANTI-PIRATARIA HWID ---
    uint32_t isLicensed;            // 0 = Nao Licenciado (Modo Demo), 1 = Licenca Pro Ativa
    uint32_t licenseDaysLeft;       // 0xFFFFFFFF = Vitalicio, ou dias restantes
    char     licenseTier[16];       // "PRO_LIFETIME", "DEMO", etc.
    char     machineHwid[40];       // HWID da maquina formatado (ex: "UM2-B8F1-49A2-E34C")

    // --- NOISE GATE DE ESTUDIO & BROADCAST MIX ---
    uint32_t ch1Gate;               // 1 = Noise Gate Ch1 Ativo, 0 = Off
    uint32_t ch2Gate;               // 1 = Noise Gate Ch2 Ativo, 0 = Off
    float    gateThresholdDb;       // Limiar do Gate em dBFS (padrao -52.0f dB)
    uint32_t loopbackBroadcastMix;  // 0 = PC Audio Only, 1 = Broadcast Mix: PC + CH1 + CH2

    // --- UNDERGROUND STUDIO CONSOLE v3.0: ANALOG PREAMP & CHANNEL STRIP ---
    int32_t  ch1PreampMode;         // 0 = Clean, 1 = 73 Trap (Neve Warm + 10.5kHz Air), 2 = Tube Grit
    int32_t  ch2PreampMode;         // 0 = Clean, 1 = 73 Trap, 2 = Tube Grit
    float    ch1CompAmount;         // 0.0f a 1.0f (Compressor 1176 FET One-Knob)
    float    ch2CompAmount;         // 0.0f a 1.0f
    float    ch1DeesserAmount;      // 0.0f a 1.0f (De-Esser Dinamico 6.5kHz)
    int32_t  ch2CabSim;             // 0 = Off, 1 = 4x12 / Bass Cabinet Sim no Ch2 Hi-Z
    int32_t  dspMonitorOnly;        // 0 = Print FX na DAW, 1 = FX apenas no Direct Monitor (DAW recebe Dry)

    // --- MONITOR ROOM & MIX CHECK (SAIDA MASTER POS-DAW) ---
    int32_t  monSubCheck;           // 1 = Sub/808 Check (<120Hz Low-Pass), 0 = Off
    int32_t  monPhoneSim;           // 1 = Phone Translation Sim (300Hz-4.2kHz), 0 = Off
    int32_t  monCrossfeed;          // 1 = Acoustic Headphone Crossfeed, 0 = Off

    // --- SAMPLER PER-APP LOOPBACK (WINDOWS PROCESS LOOPBACK) ---
    int32_t  loopbackMode;          // 0 = System Mix, 1 = Isolate App (Include PID), 2 = Exclude App (Exclude PID)
    uint32_t loopbackTargetPid;     // PID do processo alvo (ex: Chrome, Spotify, FL64)

    // --- TELEMETRIA DE ESTUDIO, CLIP FISICO DO CONVERSOR & AFINADOR CROMATICO ---
    int32_t  ch1HwClip;             // 1 = Conversor fisico int16 bateu em +-32760 (saturacao no knob fisico)
    int32_t  ch2HwClip;             // 1 = Conversor fisico int16 bateu em +-32760
    float    ch1GainReductionDb;    // Reducao de ganho atual do Compressor Ch1 em dB (0.0f a 24.0f)
    float    ch2GainReductionDb;    // Reducao de ganho atual do Compressor Ch2 em dB
    int32_t  tunerSource;           // 0 = Off, 1 = Ch1 (Mic/Vocal), 2 = Ch2 (Inst Hi-Z), 3 = Loopback (808/Sample Key)
    float    tunerFreqHz;           // Frequencia fundamental detectada em Hz (ex: 440.0f)
    int32_t  tunerMidiNote;         // Nota MIDI mais proxima (0..127, ex: 69 = A4)
    float    tunerCents;            // Desvio de afinacao em cents (-50.0f a +50.0f)
    float    dspCpuLoadPercent;     // Carga real de CPU do callback de audio (0.0f a 100.0f)
    int32_t  pdcOffsetSamples;      // Ajuste fino de compensacao de latencia p/ DAW (-256 a +256 samples)
    int32_t  inputLatencySamples;   // Latencia real de entrada reportada a DAW (samples)
    int32_t  outputLatencySamples;  // Latencia real de saida reportada a DAW (samples)
    int32_t  hardwareModel;         // 0 = Behringer UM2 (Xenyx), 1 = Behringer UMC22 (Midas)
};
#pragma pack(pop)
