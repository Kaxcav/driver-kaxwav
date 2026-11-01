# measure — harness de medição

Implementa `docs/MEASUREMENT-PROTOCOL.md` (M1–M8). Fase 1 do roadmap.

## Um comando
```powershell
cmake --build build --config Release          # driver + ferramentas abaixo
python measure/scripts/run_baseline.py         # tudo, pergunta o setup físico na hora
```
Gera `measure/out/<data>-<commit>/` (brutos, ignorado pelo Git) e
`measure/baselines/<data>-<commit>.json` (**commitado**). Rodar só parte:
`--steps m2,m5`; condições sem prompt: `--cond gain_ch1=12h --cond loopback="..." --yes`.
Cada passo grava `<passo>.json` na pasta da corrida e o baseline é remontado a cada execução,
então dá para medir em etapas (setups físicos diferentes) sem perder nada.
Os valores `DriverMode`/`BufferSize` do registro (que o FL também usa) são restaurados no fim.

| Passo | Mede | Setup físico | Ferramenta |
|---|---|---|---|
| m8 | Descobre provider ETW de áudio e perfis do WPR nesta máquina | — | `logman`, `wpr` |
| m2 | Piso: 60 s shared padrão (APO) e 60 s RAW; dBFS RMS, PSD Welch FFT 65536, degrau em 7 kHz | entradas sem nada ligado; anotar GAIN | `tools/raw_capture_test.exe` + `tools/measure_noise.analyze` |
| m5 | Deadline miss: p50/p99/max da duração do bloco, misses, gaps (telemetria do host) | — | `um2_asio_probe` + `um2_ipc.read_telemetry` |
| m7 | Null test digital do caminho DAW (SharedFlex): saída vs loopback WASAPI, entrada vs captura shared | — | `um2_asio_probe --tap-*` |
| m1 | RTL: 3 chirps por sessão, correlação cruzada, 20 sessões × modo × buffer 64/128/256, vs PDC | loopback saída → entrada (elétrico ou acústico) | `um2_asio_probe` |
| m3 | Resposta (ESS 20 Hz–20 kHz, Farina) + THD+N @ 1 kHz, nos dois modos | loopback **elétrico** | `um2_asio_probe` |
| m6 | Drift entrada/saída vs QPC (1 h), curva por janela de 5 min | — (UM2 fica em exclusivo) | `um2_drift` |

## Ferramentas (`measure/tools`, alvos do CMake)
- `um2_asio_probe` — host ASIO mínimo: carrega a DLL por caminho, toca estímulo int16 nas duas saídas
  numa amostra ASIO fixa e grava as entradas 1–2 na mesma linha de tempo. Opcional: taps WASAPI
  (captura shared e loopback do render da UM2) para o null test.
- `um2_drift` — captura + render da UM2 em exclusive event-driven, `IAudioClock` e posições de
  pacote contra QPC a cada 1 s (CSV), progresso a cada minuto em `<csv>.progress.log`.
- `tools/raw_capture_test` — já existia; reaproveitado como está para o M2.

## Scripts (`measure/scripts`)
- `run_baseline.py` — orquestrador.
- `um2_analysis.py` — estímulos e análises (correlação, PSD, ESS, THD+N, drift, null test).
- `um2_ipc.py` — leitura só-leitura de `UM2SharedState` (layout vindo do `panel_backend.py`) e da
  telemetria M5 (`src/UM2Telemetry.h`).

Dependências Python: numpy, scipy (o `tools/measure_noise.py` como script usa também sounddevice/soundfile).
