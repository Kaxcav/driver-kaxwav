# Fatos verificados — UM2 / PCM2902

Regra: um fato só entra aqui com **fonte primária** (datasheet, especificação USB-IF,
Microsoft Learn, código-fonte) **ou medição nesta máquina**, com data. Número de dossiê
de pesquisa automatizada não conta como fonte.

## Verificado

| Fato | Valor | Fonte / medição | Data |
|---|---|---|---|
| Identificação USB | `VID_08BB&PID_2902` | Gerenciador de dispositivos | 2026-10-31 |
| Piso de ruído, captura RAW | -88,5 dBFS; PSD ≈ -133 dB/Hz até 20 kHz | Medição local | 2026-10-31 |
| APO de captura padrão | Voice Clarity: corte ~7 kHz + supressão | Medição local | 2026-10-31 |
| `IPolicyConfig` para religar aprimoramentos | Falha com `0x800706C5` | Execução local | 2026-10-31 |
| Volume de endpoint em 86% | ≈ +13 dB digital; causa principal do chiado percebido | Medição local | 2026-10-31 |

## ABERTO — não escrever código que dependa destes itens

### C1 — Modo de sincronização do ADC
- Dossiê v1: ADC assíncrono (cristal do chip é master). Dossiê v2: síncrono (host é master).
- **Como fechar:** `bmAttributes` do endpoint isócrono de captura (descritor real) +
  seção de sincronização do datasheet + medição de drift (samples vs QPC, 1 h → ppm).
- **Bloqueia:** desenho do ASRC (contínuo vs esporádico).

### C2 — Group delay dos filtros e RTL mínimo
- v1: ADC 17.4/fs + DAC 14.3/fs = 0,66 ms; RTL ~4,19 ms (mas o próprio trace diz 7,3–10 ms).
- v2: ~1,2 ms por estágio `[ESTIMATIVA]`; RTL ~6,74 ms; meta 8–12 ms.
- **Como fechar:** datasheet do PCM2902 + RTL medido com loopback físico.
- **Bloqueia:** critério de aceite de latência no roadmap.

### C3 — Tipo de sample do ASIO
- `ASIOSTInt16LSB` (estado atual) vs `ASIOSTInt32LSB`.
- **Como fechar:** THD+N e CPU nas duas configurações.

### C4 — Latência algorítmica do DeepFilterNet
- v1: 40 ms. v2: 10 ms.
- **Como fechar:** paper/repo da versão exata (DFN1/2/3) + medição do build compilado.

### C5 — API de abertura do stream
- `IAudioClient3::InitializeSharedAudioStream` é **shared mode** por definição.
- Exclusive = `IAudioClient::Initialize` + `AUDCLNT_SHAREMODE_EXCLUSIVE` + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- **Como fechar:** decidir qual caminho, medir os dois (período mínimo efetivo, RTL, glitches).

### C6 — Leitura de posição no host
- `KSPROPERTY_RTAUDIO_POSITIONREGISTER` é propriedade KS de kernel, não acessível ao cliente WASAPI.
- Caminho userland: `IAudioCaptureClient::GetBuffer` (timestamp QPC) / `IAudioClock2::GetDevicePosition`.
- **Como fechar:** confirmar em Microsoft Learn e registrar a decisão.
