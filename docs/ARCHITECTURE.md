# Arquitetura

## Caminho do sample

```
[Analógico] XLR/Hi-Z → XENYX → PCM2902 ADC (16-bit, 48 kHz)
      │  USB 1.1 Full Speed, isócrono, 1 ms/frame
      ▼
[Kernel] xHCI → usbaudio.sys (classe UAC1, da Microsoft — não substituímos)
      ▼
[Userland] Host KaxWav — thread MMCSS "Pro Audio", event-driven
      │  WASAPI Exclusive + AUDCLNT_STREAMOPTIONS_RAW (sem APO)
      │  (API exata de abertura: ver C5 em FATOS-VERIFICADOS.md)
      │
      ├──► Caminho DAW ─────────────────────────────────────────────
      │      bufferSwitchTimeInfo() → FL Studio
      │      SEM DSP. SEM lock. SEM alocação. Bit-perfect.
      │
      └──► Fan-out SPSC lock-free (o callback só copia e retorna)
             ▼
           Thread secundária (prioridade abaixo da crítica)
             1. ASRC r8brain + controlador PI sobre o nível do buffer
             2. Downward expander RMS (antes de qualquer compressão)
             3. Supressão neural (DeepFilterNet) — opcional
             4. Saída para apps de comunicação (Discord/OBS)
```

## Módulos

| Módulo | Responsabilidade | Restrição |
|---|---|---|
| `src/asio` | COM, `IASIO`, double buffer, `getLatencies`, `getSamplePosition` monotônico | Latência reportada tem de bater com a medida |
| `src/core` | Abertura/reabertura do stream WASAPI, ring buffer SPSC, posição/relógio | Trata `DEVICE_INVALIDATED` e `BUFFER_SIZE_NOT_ALIGNED` |
| `src/dsp` | Expander, ASRC, adaptador de supressão | **Nunca** chamado da thread de áudio |
| `src/bridge` | Consumidor do fan-out, entrega para apps de comunicação | Discord via cabo virtual em 48 kHz (ADR 0003) |
| `src/ipc` | Protocolo versionado host ↔ painel | Leitura de estado nunca bloqueia o callback |
| `src/common` | Telemetria de deadline miss, log assíncrono, QPC | Log só fora do caminho crítico |

## Princípio de separação
O caminho da DAW e o caminho de comunicação **não compartilham nada mutável** além do
ring buffer SPSC. Qualquer falha, lentidão ou travamento no ramo de comunicação pode
custar um clique no Discord — nunca um glitch na gravação.
