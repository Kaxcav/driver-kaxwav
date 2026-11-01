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

## Estado do código — C5 e C6 (leitura de `src/AsioDriver.cpp` no commit `a7764e6`)

Só leitura: nada abaixo foi corrigido nesta rodada. Números de linha valem para esse commit.
Evidência de execução: `driver_um2.log` da suíte 8/8 de 31/10 e `um2_drift` (período mínimo do device).

### C5 — que API abre o stream em cada modo

| Modo | Render | Captura |
|---|---|---|
| **ExclusivePro** (`createBuffers`, 893–1058) | `IAudioClient::Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hwDuration, hwDuration, PCM 16-bit 2 ch)` (915–922), retry de `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` (925–941). `hwDuration` = buffer ASIO / fs (910) | Mesmo `Initialize` exclusive (963–970); fallback mono em `UNSUPPORTED_FORMAT` (973–990); retry de alinhamento (993–1009). Se ainda falhar, **captura shared** com o mix format (1020–1046, "fallback hibrido") |
| **SharedFlex** (1060–1215) | Se o mix format já é 48 kHz: `IAudioClient3::GetSharedModeEnginePeriod` + `InitializeSharedAudioStream(EVENTCALLBACK, minPeriod)` (1077–1097) — **shared mode**, como deve ser. Senão, ou se falhar: `IAudioClient::Initialize(SHARED, EVENTCALLBACK \| AUTOCONVERTPCM \| SRC_DEFAULT_QUALITY)` (1106–1145) | `IAudioClient::Initialize(SHARED, EVENTCALLBACK \| AUTOCONVERTPCM \| SRC_DEFAULT_QUALITY, mix format)` (1180–1186), retry sem `SRC_DEFAULT_QUALITY` (1187–1204) |

- **Nenhum stream usa `AUDCLNT_STREAMOPTIONS_RAW`** (não há `SetClientProperties` no arquivo). Em
  SharedFlex — e no "fallback hibrido" de captura do ExclusivePro — a captura da DAW passa pelos APOs
  do endpoint. Contraria a invariante 6 do `CLAUDE.md`.
- `InitializeSharedAudioStream` nesta UM2 devolve período de **480 frames (10 ms)**: o device não
  oferece período shared menor (log: "targetPeriod=480 frames" em todos os buffers).
- **Por que ExclusivePro @64 cai para o caminho compartilhado:** 64 / 48000 = 1,333 ms, abaixo do
  período mínimo exclusive do `usbaudio.sys` para este device, **3,0 ms** (`GetDevicePeriod` →
  `min_100ns = 30000` em captura e render, medido pelo `um2_drift`). O `Initialize` do render volta
  `0x88890020` = `AUDCLNT_E_INVALID_DEVICE_PERIOD` (log 31/10 23:30:59.954). O código só trata
  `BUFFER_SIZE_NOT_ALIGNED`; qualquer outra falha deixa `m_pRenderService` nulo e cai inteiro em
  SharedFlex (1053–1060), com a mensagem enganosa "bloqueado por outro processo" (1054). O mesmo
  vale para qualquer buffer < 144 (128 = 2,67 ms). A DAW recebe o PDC do SharedFlex (608 @64), sem
  aviso de que o modo pedido não está ativo.
- Modelo de PDC (`UpdateLatencyTelemetry`, 86–110): SharedFlex in = out = período do engine (480)
  + 2 × buffer; ExclusivePro in = frames de hardware da captura + buffer, out = frames de hardware do
  render + buffer. É modelo, não medição (errata E6) — o M1 compara com o RTL medido.

### C6 — de onde vêm posição e timestamp

- **Nenhuma leitura de posição do device.** Não há `IAudioClock`/`IAudioClock2::GetDevicePosition` no
  arquivo, e todos os `IAudioCaptureClient::GetBuffer` passam `nullptr` em `pu64DevicePosition` e
  `pu64QPCPosition` (1633, 2000, 2090, 2096): o timestamp QPC que o WASAPI anexa a cada pacote é
  descartado. `KSPROPERTY_RTAUDIO_POSITIONREGISTER` também não aparece (bom: é propriedade de kernel).
- **`samplePosition`** é um contador de blocos produzidos: zerado em `start()` (1442) e somado de
  `m_bufferSize` ao fim de cada bloco (1940). Não segue o device: o ritmo dos blocos é ditado pelo
  render (SharedFlex: padding alvo = período do engine + 2 × buffer, 2021–2025; ExclusivePro: um
  período de render por evento, 2110–2111), e a captura entra por um FIFO que **absorve a diferença
  descartando a amostra mais antiga** quando enche (1558/1568/1579) ou **sintetizando amostras com
  decaimento 0,95** quando esvazia (1651, `xruns++` por amostra em 1655). É o sample-slip que o C8
  pergunta se acontece; o M6 mede o tamanho do problema.
- **`systemTime` do `bufferSwitchTimeInfo`** (1770–1780): QPC lido logo antes de chamar a DAW
  (`qpcBeforeDaw`), convertido em ns por `double(qpc) * 1e9 / double(freq)`. É o instante do
  despacho do callback, não o instante em que a primeira amostra do bloco foi capturada ou será
  tocada. Como os blocos saem em rajada (vários `processOneAsioBlock` seguidos por evento de render),
  blocos consecutivos podem ter `systemTime` quase igual com `samplePosition` avançando um buffer
  inteiro. Flags: `kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid` (1774).
- **`getSamplePosition`** (801–811): devolve o mesmo contador com `steady_clock::now()` — o "agora"
  da chamada, não o instante em que aquela posição valeu. O par posição/tempo não é consistente.
- Time info no sentido certo: `asioMessage(kAsioSupportsTimeInfo)` decide entre `bufferSwitchTimeInfo`
  e `bufferSwitch` (862); `future(kAsioCanTimeInfo)` → `ASE_SUCCESS` (1413). O seletor inexistente da
  errata E18 não aparece.

### Errata conferida no código atual (só registro)

| Errata | Situação no código |
|---|---|
| E5 (QPC→ns estoura `uint64`) | **Não ocorre.** A conversão é em `double` (1771): sem overflow; perde resolução de 1 ns só depois de 2^53 ns ≈ 104 dias de uptime |
| E2 / invariante 5 (TIME_CRITICAL) | **Ocorre, pior que na errata:** `AvSetMmThreadCharacteristicsW(L"Pro Audio")` e, em seguida, `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)` incondicional (1507–1508); retorno do MMCSS não é checado e não há `AvSetMmThreadPriority` |
| E6 (`getLatencies` arbitrário) | Parcial: modelo por modo (86–110), não calibrado por RTL medido |
| Invariante 6 (RAW) | Captura shared sem RAW (1180–1186, 1032–1036) |
| Invariante 1 (custo variável no callback) | `processOneAsioBlock` roda o DSP do canal inteiro (HPF, gate, preamp, de-esser, compressor, cab sim, master) e grava telemetria do painel na mesma thread — dentro do mesmo bloco cronometrado pelo M5 |
| E3/E4 (process loopback) | `PropVariantClear` no blob: não ocorre (`UM2Loopback.h` 255–270). Timeout da ativação é tratado (`Wait(1500)`). O handler declara `IAgileObject` mas o `QueryInterface` não o expõe |
| Caminho não bit-perfect | `Quantize16` aplica soft-clip e dither TPDF em toda amostra não nula e escala por 32767 (`UM2Dsp.h` 33–46); o preamp "CLEAN" (modo 0) ainda passa por 3 filtros e ganho de 1,12 (`UM2Dsp.h` 453–458). O M7 mede o resíduo |

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
- **Estado do código documentado** (seção "Estado do código — C5 e C6" acima). A escolha continua aberta.

### C6 — Leitura de posição no host
- `KSPROPERTY_RTAUDIO_POSITIONREGISTER` é propriedade KS entre engine e driver.
- Caminho userland: `IAudioCaptureClient::GetBuffer` (timestamp QPC) / `IAudioClock2::GetDevicePosition`.
- **Como fechar:** registrar a decisão no código.
- **Estado do código documentado** acima: hoje não há leitura de posição do device; `samplePosition` é contador de blocos.

### C8 — Drift entre entrada e saída do mesmo device *(novo, prioridade alta)*
- Consequência direta de C1: a captura segue o cristal da UM2; o playback (Adaptive) segue o
  ritmo dos pacotes do host. Em sessão duplex longa, a entrada e a saída **derivam entre si**.
- O host ASIO entrega à DAW entrada e saída em passo travado. Se o código atual amarra os dois
  ao evento de um só stream, o outro acumula ou esvazia devagar. A suíte de teste atual
  (8/8) é curta demais para mostrar isso.
- **Como fechar:** M6 estendido — contar samples de captura e de render contra QPC por 1 h
  em duplex, registrar ppm de cada um e do diferencial; observar o nível dos buffers do host.
- **Bloqueia:** desenho do caminho da DAW (pode exigir ASRC ou sample-slip controlado também ali).
