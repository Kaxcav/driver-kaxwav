# KaxWav — instruções de projeto

Driver/host ASIO para **Behringer U-PHORIA UM2** (TI PCM2902, UAC1) no Windows 11 x64.
Leia este arquivo inteiro antes de qualquer alteração. Ele existe porque o projeto já
perdeu uma sessão inteira em rollback por violar as invariantes abaixo.

## Contexto de hardware (fechado, não rediscutir)
- `USB\VID_08BB&PID_2902` → TI PCM2902. **UAC1, USB 1.1 Full Speed, 16-bit, ≤48 kHz.**
- Service interval isócrono de **1 ms** (192 bytes/frame a 48 kHz estéreo). Teto físico.
- Piso de ruído medido em modo RAW: **-88,5 dBFS**, PSD ≈ -133 dB/Hz. Coerente com o
  SNR de 89 dB do ADC. Isso é o hardware no seu limite, não um defeito a corrigir.
- Preamp XENYX no canal 1 (XLR), canal 2 instrumento Hi-Z.

## Decisão de arquitetura (ADR 0001/0002 — fechada)
Host ASIO em **userland** sobre **WASAPI Exclusive + RAW + event-driven**.
- **NÃO** escrever driver kernel-mode (`.sys`, PortCls/WaveRT, SysVAD). Ganho marginal em
  UAC1, custo de cert EV (US$287–685/ano) e barreira HVCI/attestation. Já foi avaliado e
  rejeitado com evidência. Se alguém propuser de novo, aponte para `docs/decisions/0001`.
- ASIO existe aqui **só** para compatibilidade com DAW. Não é alavanca de latência:
  o delta de jitter contra WASAPI Exclusive event-driven é < 100 µs.

## INVARIANTES — violar qualquer uma destas é bug, não escolha de estilo

1. **Nada de custo variável no callback de áudio.** Zero alocação (`new`, `malloc`,
   `std::vector::push_back`), zero lock (`std::mutex`), zero log, zero I/O, zero exceção,
   zero FFT, zero inferência de ML. O callback copia para ring buffer e retorna.
2. **DSP pesado só na thread secundária.** Expander, ASRC, supressão neural: todos no
   consumidor do fan-out, nunca no caminho da DAW. Tentativa de STFT na thread ASIO já
   foi revertida uma vez.
3. **Fan-out é SPSC lock-free** (`std::atomic`, índices separados, `alignas(64)` contra
   false sharing). Produtor escreve cego; consumidor absorve o drift via ASRC.
4. **Escalar do endpoint de captura = exatamente 1.0.** Nunca aplicar ganho digital
   pós-ADC em 16 bits (eleva o piso irrecuperavelmente); nunca atenuar (joga bits fora).
   Ganho vem do knob analógico. Se o código escrever volume de endpoint, só pode escrever 1.0.
5. **Thread de áudio via MMCSS.** `AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)`
   + `AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL)`. Nunca `THREAD_PRIORITY_TIME_CRITICAL`
   cru. Nunca `timeBeginPeriod` como muleta de estabilidade.
6. **APO se desliga com `AUDCLNT_STREAMOPTIONS_RAW` na inicialização do stream.** Não via
   `IPolicyConfig` (não documentado, falha com `0x800706C5` nesta máquina) e não por
   registro (reverte em update de driver).
7. **Nunca remover INF por índice** (`pnputil /delete-driver oemXX.inf`). Nesta máquina os
   índices pertencem a VB-Audio e Xbox xvdd. Amarrar sempre por hardware ID.
8. **Taxa travada em 48 kHz ponta a ponta.** Nenhum endpoint intermediário em 192 kHz.
   Resampling em cascata é degradação silenciosa.
9. **Medir antes e depois, uma mudança por vez.** Nada entra sem número. "Soou melhor"
   não é evidência — foi exatamente assim que a sessão anterior terminou em rollback.
10. **Nunca comitar o ASIO SDK.** Licença proprietária Steinberg, sem redistribuição.
    Fica em `third_party/asio-sdk/` (gitignorado). Idem DLLs compiladas e `.etl`/`.wav`.

## Perguntas ABERTAS — não assuma resposta
Estão em `docs/FATOS-VERIFICADOS.md` como C1–C6. Enquanto abertas, **não escreva código que
dependa delas** e não cite número de dossiê como se fosse fato. Resumo:
- **C1** Modo de clock do ADC: assíncrono ou síncrono? Define se o ASRC com PI é obrigatório.
- **C2** Orçamento de RTL e group delay dos filtros. Define o critério de aceite (≤5 ms vs ≤12 ms).
- **C3** `ASIOSTInt16LSB` vs `ASIOSTInt32LSB`.
- **C4** Latência algorítmica real do DeepFilterNet (10 ms ou 40 ms?).
- **C5** Qual API abre o stream. **`IAudioClient3::InitializeSharedAudioStream` é shared mode
  por definição** — não existe combiná-la com exclusive. Exclusive é `IAudioClient::Initialize`
  + `AUDCLNT_SHAREMODE_EXCLUSIVE` + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- **C6** `KSPROPERTY_RTAUDIO_POSITIONREGISTER` é propriedade KS de kernel. Cliente userland
  usa `IAudioClock2::GetDevicePosition`. Os dossiês erram isso; não copie.

## Os dossiês em `docs/research/` são FONTE, não verdade
Foram gerados por pesquisa automatizada e contêm API inexistente (C5, C6) e números
inconsistentes entre si (C2). Use-os para localizar fontes primárias — datasheet do PCM2902,
Microsoft Learn, repo do DeepFilterNet — nunca como citação final. Fato só entra em
`FATOS-VERIFICADOS.md` com fonte primária ou medição nesta máquina.

## Fluxo de trabalho obrigatório
1. Rodar o harness (`measure/`) e guardar o baseline.
2. Fazer **uma** mudança.
3. Rodar o harness de novo e comparar contra o baseline.
4. Regrediu em qualquer métrica → reverter, não "ajustar depois".
5. Ganho confirmado → commit com os números no corpo da mensagem.

## Build
- MSVC x64, Release. Sistema de build: o do código existente (ver `MIGRATION.md`).
- ASIO SDK: baixar da Steinberg e extrair em `third_party/asio-sdk/`.
- Alvos: `BehringerUM2ASIODriver.dll` (COM/ASIO), `BehringerUM2Bridge.dll`,
  `BehringerUM2Panel.exe`, `test_um2_host.exe`.
- Registro COM do ASIO em `HKLM\SOFTWARE\ASIO` (ou HKCU para dev).

## Estilo
- C++20. RAII em tudo que é handle COM/Win32. `HRESULT` sempre checado; nunca descartado.
- Erros de áudio tratados explicitamente: `AUDCLNT_E_DEVICE_INVALIDATED`,
  `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` (com o retry obrigatório),
  `AUDCLNT_E_RESOURCES_INVALIDATED`.
- Respostas e commits em PT-BR; identificadores e comentários de código em inglês.
