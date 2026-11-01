# MEGA PROMPT — Gemini Deep Research
## Tema: engenharia de drivers de áudio no Windows para construir o melhor driver possível para a Behringer UM2 (PCM2902 / USB Audio Class 1.0)

---

## 0. PAPEL E MISSÃO

Você é um **engenheiro de áudio de sistemas** sênior: kernel-mode Windows (WDM/KS/AVStream/PortCls), USB (UAC1/UAC2, transferências isócronas), ASIO SDK, WASAPI/IAudioClient3, APOs, DSP de tempo real e medição eletroacústica. Não escreva para leigos. Não repita conceitos básicos.

**Missão:** produzir um **dossiê técnico de engenharia** que me permita decidir a arquitetura e implementar, do zero ou evoluindo o que já existe, o **melhor driver de áudio tecnicamente possível** para a interface **Behringer U-PHORIA UM2** no Windows 10/11 x64 — com foco em **latência mínima estável, piso de ruído mínimo, integridade bit-perfect, estabilidade de clock e compatibilidade (ASIO + WASAPI + WDM)**.

Trate como pesquisa de engenharia, não como tutorial. Toda afirmação de números (latência, SNR, tamanho de pacote, períodos mínimos) precisa de **fonte** ou da marca explícita `[ESTIMATIVA]`.

---

## 1. CONTEXTO VERIFICADO (ponto de partida real, não hipotético)

Hardware e ambiente já instrumentados por mim:

- **Dispositivo:** Behringer U-PHORIA UM2. USB enumera como `USB\VID_08BB&PID_2902` → silício **TI/Burr-Brown PCM2902** (CODEC USB Audio Class 1.0, 16-bit, estéreo, até 48 kHz). Preamp **XENYX** no canal 1 (mic, XLR), canal 2 instrumento Hi-Z. Monitoramento direto analógico via chave MONITOR.
- **SO:** Windows 11 x64. Driver de transporte atual: classe `usbaudio.sys` (UAC1) — a Behringer distribui driver proprietário (base **Ploytec**) para as UMC, mas a UM2 opera class-compliant.
- **Estado do meu projeto:** existe um driver/host ASIO próprio ("KaxWav") que faz ponte entre ASIO e WASAPI, com uma *bridge* DLL, IPC para um painel de controle e injeção em endpoint virtual (hoje **VB-Audio CABLE** como destino para Discord/OBS).
- **Medições já feitas no hardware real:**
  - Captura **RAW** (`AUDCLNT_STREAMOPTIONS_RAW`, sem APO): ruído branco essencialmente plano em **-88,5 dBFS**, PSD ≈ -133 dB/Hz até 20 kHz. Isso é coerente com o piso XENYX + ADC do PCM2902 (~89 dB SNR de datasheet).
  - Captura **shared** padrão passava pelo APO de "Voice Clarity"/aprimoramentos: corte de banda em ~7 kHz e supressão agressiva (silêncio zerado). "Limpava" o sinal, mas destruía banda.
  - O chiado percebido era dominado por **ganho digital pós-ADC**: o painel forçava o volume do endpoint de captura em 86% (≈ +13 dB). Com o slider em ~19% (-15,7 dB) o problema desaparece. Regra empírica derivada: **nunca acima de 54% (= 0 dB)**.
  - Tentativa de redutor espectral STFT na thread ASIO (N=256) foi revertida: artefatos + risco de deadline. Lição registrada: **redução espectral pesada só fora do caminho ASIO**.
  - Suíte de testes do host ASIO passa 8/8 em SharedFlex e ExclusivePro (buffers 64/144/256/1024); em ExclusivePro @64 há fallback transparente para caminho compartilhado (PDC 352/608).

**Pesquise assumindo esse ponto de partida.** Onde meu diagnóstico puder estar errado ou incompleto, **me contradiga com evidência**.

---

## 2. PREMISSAS A VALIDAR OU DERRUBAR (faça isso antes de qualquer recomendação)

Para cada item: confirme, refute ou qualifique, **com fonte**.

1. A UM2 é **irredutivelmente 16-bit / 44,1–48 kHz / UAC1**? Existe modo não documentado no PCM2902 (ou no firmware da UM2) para 24-bit ou taxas maiores? O que dizem os descritores USB reais (`lsusb -v` / USB Device Tree Viewer / `snd-usb-audio` no Linux)?
2. O PCM2902 usa clock **adaptive** ou **asynchronous**? Existe endpoint de feedback explícito? Em UAC1 adaptive, quem é o master de clock e como o drift é resolvido na prática?
3. Qual o **limite físico de latência** de UAC1: service interval de 1 ms (full-speed frame), tamanho de pacote, latência de grupo do ADC/DAC sigma-delta do PCM2902, buffering interno do chip. Monte o **orçamento de latência** completo (analógico → ADC → USB → kernel → ASIO → app → kernel → USB → DAC → analógico).
4. Em um device UAC1 fixo em 16-bit, **existe ganho real** em usar ASIO em vez de WASAPI Exclusive event-driven? Quantifique em µs e em jitter, não em marketing.
5. O piso de -88,5 dBFS é mesmo o limite do conjunto XENYX+PCM2902, ou há margem (alimentação USB, layout, ground loop, impedância da fonte, ruído do bus)? O que mudaria com booster inline (Cloudlifter/FetHead) para dinâmico?
6. É verdade que **todo** ganho digital pós-ADC em 16 bits eleva o piso de ruído de forma irrecuperável, e que a única solução correta é ganho analógico + headroom? Qual o gain staging ótimo para 16-bit/89 dB SNR?

---

## 3. EIXOS DE PESQUISA

Cubra **todos** os blocos. Em cada um: estado da arte, APIs/arquivos/estruturas concretas, armadilhas conhecidas, números, fontes.

### A. Camada de transporte USB no Windows
- `usbaudio.sys` (UAC1) vs `usbaudio2.sys` (UAC2): capacidades, limites de latência, modos suportados, chaves de registro/propriedades tunáveis, diferenças Win10 vs Win11 (incluindo melhorias de low-latency anunciadas no Windows 11 e o suporte a **ASIO class-compliant** no driver USB Audio 2.0 da Microsoft — isso cobre UAC1? Em que builds?).
- Escrever um **driver USB isócrono próprio**: caminhos possíveis — (a) KS/AVStream + PortCls/WaveRT substituindo `usbaudio.sys`, (b) userland com **WinUSB** (suporte isócrono desde Win8.1: `WinUsb_RegisterIsochBuffer`, `WinUsb_WriteIsochPipe`, `WinUsb_ReadIsochPipeAsap`), (c) **libusbK** / UsbDk / WinUSB-raw, (d) UMDF2. Para cada: viabilidade, jitter real, overhead de transição, perda de pacotes, requisitos de assinatura, precedentes de produto.
- Como o driver captura o device exclusivamente (desbind do driver de classe), convivência com `oemXX.inf`, driver store, INF de substituição de classe, riscos de desinstalar INF de terceiros por índice (erro que já cometi).
- Agendamento isócrono: tamanho de pacote, `URB_FUNCTION_ISOCH_TRANSFER`, número de frames por URB, pipelining, profundidade de fila vs latência vs robustez a DPC storms.

### B. Modelos de driver de áudio no Windows
- **PortCls + WaveRT** (streaming de buffer compartilhado, `KSPROPERTY_RTAUDIO_*`, notificação por evento, packet-based WaveRT no Win10+): como atingir período de 128 ou 64 samples.
- **AVStream vs PortCls**, miniports, `IPortWaveRTStream`, DMA-less, `KSPROPERTY_RTAUDIO_PACKETCOUNT`.
- **Universal/DCH**: requisitos, separação driver ↔ Hardware Support App, componentes INF, por que co-installers morreram, MSIX para o painel.
- **SysVAD** (Virtual Audio Device Driver sample) como base para substituir o VB-CABLE por um **endpoint virtual próprio**: o que o sample já entrega, o que falta (loopback, multi-client, APO), licença.
- Assinatura e distribuição: **attestation signing** vs **WHQL/HLK**, certificado EV, Partner Center, custos, prazos, o que é obrigatório para kernel-mode em Win11 com Secure Boot/HVCI, e as implicações de HVCI para o código do driver.

### C. ASIO — fazer certo
- Licenciamento do **ASIO SDK** (Steinberg): termos atuais, o que pode ser distribuído, alternativas (ASIO2 interfaces reimplementadas?).
- Contrato correto do host/driver: `bufferSwitchTimeInfo`, double buffering, `ASIOTimeInfo` (samplePosition, systemTime), `kAsioResetRequest`/`kAsioBufferSizeChange`/`kAsioLatenciesChanged`, reporte honesto de `getLatencies()` (input/output), `getSamplePosition()` monotônico.
- Formatos: `ASIOSTInt16LSB` vs `ASIOSTInt32LSB` (e `Int24LSB`) para um device 16-bit — conversão, dither, onde o 32-bit só adiciona custo.
- **Thread de áudio em tempo real:** `AvSetMmThreadCharacteristics("Pro Audio")`, MMCSS, `AvSetMmThreadPriority`, por que `THREAD_PRIORITY_TIME_CRITICAL` puro é pior, timer resolution (`NtSetTimerResolution` / `timeBeginPeriod` e a mudança de comportamento no Win10 2004+), core parking, affinity, Spectre/retpoline no caminho crítico.
- **Fontes abertas para estudar:** FlexASIO (MIT, dechamps), ASIO2WASAPI, PortAudio (host APIs WASAPI/WDMKS/ASIO), JACK2 no Windows, miniaudio, BASSASIO, `ASIO4ALL` (fechado — o que se sabe por engenharia reversa/documentação). O que cada um acerta e erra em: clock, resampling, multi-client, relato de latência.
- **Multi-client ASIO:** como entregar o device a FL Studio e simultaneamente a Discord/OBS sem destruir latência (arquitetura de fan-out, ring buffer lock-free, SPSC, cross-thread sem alocação, sem lock no callback).

### D. WASAPI / caminho compartilhado
- `IAudioClient3`: `GetSharedModeEnginePeriod`, `GetCurrentSharedModeEnginePeriod`, `InitializeSharedAudioStream` — períodos mínimos reais por device, pré-requisitos no driver para expor períodos pequenos, por que a maioria dos devices fica em 10 ms.
- **Exclusive mode** event-driven: alinhamento de buffer, `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`, `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, `AUDCLNT_STREAMOPTIONS_RAW` e `MATCH_FORMAT`.
- **APOs e "aprimoramentos"** no Win11: cadeia SFX/MFX/EFX, `PKEY_FX_*`, Voice Clarity / Communications APO, Studio Effects (NPU), **como detectar e desabilitar programaticamente e por política** — e por que `IPolicyConfig` é não documentado e falha (no meu caso `hr 0x800706C5`). Qual a via **suportada** (property store do endpoint, `DisableSystemEffects`, `AUDCLNT_STREAMOPTIONS_RAW`, PowerShell/registry, política por app).
- Loopback capture, `AUDCLNT_STREAMFLAGS_LOOPBACK`, e **process loopback** (`ActivateAudioInterfaceAsync` com `AUDIOCLIENT_ACTIVATION_PARAMS` / `PROCESS_LOOPBACK`) — captura por processo sem cabo virtual.
- Volume de endpoint: onde exatamente o escalar é aplicado (hardware vs engine vs APO), por que o slider de captura em 86% soma ruído, qual propriedade ler/gravar (`IAudioEndpointVolume` vs `ISimpleAudioVolume` vs channel volume do KS) e se existe ganho de hardware real no PCM2902.

### E. Clock, drift e resampling
- Natureza do drift em UAC1 adaptive; magnitude típica em ppm; como medir (contagem de samples vs QPC ao longo de minutos).
- Controle de drift: estimador PI sobre nível de buffer, ajuste contínuo de ratio, vs inserção/descarte de samples (ruim), vs ASRC contínuo.
- Qualidade de SRC: **r8brain-free-src**, **soxr**, libsamplerate, speex resampler — THD+N, latência, custo de CPU, licença. Qual escolher para ASRC de drift (ratio ~1.00001) e qual para conversão de taxa (48k↔44,1k, e 192k dos endpoints CABLE).
- Como evitar **resampling em cascata** (device 48k → CABLE 192k → app 48k) e por que isso é um gerador silencioso de degradação.

### F. DSP: ruído, gate e gain staging
- Metodologia correta de **medição de piso de ruído**: PSD em dB/Hz vs dBFS banda larga, janelas, A-weighting, por que -88,5 dBFS pode ser excelente e ainda soar ruidoso depois de compressão/OTT.
- Por que o chiado "aparece" no FL Studio: ganho de make-up, multiband upward compression (OTT), expansão de range dinâmico do ruído. Ordem correta de cadeia (expander/gate **antes** de compressão) e parâmetros de partida para voz.
- Design de **gate/expander** de qualidade: detecção RMS vs peak, lookahead, histerese, release programa-dependente, downward expansion suave (ratio 1.5–2:1) em vez de gate hard.
- **Supressão neural**: RNNoise, DeepFilterNet (1/2/3), NSNet2/DNS Challenge baselines, Krisp/NVIDIA Broadcast (proprietário). Para cada: licença, latência algorítmica, custo de CPU, qualidade em voz com chiado branco, viabilidade de rodar **na bridge** (Discord/OBS) e **jamais** na thread ASIO.
- Subtração espectral STFT: por que gerou artefato no meu caso, hop/janela mínimos viáveis, musical noise, alternativas (Wiener, MMSE-LSA, spectral floor).
- **Dither e truncamento** em 16-bit: TPDF vs noise shaping, onde aplicar (apenas na saída final para 16-bit), por que não aplicar em cada estágio.
- DC offset, remoção de hum 60 Hz/120 Hz (Brasil), ruído de bus USB, ground loop — mitigação em software vs hardware.

### G. Medição e validação (protocolo objetivo)
- **Latência round-trip real**: RTL Utility (Oblique Audio), loopback físico, método de pulso, por que o número reportado pelo driver mente. Como instrumentar no meu próprio driver.
- **Qualidade**: RMAA (legado), REW, ARTA, Multitone Analyzer — sweep senoidal, THD+N, IMD, SNR, crosstalk, resposta de frequência, teste de bit-perfect (null test digital).
- **Estabilidade**: LatencyMon, `xperf`/WPR com provider **Microsoft-Windows-Audio**, contadores de glitch, `AudioGlitchDetection`, detecção de underrun/overrun no próprio driver, logging de deadline miss por callback.
- **HLK**: quais testes de áudio existem, o que é obrigatório para certificação, o que é útil mesmo sem certificar.
- Defina um **protocolo de regressão reproduzível**: lista de medições, condições, tolerâncias, formato de relatório — para eu rodar antes/depois de cada mudança e nunca mais depender de "achei que soou melhor".

### H. Benchmark competitivo
- **Ploytec USB ASIO** (base dos drivers Behringer/UMC): arquitetura conhecida, latências publicadas, por que é referência.
- **Steinberg Generic Low Latency ASIO**, **ASIO4ALL**, **FlexASIO**, drivers Focusrite/RME/MOTU/Universal Audio: números reais de RTL em 48 kHz com buffer 64/128/256, e o que no design explica a diferença.
- Para a **UM2 especificamente**: quais latências a comunidade reporta (fóruns, Reddit r/audioengineering, Gearspace, KVR, forum Behringer/Music Tribe), com que driver, e qual o melhor resultado conhecido. Qual é o **alvo realista** que meu driver deve perseguir e a partir de que ponto eu estaria brigando com a física do UAC1.

### I. Referências de outras plataformas (para roubar boas ideias)
- **Linux `snd-usb-audio`**: parsing de descritores, `implicit_fb`, quirks table — a fonte mais honesta sobre o que o hardware realmente faz. Procure se a UM2/PCM2902 tem quirk registrado.
- **macOS CoreAudio/AudioServerPlugIn**: por que latência class-compliant é melhor lá, o que o modelo de driver faz de diferente, o que é portável em conceito.
- **Pipewire/JACK**: modelo de grafo, quantum, ganho de usar um grafo em vez de ponto-a-ponto.

### J. Decisão de arquitetura
Monte uma **matriz de decisão** comparando, para o meu caso concreto (UM2, Win11, uso: produção no FL Studio + voz no Discord/OBS, dev solo, sem orçamento de certificação WHQL):

1. **Host ASIO em userland sobre WASAPI Exclusive/RAW** (o que tenho hoje).
2. **Host ASIO em userland falando USB direto** (WinUSB/libusbK isócrono, bypass do `usbaudio.sys`).
3. **Driver kernel-mode PortCls/WaveRT próprio** + camada ASIO por cima.
4. **Driver kernel-mode + endpoint virtual próprio (SysVAD-derived)** substituindo o VB-CABLE.
5. **Não escrever driver**: configurar corretamente o caminho existente (RAW + exclusive + gain staging + gate) e investir o esforço em DSP e ferramentas.

Eixos da matriz: latência mínima alcançável, jitter, estabilidade, esforço (homem-semana), risco de bricar o SO do usuário, exigência de assinatura/custo, manutenibilidade em updates do Windows, multi-client, qualidade sonora real ganha. **Dê uma recomendação única e defenda-a**, inclusive se a recomendação for "não escreva um driver kernel".

---

## 4. FONTES PRIORITÁRIAS

Prefira, nesta ordem: documentação oficial da Microsoft (Windows Driver Kit, docs de Audio Drivers, WASAPI, WinUSB isoch), especificações **USB Audio Class 1.0/2.0** (usb.org), **datasheet do TI PCM2902** (e PCM290x family), ASIO SDK docs da Steinberg, código-fonte aberto (FlexASIO, PortAudio, `snd-usb-audio`, SysVAD, WDK samples), notas de release de fabricantes (Ploytec, RME, Focusrite), papers de DSP (supressão de ruído, ASRC) e, por último, fóruns técnicos qualificados (Gearspace, KVR, MSDN/StackOverflow com resposta verificável).

Desconfie e sinalize: blogs de SEO, "best ASIO driver 2026", listas genéricas, conteúdo gerado sem medição.

---

## 5. ENTREGÁVEIS E FORMATO DE SAÍDA

Produza **um relatório único, longo, em PT-BR com termos técnicos em inglês**, nesta estrutura:

1. **Sumário executivo** — 10 bullets com as conclusões que mudam decisão.
2. **Verdade sobre o hardware** — descritores, limites, orçamento de latência com números e fontes.
3. **Checagem das minhas 6 premissas** (seção 2) — tabela: premissa / veredito / evidência / fonte.
4. **Mapa de arquiteturas de driver no Windows** — diagrama em texto do caminho do sample, de analógico a aplicação, em cada arquitetura candidata.
5. **Blocos A–I** — cada um com: estado da arte, APIs/estruturas concretas com nomes exatos, armadilhas, números, fontes.
6. **Matriz de decisão** (bloco J) + **recomendação defendida**.
7. **Roadmap em 3 fases** — para cada fase: escopo, critérios de aceite **mensuráveis**, protocolo de medição, risco, esforço estimado.
8. **Protocolo de medição de regressão** — pronto para eu automatizar (ferramentas, comandos, métricas, tolerâncias).
9. **Receita de gain staging e cadeia de DSP** — valores iniciais concretos para voz no meu setup (XENYX + 16-bit), e o que deve ficar na bridge vs no caminho ASIO.
10. **Tabela de licenças e custos** — ASIO SDK, SRC libs, supressores neurais, certificado EV, HLK, Partner Center.
11. **Armadilhas que já me pegaram ou vão pegar** — lista com mitigação (incluindo: remover INF de terceiros por índice, volume de endpoint, APO de Voice Clarity, resampling em cascata, DSP na thread ASIO, deadline miss sob DPC storm).
12. **Lacunas de conhecimento** — o que a pesquisa não conseguiu determinar e qual experimento meu resolveria.
13. **Bibliografia** — links completos, agrupados por bloco.

**Regras de qualidade:**
- Nunca invente nome de API, flag, GUID, propriedade de registro ou número de página de datasheet. Se não tiver certeza, escreva `[NÃO VERIFICADO]`.
- Separe sempre **fato com fonte** / **consenso de engenharia** / `[ESTIMATIVA]`.
- Quando houver código, use C/C++ (Win32/WDK) ou C++ do ASIO SDK, com os tipos e assinaturas reais.
- Não me ofereça caminhos "seguros e genéricos". Quero o limite técnico e o custo honesto de chegar nele.
- Se a conclusão correta for que **meu esforço está no lugar errado**, diga isso explicitamente e mostre onde o ganho real está.
