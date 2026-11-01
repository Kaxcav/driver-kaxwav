# MEGA PROMPT 2 — Gemini Deep Research
## Tema: estado da arte, evolução e fronteira dos drivers de áudio — base de conhecimento para uma SKILL de engenharia de driver

---

## 0. QUEM CONSOME ESTA PESQUISA (leia antes de tudo)

O leitor **não é humano**. O destino desta pesquisa é virar uma **skill** (base de conhecimento estruturada) carregada por um agente de codificação (Claude Code) que vai **escrever, compilar, instalar, medir e iterar** um driver de áudio real para Windows x64.

Isso muda o formato de saída de forma não-negociável:

- **Regras determinísticas, não conselhos.** "Use `AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)` antes do primeiro callback; se retornar 0, logue `GetLastError()` e caia para prioridade normal" — não "é importante priorizar a thread de áudio".
- **Assinaturas exatas.** Nome real de API, tipos, headers, lib de link, versão mínima do Windows, HRESULTs de falha esperados. Nome errado = código que não compila.
- **Árvores de decisão explícitas.** "Se o device é UAC1 → X. Se UAC2 com feedback endpoint → Y. Se `IAudioClient3::GetSharedModeEnginePeriod` retorna min > 10 ms → Z."
- **Padrões copiáveis.** Blocos de código mínimos, corretos, com tratamento de erro, não pseudocódigo.
- **Checklists verificáveis** com o comando/medição que prova cada item.
- **Anti-padrões nomeados** com o sintoma observável de cada um.

Zero introdução, zero "o áudio digital é fascinante", zero recapitulação de conceito que um engenheiro de sistemas já tem.

**Complementaridade:** existe um outro relatório em curso, específico sobre a Behringer UM2 / PCM2902 / UAC1 (decisão de arquitetura para aquele hardware). **Não repita esse escopo.** Aqui o escopo é o **campo inteiro**: como drivers de áudio funcionam, como chegaram onde estão, o que mudou nos últimos 3 anos, o que está na fronteira, e o que disso é reaproveitável como conhecimento operacional. Onde algo se aplicar a um device USB class-compliant de 16-bit, marque `[APLICA-SE A UAC1 16-BIT]` mas não aprofunde o caso.

---

## 1. MISSÃO

Produzir a **base de conhecimento completa de engenharia de drivers de áudio** — histórica, atual e de fronteira — organizada para uso operacional por agente, cobrindo:

1. **Como funciona** — todas as arquiteturas de driver de áudio relevantes, o caminho do sample em cada uma, e os contratos de API.
2. **Como chegou aqui** — a evolução, e principalmente **por que** cada geração substituiu a anterior (que problema resolveu). Isso importa porque revela os trade-offs permanentes.
3. **O que é novo** — tudo que mudou de 2023 até hoje (final de 2026) e que um engenheiro que parou em 2020 não sabe.
4. **Onde está a fronteira** — pesquisa, protótipos, ideias ainda não comercializadas, e o que é tecnicamente possível mas ninguém fez.
5. **Como medir e provar** — porque sem medição um driver é fé.

---

## 2. EIXOS DE PESQUISA

### A. Anatomia e taxonomia — o caminho do sample
Para **cada** arquitetura abaixo: diagrama em texto do caminho completo do sample (transdutor → aplicação e volta), onde está cada buffer, quem é o relógio, onde o sample pode ser alterado, qual a latência estrutural, e o contrato de API que o driver precisa cumprir.

- Windows: **MME/WaveOut** → **DirectSound** → **WDM/KS (WaveCyclic, WavePci)** → **WaveRT** → **packet-based WaveRT** (Win10+) → **AudioEngine + APO pipeline** → **WASAPI (shared / exclusive / RAW / offload)** → **IAudioClient3 low-latency** → **AudioGraph/WinRT**.
- **ASIO** como by-pass histórico: por que existiu, o que ele realmente contorna hoje vs em 1997, e o que sobrou de justificativa técnica.
- **PortCls vs AVStream**: quando cada um, e por que áudio praticamente só usa PortCls.
- **APOs**: SFX/MFX/EFX, GFX legado, ordem de cadeia, modos (Raw/Default/Movie/Communications/Media/Speech/Notification), `PKEY_FX_*`, APO loading, APO em processo de áudio protegido, **APO v3 / "Audio Effects Discovery"** e o controle por aplicação.
- Fora do Windows, para contraste estrutural: **CoreAudio + AudioServerPlugIn + DriverKit/AudioDriverKit** (macOS), **ALSA + PipeWire/JACK** (Linux), **AAudio/Oboe + MMAP exclusive** (Android), **WASAPI vs CoreAudio vs AAudio** em latência estrutural. O que cada plataforma acertou que o Windows não.

### B. Evolução — e o porquê de cada salto
Linha do tempo com a **causa técnica** de cada transição, não só a data:

- Por que WaveCyclic morreu (cópias de buffer, DPC, latência).
- O que WaveRT mudou (buffer compartilhado kernel↔user, DMA-less, notificação por evento) e por que isso foi o salto maior.
- Por que packet-based WaveRT foi necessário depois de WaveRT.
- Por que a Microsoft criou o AudioEngine e o pipeline de APO — e o preço disso em latência.
- Por que `IAudioClient3` existe e por que, na prática, **tão poucos devices expõem períodos pequenos**.
- **Universal/DCH**: o que forçou a separação driver/app, morte dos co-installers, driver packages declarativos.
- **Secure Boot, HVCI, VBS, driver blocklist, assinatura de kernel**: como isso encolheu o espaço de projeto para drivers de áudio de terceiros nos últimos anos.
- A reação do mercado: por que RME/Focusrite/MOTU/UA ainda escrevem driver proprietário, e por que a Apple matou kexts.

### C. O que é novo (2023 → final de 2026) — prioridade máxima de pesquisa
Investigue e confirme com fonte o estado **atual** de cada item. Marque `[CONFIRMADO + build/versão]`, `[ANUNCIADO, não verificado]` ou `[NÃO ENCONTRADO]`:

- **ASIO class-compliant no driver USB Audio 2.0 da Microsoft** — existe? em que build? cobre UAC1? qual a latência? substitui driver de terceiro?
- Melhorias de **low-latency audio no Windows 11** (incl. mudanças no scheduler, MMCSS, timer coalescing, e o que 24H2/25H2 trouxe para áudio).
- **Windows MIDI Services** e **MIDI 2.0 / UMP** — arquitetura nova, driver model, relevância para quem escreve driver de áudio.
- **Bluetooth LE Audio, LC3, Auracast** no Windows: pilha, offload, latência, impacto no modelo de endpoint.
- **Studio Effects / NPU-accelerated audio effects** (Windows Studio Effects, voice focus, background blur de áudio): onde roda, qual API expõe, como um driver convive ou desvia.
- **Voice Clarity** no Win11 — qual APO, qual escopo, como é ativado por padrão, e o controle programático **suportado**.
- **Rust em drivers Windows** (`windows-drivers-rs`, WDF em Rust): maturidade real, suporte oficial, viabilidade para um driver de áudio hoje.
- **UAC3 (USB Audio Device Class 3.0)**: adoção real, suporte no Windows, por que praticamente não existe em hardware.
- **USB4/Thunderbolt** e áudio: muda algo no agendamento isócrono?
- **Áudio em rede**: AES67, Dante, AVB/Milan, NDI audio, e drivers virtuais de rede no Windows. O que mudou e qual a latência real alcançável.
- **Áudio no WSL2 / containers / VMs** — passthrough, RemoteFX, latência.
- **Spatial audio / Dolby Atmos / Sonic**: como se encaixa no pipeline de APO e o que isso impõe ao driver.
- Mudanças em **HLK / certificação / Partner Center / attestation signing** nos últimos 3 anos (custos, prazos, requisitos novos).
- Qualquer **deprecação** anunciada que afete driver de áudio (APIs, modelos, ferramentas, SDKs).

### D. Fronteira e ideias autênticas
Esta seção é a mais valiosa. Quero **ideias de engenharia que não estão em produto comercial** — e quero que você separe honestamente o que já existe do que é genuinamente inexplorado.

Pesquise papers (DAFx, AES, ICASSP, Interspeech), teses, repos de pesquisa, blogs de engenheiros de DAW/driver, issues e design docs de projetos open source (PipeWire, JACK, PortAudio, FlexASIO, miniaudio, ASIO4ALL discussions), e patentes recentes. Para cada ideia: **já existe? onde? se não, por que não? qual o risco?**

Direções a investigar (e proponha outras por conta própria):

1. **Buffer size adaptativo com telemetria** — ajustar o buffer em tempo real a partir de estatística de deadline miss / DPC latency / xrun, em vez de o usuário escolher 64 ou 256 na mão. Existe? PipeWire faz algo nessa linha com quantum dinâmico?
2. **Previsão de drift por modelo** — em vez de PI controller reativo sobre nível de buffer, estimar o drift de clock com filtro de Kalman / regressão e aplicar ASRC antecipado. Precedentes?
3. **Zero-copy até o DAW** — memória compartilhada do kernel direto ao buffer do plugin host, eliminando cópias intermediárias. Quem chegou mais perto?
4. **Fan-out multi-client sem custo de latência** — um device, N clientes (DAW + comunicação + gravação) com SPSC lock-free e sem lock no callback. Como JACK/PipeWire resolvem e o que é portável para um driver ASIO no Windows.
5. **Perfil de ruído auto-calibrado por posição de ganho** — o driver aprende o espectro do próprio piso de ruído do preamp em cada posição de GAIN e aplica subtração/expansão calibrada, em vez de um gate genérico. Isso existe em algum produto?
6. **Gain staging automático** — o driver mede o sinal de entrada, detecta ganho analógico insuficiente ou clipping iminente, e orienta/ajusta. Precedentes (Zoom/Rode fazem algo?).
7. **DSP acelerado por NPU fora do caminho crítico** — supressão neural rodando em NPU/GPU apenas na ramificação de comunicação, com o caminho de gravação intacto. Latência real de NPU, APIs (DirectML, ONNX Runtime, Windows ML), viabilidade.
8. **CI com hardware no loop** — pipeline automatizado que, a cada build, roda loopback físico, mede RTL, THD+N, SNR, null test bit-perfect, e falha o build em regressão. Alguém faz isso publicamente?
9. **Grafo de DSP hot-reload** — recarregar a cadeia de processamento sem derrubar o stream. Como o PipeWire faz, e o que impede no Windows.
10. **Telemetria de xrun com causa raiz** — correlacionar underrun com ETW (DPC/ISR, thread preemption, page fault, power state) automaticamente, para dizer *qual driver do sistema* causou o glitch. LatencyMon faz parcialmente; o que falta para automatizar.
11. **Ring buffer com janela de tolerância variável** — latência baixa quando o sistema está calmo, folga automática sob carga, sem reset de stream audível.
12. **Driver como processo de usuário com prioridade de tempo real garantida** — o que o Windows oferece hoje de verdade (MMCSS, `SetThreadInformation`, `PROCESS_POWER_THROTTLING`), e onde o modelo userland quebra.
13. **Null test contínuo em produção** — o driver verificando continuamente sua própria transparência bit-perfect.
14. **Espaço de projeto pós-WDM** — alguém está propondo substituir o modelo de driver de áudio do Windows? O que a comunidade pro-audio pede à Microsoft (fóruns de feedback, Hub de desenvolvedores, grupos de trabalho AES)?

### E. Padrões de código canônicos (payload direto da skill)
Para cada item: trecho **mínimo, correto, com erro tratado**, em C/C++ (Win32/WDK) ou Rust onde fizer sentido. Diga header, lib, versão mínima do Windows e HRESULTs/NTSTATUS esperados em falha.

- Enumerar endpoints (`IMMDeviceEnumerator`), ler property store, identificar o device físico por instance path.
- Abrir WASAPI **exclusive event-driven** com alinhamento de buffer correto, incluindo o retry obrigatório em `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`.
- Abrir com `AUDCLNT_STREAMOPTIONS_RAW` + `MATCH_FORMAT` via `IAudioClient2::SetClientProperties` / `IAudioClient3`.
- Negociar período mínimo com `IAudioClient3::GetSharedModeEnginePeriod` + `InitializeSharedAudioStream`.
- Loop de áudio em tempo real: MMCSS, event wait, tratamento de `AUDCLNT_E_DEVICE_INVALIDATED`, `AUDCLNT_E_RESOURCES_INVALIDATED`, reinit sem vazar.
- **Process loopback** (`ActivateAudioInterfaceAsync` + `AUDIOCLIENT_ACTIVATION_PARAMS` com `PROCESS_LOOPBACK`) — captura por PID, incluir/excluir árvore de processos.
- Driver ASIO: esqueleto de COM, `IASIO` vtable, registro (CLSID em `HKLM\SOFTWARE\ASIO`), `bufferSwitchTimeInfo` correto, `getLatencies`, `getSamplePosition` monotônico, emissão correta de `kAsioResetRequest`.
- Ring buffer SPSC lock-free correto em x86-64 (barreiras, false sharing, `alignas(64)`), e por que `std::mutex` no callback é bug.
- ASRC de drift: estrutura do controlador + aplicação de ratio fracionário.
- Leitura/escrita de volume de endpoint (`IAudioEndpointVolume`) e detecção de ganho digital indevido.
- Detecção e log de **deadline miss** por callback (QPC, estatística, percentis).
- ETW: iniciar sessão com provider `Microsoft-Windows-Audio`, comandos `wpr`/`xperf` prontos, o que procurar no trace.
- Esqueleto de **PortCls/WaveRT miniport** e de **SysVAD-derived virtual endpoint**: arquivos, interfaces, pontos de extensão, INF mínimo.
- Instalação segura: `pnputil /add-driver /install`, `devcon`, como **nunca** remover INF de terceiro por índice, como identificar o próprio INF por provider.

### F. Medição, prova e CI
- Catálogo de ferramentas com **o que cada uma mede e o comando**: RTL Utility, REW, ARTA, Multitone Analyzer, RMAA, LatencyMon, WPR/WPA, `logman`, DPC Latency Checker (e por que é obsoleto), `AudioGlitchDetection`, testes de áudio do HLK.
- **Métricas que importam**: RTL medido (não reportado), jitter de callback (p50/p99/max), deadline miss rate, xrun count, THD+N, SNR, piso de ruído em PSD, resposta de frequência, crosstalk, bit-perfect null test, drift em ppm por hora.
- **Protocolo de medição reproduzível**: condições controladas, aquecimento, carga de CPU sintética, número de repetições, tolerâncias.
- Como montar **CI com loopback físico** — o que automatizar, onde falha, como gerar relatório de regressão.
- **Armadilhas de medição**: driver que mente na latência reportada, resampling escondido inflando resultado, APO ativo durante o teste, volume de endpoint ≠ 0 dB, autoajuste do dispositivo, buffer do sistema operacional mascarando xrun.

### G. Anti-padrões catalogados
Tabela: **anti-padrão / sintoma observável / causa / correção / como provar que corrigiu**. Inclua no mínimo:

- Alocação, lock, log, I/O ou exceção dentro do callback de áudio.
- DSP de custo variável (FFT, ML) na thread de tempo real.
- Ganho digital aplicado após o ADC em sinal de baixa amplitude.
- Resampling em cascata entre endpoints de taxas diferentes.
- Confiar no valor de latência reportado pelo driver.
- Inserir/descartar samples para corrigir drift.
- `timeBeginPeriod` global como muleta de estabilidade.
- Prioridade `TIME_CRITICAL` crua em vez de MMCSS.
- Remover INF de terceiros por índice (`oemXX.inf`).
- Ignorar `AUDCLNT_E_DEVICE_INVALIDATED` (device removido/trocado de taxa).
- Gate hard em voz quando o correto é downward expansion.
- Testar "de ouvido" sem medição de referência A/B nivelada em loudness.

### H. Ecossistema, licenças e restrições legais
- **ASIO SDK**: termos atuais da Steinberg, o que se pode distribuir, exigência de licença para produto, alternativas.
- Licenças de bibliotecas úteis: PortAudio, miniaudio, r8brain-free-src, soxr, libsamplerate, RNNoise, DeepFilterNet, speexdsp, KFR, FFTW vs pffft vs Intel MKL.
- WDK/WDF, SysVAD e samples da Microsoft: licença e o que pode ir para produto fechado.
- Assinatura: certificado EV, Partner Center, attestation vs WHQL — custo, prazo, requisitos atuais.
- Risco de distribuir driver kernel não certificado (bloqueio, Smart App Control, Defender, blocklist).

---

## 3. FORMATO DE SAÍDA — estruturado para virar skill

Entregue **um documento único**, em **PT-BR com termos técnicos em inglês**, organizado assim:

### Parte 1 — `SKILL.md` proposto
Escreva o conteúdo de um `SKILL.md` real:
- **Frontmatter**: `name` em kebab-case e `description` de uma linha (até ~200 caracteres) que descreva **quando** a skill deve ser acionada (gatilhos: escrever/depurar driver de áudio Windows, ASIO, WASAPI, latência, xrun, piso de ruído, APO, driver USB isócrono…).
- **Corpo**: as regras operacionais de mais alto valor — árvore de decisão de arquitetura, loop de trabalho obrigatório (medir → mudar uma coisa → medir → comparar), invariantes que nunca podem ser violadas, e ponteiros para os arquivos de referência da Parte 2.
- Mantenha o corpo **enxuto**: é o que fica sempre carregado. Detalhe vai para as referências.

### Parte 2 — Arquivos de referência
Proponha e **escreva o conteúdo** de cada arquivo de referência, com nome exato de arquivo. Sugestão de divisão (ajuste se tiver uma melhor e justifique):

- `references/arquiteturas.md` — eixo A + B (caminho do sample, evolução e o porquê).
- `references/estado-da-arte-2026.md` — eixo C, com marcação de confiança por item.
- `references/fronteira-e-ideias.md` — eixo D, cada ideia com: o que é, precedentes, por que ninguém fez, risco, experimento mínimo para validar.
- `references/padroes-de-codigo.md` — eixo E, trechos prontos.
- `references/medicao-e-ci.md` — eixo F, protocolo + comandos.
- `references/anti-padroes.md` — eixo G, tabela.
- `references/licencas-e-assinatura.md` — eixo H.
- `references/bibliografia.md` — links completos agrupados.

### Parte 3 — Checklists executáveis
- Checklist de **bring-up** de um driver novo (do zero ao primeiro stream estável).
- Checklist de **otimização de latência** (ordem exata das alavancas, da maior para a menor, com ganho esperado).
- Checklist de **caça a xrun/glitch**.
- Checklist de **caça a ruído/piso** (sinal → ADC → ganho digital → APO → DSP → dither).
- Checklist de **release** (assinatura, instalação limpa, rollback, convivência com outros drivers).

### Parte 4 — Tabelas de referência rápida
- APIs por cenário (quero X → use Y, mínimo Windows Z).
- Códigos de erro mais comuns (HRESULT/NTSTATUS/ASIOError) → causa provável → ação.
- Latência estrutural por arquitetura (números, com fonte).
- Comparação de plataformas (Windows/macOS/Linux/Android) em latência mínima, modelo de driver e esforço.

---

## 4. REGRAS DE QUALIDADE (invioláveis)

1. **Nunca invente** nome de API, flag, GUID, chave de registro, campo de struct, nome de arquivo do WDK, número de build do Windows ou título de paper. Sem certeza → `[NÃO VERIFICADO]`. API inventada em skill = código que não compila e horas perdidas.
2. Marque toda afirmação como **fato com fonte** / **consenso de engenharia** / `[ESTIMATIVA]`.
3. Todo número (latência, período, ppm, dB, custo) precisa de **unidade, condição de medição e fonte**.
4. Para tudo que mudou depois de 2023, cite **build/versão** do Windows e data da fonte.
5. Na seção de fronteira (D), **não confunda** "ninguém fez" com "não encontrei". Diga qual foi a busca.
6. Prefira fonte primária: Microsoft Learn/WDK, especificações USB-IF, ASIO SDK, código-fonte, papers revisados. Fóruns só com resposta verificável. Marque e desconfie de blog SEO.
7. Escreva para execução, não para leitura. Se um parágrafo não muda nenhuma decisão ou linha de código, corte.
8. Se algum item desta pesquisa estiver **tecnicamente obsoleto ou errado na premissa**, diga — inclusive se a conclusão for que escrever driver de áudio em 2026 é a pior forma de gastar esforço de engenharia, e qual seria a melhor.
