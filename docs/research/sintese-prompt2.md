# Síntese do dossiê do Prompt 2 — o que aproveitar e errata

**31/10/2026.** O dossiê do Prompt 2 é o mais útil dos três como mapa do campo e como fonte de
padrões de código, mas **não pode ser copiado**: tem bugs que compilam e falham em produção.

## Aproveitável (depois de conferido)

| Item | Estado |
|---|---|
| História e porquê de cada geração (MME → DirectSound/kmixer → WaveCyclic/WavePci → WaveRT → packet WaveRT → AudioEngine/APO → IAudioClient3) | Bom como contexto; nenhuma decisão depende disso |
| Por que ASIO ainda existe: buffers não-intercalados por canal, `ASIOTimeInfo` (samplePosition + systemTime) para PDC, sessão ASIO estável | Correto e relevante — justifica manter a camada ASIO |
| Padrão #2: WASAPI Exclusive event-driven com retry em `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` (GetBufferSize → re-Activate → re-Initialize com duração recalculada) | Bate com o padrão documentado pela Microsoft. Base para o código |
| Padrão #4: `GetSharedModeEnginePeriod` + `InitializeSharedAudioStream` (shared mode) | Correto, e confirma C5: é shared, não exclusive |
| Padrão #8: ring buffer SPSC (acquire/release, `alignas` por cache line, capacidade potência de 2) | Correto. Para áudio, acrescentar escrita/leitura em bloco |
| Padrão #9: controlador PI com anti-windup e limite de ±500 ppm | Estrutura correta; ganhos `kp`/`ki` precisam ser sintonizados com o drift medido (C8) |
| Padrão #11: tracker de deadline miss com QPC | Correto; o cálculo de p99 (com sort) roda **fora** do callback |
| Tabela de HRESULT (`BUFFER_SIZE_NOT_ALIGNED` 0x88890019, `DEVICE_INVALIDATED` 0x88890004, `RESOURCES_INVALIDATED` 0x88890026, `UNSUPPORTED_FORMAT` 0x88890008, `DEVICE_IN_USE` 0x8889000A) | Valores conferem |
| Tabela de licenças (FFTW GPL, soxr LGPL, r8brain MIT, libsamplerate BSD, RNNoise BSD, speexdsp BSD, PFFFT BSD) | Plausível; conferir cada uma antes de vincular |
| `IAudioEffectsManager` (Win11 22H2) para listar/desligar efeitos por stream | API real; útil para **provar** que o RAW pegou (lista de efeitos vazia) |
| Licença do ASIO SDK: dual GPLv3/proprietária | **Confirmado** (Steinberg, 29/10/2025) |
| Anti-padrões (alocação, lock, log, DSP variável, drift por descarte, `timeBeginPeriod`, TIME_CRITICAL, INF por índice, ignorar `DEVICE_INVALIDATED`) | Bom; já refletido no `CLAUDE.md` |

Fora de escopo (ADR 0001): esqueleto de miniport WaveRT, INF DCH, test-signing, Driver Verifier,
`/INTEGRITYCHECK`, checklist de release de `.sys`.

## Errata — NÃO copiar sem corrigir

| # | Onde | Erro | Correção |
|---|---|---|---|
| E1 | Invariantes, checklist de xrun, anti-padrões | Proíbe `WaitForSingleObject` na thread de áudio e manda "zerar" a ocorrência no caminho crítico | Esperar o evento do WASAPI **é** o loop event-driven. Proibido é esperar o que outra thread segura. (`CLAUDE.md` invariante 1) |
| E2 | Padrão #5 (loop MMCSS) | Se o MMCSS falha, cai para `THREAD_PRIORITY_TIME_CRITICAL` — contradiz o próprio anti-padrão | Registrar e reportar a falha; não mascarar (`CLAUDE.md` invariante 5) |
| E3 | Padrão #6 (process loopback) | `PropVariantClear` num `VT_BLOB` cujo `pBlobData` aponta para struct **na pilha** → `CoTaskMemFree` em memória de pilha → corrupção de heap | Não chamar `PropVariantClear` nesse PROPVARIANT (ou alocar o blob com `CoTaskMemAlloc`) |
| E4 | Padrão #6 | Ignora timeout do `WaitForSingleObject(…, 5000)` e lê o resultado mesmo assim; handler sem `FtmBase` | Tratar `WAIT_TIMEOUT`; conferir o handler contra o sample oficial *ApplicationLoopback* da Microsoft |
| E5 | Padrão #7 (ASIO) | `qpc * 1e9 / freq` estoura `uint64` quando `qpc > ~1,8e10` — com QPC de 10 MHz, **~30 min de uptime** | `(qpc / freq) * 1e9 + (qpc % freq) * 1e9 / freq` |
| E6 | Padrão #7 | `getLatencies` devolve `bufferSize` e `2 × bufferSize` arbitrários | Latência reportada = buffers + group delay do chip (0,66 ms) + USB, **calibrada pelo RTL medido** |
| E7 | Padrão #7 | `createBuffers` devolve ponteiros nulos | É esqueleto; não serve como base |
| E8 | Padrão #3 (RAW) | Não diz que `SetClientProperties` tem de vir **antes** de `Initialize` | Ordem: `Activate` → `SetClientProperties` → `Initialize` |
| E9 | Padrão #10 (volume) | Só corrige se o nível for > 0 dB; deixa atenuação passar | Escrever sempre 0,0 dB via `SetMasterVolumeLevel` (ADR 0005) |
| E10 | Geral (P1 e P2) | "Escalar 1.0 (54%)" mistura ganho linear com posição de slider | `SetMasterVolumeLevelScalar(1.0)` = slider 100% = ganho máximo. Proibido no código; o CI barra |
| E11 | Tabela de erros | `ASE_NoClock` = -999 | -995. (-999 é `ASE_HWMalfunction`) |
| E12 | Medição | GUID `{E27950EB-…}` como provider de áudio e perfis `wpr -start Audio -start DPCISR` sem fonte | Descobrir localmente: `logman query providers \| findstr /i audio` e `wpr -profiles` |
| E13 | Métricas | Alvos inconsistentes: RTL < 4,5 ms × tabela com 6,5 ms para UAC1; drift < 15 ppm × ±2 ppm no protocolo | Alvos só depois de medir (C2, C8) |
| E14 | Estado da arte | "Win11 24H2 fixa threads Pro Audio em P-cores e desliga timer coalescing" sem fonte | `[NÃO VERIFICADO]` — não usar como premissa |
| E15 | Árvore de decisão | Process loopback "elimina cabos virtuais" | Só no sentido captura de app. Discord precisa de microfone (ADR 0003) |
| E16 | Checklist de release | `pnputil /delete-driver <oemXX.inf>` | Localizar por *Original Name*/provedor antes (invariante 7) |
| E18 | Padrão #7 (ASIO) | `future(kAsioEnableTimeInfo)` → `ASE_SUCCESS` | Seletor inexistente. Host pergunta ao driver com `future(kAsioCanTimeInfo)` (10) → `ASE_SUCCESS`; driver pergunta ao host com `asioMessage(kAsioSupportsTimeInfo)` (7) e escolhe `bufferSwitchTimeInfo` ou `bufferSwitch` |
| E17 | Padrão #1 | Lê `PKEY_Device_InstanceId` do property store do **endpoint** | `[NÃO VERIFICADO]` — confirmar que a chave existe nesse store |

## Descoberta que nenhum dos três dossiês fez
Com o datasheet (C1), a UM2 tem **captura assíncrona e playback adaptativo** — entrada e saída
em clocks diferentes. Isso afeta o caminho da DAW, não só a bridge. Registrado como **C8** em
`docs/FATOS-VERIFICADOS.md`.
