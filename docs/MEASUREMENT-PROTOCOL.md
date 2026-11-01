# Protocolo de medição

Toda mudança é medida contra o baseline anterior. Uma mudança por vez.

## Condições fixas
- 48 kHz, buffer 64 / 128 / 256.
- Escalar do endpoint de captura = 1.0. Aprimoramentos do Windows desligados.
- Mesmo cabo de loopback, mesma posição de GAIN (anotar), mesmo nível de saída.
- 5 min de aquecimento antes de medir. Mínimo 5 repetições por métrica.

## Métricas

| ID | Métrica | Ferramenta | Tolerância de regressão |
|---|---|---|---|
| M1 | RTL medido (média, desvio padrão) | Loopback físico + RTL Utility / harness próprio | +0,1 ms na média; σ ≤ 0,5 ms |
| M2 | Piso de ruído (dBFS e PSD) | Captura RAW, entrada terminada | +0,5 dB |
| M3 | Resposta em frequência 20 Hz–20 kHz | Sweep logarítmico | ±0,1 dB; sem degrau em 7 kHz |
| M4 | THD+N @ 1 kHz | Sweep / tom | +0,005 pp |
| M5 | Deadline miss / underrun | Contador interno do host (QPC p50/p99/max) | **zero** em 40 min sob carga |
| M6 | Drift do clock | Samples capturados vs QPC por 1 h | registrar ppm (fecha C1) |
| M7 | Bit-perfect | Null test digital do caminho DAW | resíduo = 0 |
| M8 | Glitches do sistema | WPR com provider `Microsoft-Windows-Audio` | zero |

## Registro
Cada rodada gera `measure/baselines/<data>-<commit>.json`. O commit que muda
comportamento de áudio traz no corpo da mensagem o diff das métricas.
