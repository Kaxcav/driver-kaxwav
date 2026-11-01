# KaxWav — driver ASIO para Behringer UM2

Host ASIO em userland para a **Behringer U-PHORIA UM2** (TI PCM2902, USB Audio Class 1.0)
no Windows 11 x64. Latência mínima estável, caminho bit-perfect para a DAW e um ramo
separado com DSP para comunicação (Discord/OBS).

## Arquitetura em uma linha
`PCM2902 → usbaudio.sys → WASAPI Exclusive (RAW, event-driven) → host KaxWav →`
`├─ ASIO → DAW (intocado, sem DSP)`
`└─ fan-out SPSC → ASRC → expander → supressão → cabo virtual 48 kHz → Discord / captura por processo → OBS`

Detalhes em [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Por que **não** há driver
kernel-mode: [`docs/decisions/0001`](docs/decisions/0001-nao-escrever-driver-kernel.md).

## Estrutura
| Pasta | Conteúdo |
|---|---|
| `src/asio/` | Objeto COM e implementação `IASIO` |
| `src/core/` | Cliente WASAPI, ring buffer SPSC, relógio/posição |
| `src/dsp/` | Expander, ASRC (r8brain), adaptadores de supressão |
| `src/bridge/` | Fan-out e Process Loopback para apps de comunicação |
| `src/ipc/` | Protocolo host ↔ painel |
| `src/common/` | Log fora do caminho crítico, telemetria, QPC, erros |
| `panel/` | Painel de controle (backend Python + web) |
| `measure/` | Harness de medição: RTL, piso, THD+N, glitches |
| `tests/` | `test_um2_host` e testes unitários |
| `tools/` | Utilitários de diagnóstico |
| `installer/` | Instalação/desinstalação segura |
| `docs/` | Arquitetura, fatos verificados, protocolo, roadmap, ADRs, pesquisa |
| `third_party/` | Dependências baixadas localmente (ASIO SDK **não** é versionado) |

## Build
1. Visual Studio 2022 (MSVC x64), Release.
2. Baixar o **ASIO SDK** da Steinberg e extrair em `third_party/asio-sdk/`
   (ver `third_party/README.md`). Ele é proprietário e não pode ser redistribuído.
3. O sistema de build é o do código existente, trazido na migração (`MIGRATION.md`).

## Estado
Ver [`docs/ROADMAP.md`](docs/ROADMAP.md). Perguntas técnicas ainda abertas estão em
[`docs/FATOS-VERIFICADOS.md`](docs/FATOS-VERIFICADOS.md).

## Regras do projeto
[`CLAUDE.md`](CLAUDE.md) — invariantes de tempo real, gain staging e instalação que nunca
podem ser violadas.
