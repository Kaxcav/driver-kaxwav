# Fatos verificados — UM2 / PCM2902

Regra: um fato só entra aqui com **fonte primária** (datasheet, especificação USB-IF,
Microsoft Learn, código-fonte, publicação oficial) **ou medição nesta máquina**, com data.
Número de dossiê de pesquisa automatizada não conta como fonte.

## Verificado

| Fato | Valor | Fonte / medição | Data |
|---|---|---|---|
| Identificação USB | `VID_08BB&PID_2902` | Gerenciador de dispositivos | 2026-10-31 |
| Interface USB | USB 1.1 Full Speed | Datasheet TI PCM2900/2902 (SLES035E) | 2026-10-31 |
| **Modo de sincronização** (fecha C1) | **Playback: Adaptive. Record: Asynchronous.** | Datasheet TI, *Features*: "USB Adaptive Mode for Playback / USB Asynchronous Mode for Record" | 2026-10-31 |
| Resolução e taxas | 16-bit (e 8-bit). DAC: 32 / 44,1 / 48 kHz. ADC: 8 a 48 kHz | Datasheet TI | 2026-10-31 |
| **Group delay ADC** (fecha parte de C2) | td = 17,4 / fS → **362 µs @ 48 kHz** | Datasheet TI, *ADC digital filter* | 2026-10-31 |
| **Group delay DAC** (fecha parte de C2) | td = 14,3 / fS → **298 µs @ 48 kHz** | Datasheet TI, *DAC digital filter* | 2026-10-31 |
| Group delay total do chip | **0,66 ms** ida e volta @ 48 kHz | Soma das duas linhas acima | 2026-10-31 |
| ADC | SNR 89 dB, dynamic range 89 dB, THD+N 0,01% | Datasheet TI | 2026-10-31 |
| DAC | SNR 96 dB, dynamic range 93 dB, THD+N 0,005% | Datasheet TI | 2026-10-31 |
| Clock de referência | XTI 12 MHz, faixa aceita 11,994–12,008 MHz | Datasheet TI (*Clock frequency*) | 2026-10-31 |
| Piso de ruído, captura RAW | -88,5 dBFS; PSD ≈ -133 dB/Hz até 20 kHz | Medição local | 2026-10-31 |
| APO de captura padrão | Voice Clarity: corte ~7 kHz + supressão | Medição local | 2026-10-31 |
| `IPolicyConfig` para religar aprimoramentos | Falha com `0x800706C5` | Execução local | 2026-10-31 |
| Volume de endpoint em 86% | ≈ +13 dB digital; causa principal do chiado percebido | Medição local | 2026-10-31 |
| **Faixa do volume do endpoint de captura** | **−96 a +30 dB, passo 1,5 dB; 0 dB = slider 0,537** | Medição local: `GetVolumeRange`, `GetMasterVolumeLevel` e `GetMasterVolumeLevelScalar` via COM, nos endpoints de captura que o painel reconhece pelo nome, com o `usbaudio.sys` atual. **Não vem de documentação** | 2026-10-31 |
| **Semântica de `SetMasterVolumeLevelScalar`** | Valor normalizado *audio-tapered* 0.0–1.0, **1.0 = máximo da faixa**. Não é ganho linear | Microsoft Learn, `IAudioEndpointVolume::SetMasterVolumeLevelScalar` | 2026-10-31 |
| **Licença do ASIO SDK** (C7) | **Dual: GPLv3 ou proprietária Steinberg**, desde out/2025 | Comunicado Steinberg 2025-10-29; heise | 2026-10-31 |
| Process Loopback — versão mínima | Windows 10 build 20348 | Microsoft Learn, `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS` | 2026-10-31 |
| Process Loopback — direção | Captura a **saída** de um processo. Não cria microfone virtual | Microsoft Learn (semântica da API) | 2026-10-31 |
| **Latência do DeepFilterNet** (fecha C4) | Janela 20 ms, hop 10 ms, lookahead 2 frames → **40 ms no total** | Paper DeepFilterNet (arXiv 2305.08227), seção 3 | 2026-10-31 |
| Licença do código DeepFilterNet | MIT ou Apache-2.0 (à escolha) | README do repo Rikorose/DeepFilterNet | 2026-10-31 |
| Valores `ASIOError` | `ASE_NotPresent` -1000, `HWMalfunction` -999, `InvalidParameter` -998, `InvalidMode` -997, `SPNotAdvancing` -996, **`NoClock` -995**, `NoMemory` -994 | Enumeração sequencial do `asio.h` (confirmar no SDK local) | 2026-10-31 |

## Consequências do que foi verificado

- **C1 → o ADC é master do próprio clock.** A captura chega no ritmo do cristal da UM2, não
  do host. Qualquer consumidor em outro domínio de clock (cabo virtual, Discord) precisa de
  ASRC contínuo. O ASRC com controlador PI **é obrigatório** no ramo da bridge.
- **C1 → entrada e saída rodam em clocks diferentes (ver C8).**
- **C2 → o chip contribui 0,66 ms.** O dossiê v2 (1,2 ms por estágio) estava errado. O resto
  do RTL é USB (1 ms por sentido) + buffering do `usbaudio.sys` + buffer do host —
  isso só se fecha medindo.
- **Faixa do endpoint → `SetMasterVolumeLevelScalar(1.0)` = +30 dB de ganho digital** depois do ADC
  (o máximo da faixa), pior que os +13 dB dos 86%. A faixa é do driver de classe e pode mudar com
  atualização dele: por isso o código confere `GetVolumeRange` a cada execução em vez de usar número fixo.
- **C4 → DeepFilterNet custa 40 ms.** Serve para o Discord; nunca para monitoramento.
- **C7 → o SDK só poderia ir para o repo num projeto GPLv3.** O produto é comercial e fechado
  (licença PRO), então vale o termo proprietário: o SDK fica fora do repo.

## ABERTO — não escrever código que dependa destes itens

### C2 (resto) — RTL total
- Parte do chip fechada (0,66 ms). O total ainda tem quatro estimativas incompatíveis nos
  dossiês (4,19 / 6,5 / 6,74 / "7,3–10" ms) e nenhuma medição.
- **Como fechar:** M1 com loopback físico, buffers 64/128/256.
- **Bloqueia:** critério de aceite de latência no roadmap.

### C3 — Tipo de sample do ASIO
- `ASIOSTInt16LSB` (estado atual) vs `ASIOSTInt32LSB`.
- **Como fechar:** THD+N e CPU nas duas configurações.

### C5 — API de abertura do stream
- `IAudioClient3::InitializeSharedAudioStream` é **shared mode** por definição.
- Exclusive = `IAudioClient::Initialize` + `AUDCLNT_SHAREMODE_EXCLUSIVE` + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- **Como fechar:** medir os dois caminhos (período mínimo efetivo, RTL, glitches) e escolher.

### C6 — Leitura de posição no host
- `KSPROPERTY_RTAUDIO_POSITIONREGISTER` é propriedade KS entre engine e driver.
- Caminho userland: `IAudioCaptureClient::GetBuffer` (timestamp QPC) / `IAudioClock2::GetDevicePosition`.
- **Como fechar:** registrar a decisão no código.

### C8 — Drift entre entrada e saída do mesmo device *(novo, prioridade alta)*
- Consequência direta de C1: a captura segue o cristal da UM2; o playback (Adaptive) segue o
  ritmo dos pacotes do host. Em sessão duplex longa, a entrada e a saída **derivam entre si**.
- O host ASIO entrega à DAW entrada e saída em passo travado. Se o código atual amarra os dois
  ao evento de um só stream, o outro acumula ou esvazia devagar. A suíte de teste atual
  (8/8) é curta demais para mostrar isso.
- **Como fechar:** M6 estendido — contar samples de captura e de render contra QPC por 1 h
  em duplex, registrar ppm de cada um e do diferencial; observar o nível dos buffers do host.
- **Bloqueia:** desenho do caminho da DAW (pode exigir ASRC ou sample-slip controlado também ali).
