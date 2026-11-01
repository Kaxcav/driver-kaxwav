# Síntese dos dossiês do Prompt 1 (v1 + v2) — consenso, contradições e plano
**31/10/2026** — insumo para a skill e para a próxima fase de código.

---

## 1. CONSENSO (os dois dossiês concordam — tratar como decidido)

| # | Conclusão | Consequência prática |
|---|---|---|
| 1 | PCM2902 = UAC1 / USB 1.1 FS / 16-bit / ≤48 kHz, service interval de 1 ms, 192 bytes/frame. Sem modo oculto, sem firmware reprogramável. | Teto físico. Nenhuma camada de software contorna. |
| 2 | Driver kernel-mode próprio (PortCls/WaveRT) = ganho marginal a nulo em UAC1, + cert EV (US$287–685/ano) + attestation/HVCI. | **Arquiteturas 3 e 4 mortas.** Não escrever `.sys`. |
| 3 | ASIO não tem privilégio de kernel sobre WASAPI Exclusive event-driven; delta < 100 µs de jitter. | Manter a camada ASIO **só** pela compatibilidade com DAW. Não é alavanca de latência. |
| 4 | Arquitetura recomendada: host ASIO em userland sobre WASAPI Exclusive + RAW + event-driven. | É o que já existe. O esforço vai para fan-out e DSP, não para transporte. |
| 5 | `AUDCLNT_STREAMOPTIONS_RAW` é a via suportada para matar APO. `IPolicyConfig` é não documentado (explica o 0x800706C5); property store é frágil e reverte em update. | Para de brigar com registro. RAW na inicialização do stream. |
| 6 | ~~Process Loopback substitui o VB-CABLE.~~ **CORRIGIDO:** Process Loopback *captura a saída* de um processo. O Discord precisa de um *microfone* — não aceita processo como entrada. Os dois dossiês erram isso. | Discord continua precisando de cabo virtual; a correção barata é pôr CABLE Input/Output em **48 kHz** (hoje 192 kHz) e matar o resampling em cascata. Captura por processo serve ao OBS. Ver ADR 0003. |
| 7 | Gain staging: nível do endpoint em **0,0 dB** (ganho linear 1.0 — *não* `SetMasterVolumeLevelScalar(1.0)`, que é slider em 100%; ver ADR 0005); todo ganho no knob analógico XENYX, picos em ~-12 a -6 dBFS. | Muda o estado atual (slider em 19% = -15,7 dB joga bits fora). |
| 8 | DSP pesado **nunca** na thread ASIO. Fan-out SPSC lock-free; DSP só na thread secundária. | Confirma a lição do rollback de hoje. |
| 9 | Drift: nunca inserir/descartar sample. ASRC contínuo com r8brain-free-src (MIT) + controlador PI sobre nível de buffer. | Só no ramo da bridge. Caminho da DAW intocado. |
| 10 | Supressão: DeepFilterNet > RNNoise > subtração espectral STFT caseira. Expander RMS *antes*, não gate hard. | A tentativa STFT de hoje estava no algoritmo errado **e** no lugar errado. |
| 11 | MMCSS `AvSetMmThreadCharacteristicsW(L"Pro Audio")` + `AvSetMmThreadPriority`, não `THREAD_PRIORITY_TIME_CRITICAL`. | Verificar o que o código atual faz. |
| 12 | Nunca remover INF por índice (`oemXX.inf`); amarrar por hardware ID `USB\VID_08BB&PID_2902`. | Já corrigido no `.bat`. Confirmado como regra. |

---

## 2. CONTRADIÇÕES ENTRE v1 E v2 (resolver antes de codar)

### C1 — Modo de clock do ADC *(crítico: define todo o desenho de drift)*
- **v1:** DAC = Adaptive (SpAct), **ADC = Asynchronous** → o cristal do hardware é master, host é escravo, não há feedback endpoint do host.
- **v2:** **ADC = Synchronous** → host/SOF é master, drift resolvido pelo PLL do chip.
- Mutuamente exclusivos. Se v1 estiver certo, o ASRC com PI é obrigatório. Se v2 estiver certo, o drift é muito menor e o ASRC pode ser esporádico.
- **Resolver:** datasheet PCM2902 (seção de sincronização) + descritores reais (`bmAttributes` do endpoint isócrono: Async/Adaptive/Sync) + medição empírica: contagem de samples capturados vs QPC por 1 h → ppm.

### C2 — Orçamento de RTL e group delay *(define o critério de aceite)*
- **v1 (sumário):** ~4,19 ms. Group delay com números de datasheet: ADC 17.4/fs (362 µs) + DAC 14.3/fs (298 µs) = **0,66 ms**.
- **v1 (próprio trace de pesquisa):** "7,3 a 10 ms". **Contradiz o próprio sumário.**
- **v2:** ~6,74 ms teórico, group delay **~1,2 ms por estágio** = 2,4 ms, mas **marcado `[ESTIMATIVA]`** pelo próprio relatório. Meta sugerida: 8–12 ms.
- Os números de v1 para group delay (17.4/fs, 14.3/fs) têm cara de valor real de datasheet; os de v2 são admitidamente chute. Mas o RTL total de v1 não fecha com o resto do próprio documento.
- **Impacto:** critério de aceite ≤5 ms (v1) vs ≤12 ms (v2) é diferença de 2,4×. Com o número errado a gente aprova um driver ruim ou persegue o impossível.
- **Resolver:** datasheet (group delay dos filtros de decimação/interpolação) + **medir** RTL real com loopback físico. A medição manda.

### C3 — Tipo de sample do ASIO
- **v1:** rejeitar `ASIOSTInt16LSB`, declarar `ASIOSTInt32LSB`.
- **v2:** manter `ASIOSTInt16LSB`, não adicionar conversão inútil.
- O rollback de hoje voltou para `ASIOSTInt16LSB`. v2 é mais defensável: não há informação a ganhar acima de 16 bits na entrada. Baixo risco, decidir por medição (THD+N e carga de CPU nas duas).

### C4 — Latência algorítmica do DeepFilterNet
- **v1:** janela 20 ms, lookahead 2 frames, **latência final 40 ms**.
- **v2:** **exatamente 10 ms**.
- 4× de diferença. Em VoIP, 40 ms é perceptível mas tolerável; 10 ms é grátis.
- **Resolver:** paper/repo do DeepFilterNet (versão importa: DFN1 vs DFN2 vs DFN3) + medir o build compilado.

### C5 — Qual API abre o stream
- Os dois escrevem, em algum ponto, `IAudioClient3::InitializeSharedAudioStream` combinado com "Exclusive".
- **Isso não existe.** `InitializeSharedAudioStream` é, por definição, modo **compartilhado** (é a via de baixa latência do shared mode). Exclusive mode se abre com `IAudioClient::Initialize` + `AUDCLNT_SHAREMODE_EXCLUSIVE` + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- **Ação:** decidir explicitamente qual caminho queremos (Exclusive puro vs Shared com período mínimo via IAudioClient3) e **não copiar o trecho dos relatórios**.

### C6 — `KSPROPERTY_RTAUDIO_POSITIONREGISTER` no host userland
- Os dois sugerem o host ASIO ler o position register do WaveRT; v1 chega a mostrar struct do WDK.
- **Erro de camada.** Essa é propriedade KS entre engine e driver em kernel. Cliente WASAPI em userland obtém posição via `IAudioCaptureClient::GetBuffer` / `IAudioClock` / `IAudioClock2::GetDevicePosition`.
- **Ação:** ignorar esse trecho nas duas versões.

---

## 3. O QUE FAZER COM ISSO — plano

### Fase 0 — Fechar a verdade (antes de qualquer linha de código)
Resolver C1–C6 e produzir `fatos-verificados-um2.md`: cada fato com fonte primária ou medição, nada herdado de relatório sem conferência. É esse arquivo que vira a ground truth da skill, não os dossiês crus.
- Fonte primária: datasheet PCM2902 (group delay, modo de sync, SNR), descritores USB reais, Microsoft Learn (assinaturas de API), repo DeepFilterNet.
- Medição na máquina: ppm de drift (sample count vs QPC, 1 h), RTL real com loopback.

### Fase 1 — Harness de medição ANTES de mudar o driver
Hoje a gente reverteu por não ter como provar melhora ou piora. Todo critério de aceite dos dois dossiês é mensurável, e não temos o instrumento. Então o instrumento vem primeiro:
- RTL com loopback físico (saída L → entrada), repetível, com desvio padrão.
- Piso de ruído em PSD + dBFS, com o endpoint em 0,0 dB e sem APO.
- Contador de deadline miss / underrun dentro do próprio host (QPC, p50/p99/max).
- Sweep senoidal → THD+N e resposta de banda (detecta o degrau de 7 kHz do Voice Clarity).
- Script único que roda tudo e cospe um relatório comparável entre builds.

### Fase 2 — Os três ganhos baratos (nessa ordem, medindo entre cada um)
1. **Nível do endpoint em 0,0 dB** + ganho no XENYX para picos ~-9 dBFS. (Hoje está em 19% = jogando ~2,6 bits fora.)
2. **RAW + expander RMS como primeiro estágio** — é a resposta certa para o problema de hoje. O erro não foi ligar o RAW; foi ligar o RAW e deixar o OTT do FL levantar o piso sem nada antes dele.
3. **VB-CABLE em 48 kHz** (não em 192 kHz) — mata o resampling em cascata. Process Loopback não substitui o cabo para o Discord (ver item 6 corrigido); serve só ao OBS.

### Fase 3 — Fan-out SPSC + ASRC
Ring buffer SPSC lock-free, zero alocação/lock/log no callback. ASRC r8brain + PI **só** no consumidor. Caminho da DAW intocado. Aceite: 8 h sem glitch com DAW em carga.

### Fase 4 — DeepFilterNet só no ramo de comunicação
Depois de C4 resolvido. Se a latência real for 40 ms e não 10 ms, continua valendo para Discord, mas entra como opção, não como padrão.

### Depois: a skill
A skill **não** se monta com esses dois dossiês. Ela se monta com: `fatos-verificados-um2.md` (Fase 0) + o dossiê do Prompt 2 (campo geral, padrões de código, anti-padrões) + a auditoria desta sessão (o que já falhou de verdade nesta máquina). Os dossiês do Prompt 1 entram como fonte, não como conteúdo — justamente porque C2, C5 e C6 provam que eles vazam erro de API e número inconsistente.
