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
- Período mínimo exclusive do `usbaudio.sys` = **3,0 ms** → menor buffer ExclusivePro = **144** a 48 kHz.
- Clock (medido, M6): entrada e saída andam juntas (0,002 ppm/1 h), ~11 ppm abaixo do QPC. O caminho
  da DAW não precisa de ASRC; a bridge (outro domínio) precisa de ASRC com PI, faixa ±50 ppm.

## Decisão de arquitetura (ADR 0001/0002 — fechada)
Host ASIO em **userland** sobre **WASAPI Exclusive + RAW + event-driven**.
- **NÃO** escrever driver kernel-mode (`.sys`, PortCls/WaveRT, SysVAD). Ganho marginal em
  UAC1, custo de cert EV (US$287–685/ano) e barreira HVCI/attestation. Já foi avaliado e
  rejeitado com evidência. Se alguém propuser de novo, aponte para `docs/decisions/0001`.
- ASIO existe aqui **só** para compatibilidade com DAW. Não é alavanca de latência:
  o delta de jitter contra WASAPI Exclusive event-driven é < 100 µs.

## INVARIANTES — violar qualquer uma destas é bug, não escolha de estilo

1. **Nada de custo variável no callback de áudio.** Zero alocação (`new`, `malloc`,
   `std::vector::push_back`), zero lock (`std::mutex`, `EnterCriticalSection`), zero log,
   zero I/O, zero exceção, zero FFT, zero inferência de ML. O callback copia para ring
   buffer e retorna. Exceção única e obrigatória: a thread de áudio **espera o evento do
   WASAPI** (`WaitForSingleObject`/`WaitForMultipleObjects` no evento de buffer + evento de
   parada) — esse é o loop event-driven. Proibido é esperar qualquer coisa que outra thread
   segura. Checklist que mande "zerar WaitForSingleObject" no caminho crítico está errado.
2. **DSP pesado só na thread secundária.** Expander, ASRC, supressão neural: todos no
   consumidor do fan-out, nunca no caminho da DAW. Tentativa de STFT na thread ASIO já
   foi revertida uma vez.
3. **Fan-out é SPSC lock-free** (`std::atomic`, índices separados, `alignas(64)` contra
   false sharing). Produtor escreve cego; o consumidor da bridge absorve o drift (~11 ppm) via ASRC.
4. **Nível do endpoint de captura = 0,0 dB (ganho linear 1.0).** Escrever com
   `IAudioEndpointVolume::SetMasterVolumeLevel(0.0f, nullptr)`, depois de conferir com
   `GetVolumeRange` que 0 dB está dentro da faixa.
   **NUNCA `SetMasterVolumeLevelScalar(1.0f)`**: o "scalar" dessa API é a *posição do
   slider* (curva audio-tapered, 1.0 = máximo da faixa), não ganho unitário. Nesta máquina
   1.0 = slider em 100% = ganho digital máximo — exatamente o bug do chiado (86% já dava
   ≈ +13 dB; 0 dB fica em ~54%). Nunca aplicar ganho digital pós-ADC em 16 bits (eleva o
   piso irrecuperavelmente); nunca atenuar (joga bits fora). Ganho vem do knob analógico.
5. **Thread de áudio via MMCSS.** `AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)` +
   `AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL)`, e nada mais. Se o MMCSS retornar NULL, registre o
   erro e reporte, não mascare. **Nunca `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`**, nem como
   "piso" depois do MMCSS: medido em 01/11/2026, ele **substitui** o reforço do MMCSS e deixa a
   thread em 15 (só MMCSS: 24; MMCSS + CRITICAL: 26 —
   `measure/baselines/2026-11-01-prio-variantes.json`). A exceção aberta em 01/11 está **encerrada**.
   Nunca `timeBeginPeriod` como muleta.
6. **APO se desliga com `AUDCLNT_STREAMOPTIONS_RAW` na inicialização do stream.** Não via
   `IPolicyConfig` (não documentado, falha com `0x800706C5` nesta máquina) e não por
   registro (reverte em update de driver).
7. **Nunca remover INF por índice** (`pnputil /delete-driver oemXX.inf`). Nesta máquina os
   índices pertencem a VB-Audio e Xbox xvdd. Amarrar sempre por hardware ID.
8. **Taxa travada em 48 kHz ponta a ponta.** Nenhum endpoint intermediário em 192 kHz.
   Resampling em cascata é degradação silenciosa.
9. **Medir antes e depois, uma mudança por vez.** Nada entra sem número. "Soou melhor"
   não é evidência — foi exatamente assim que a sessão anterior terminou em rollback.
10. **Nunca comitar o ASIO SDK.** O SDK em uso (2.3.3, 2019) é só proprietário; o 2.3.4+ é
    dual GPLv3/proprietário, mas o produto é comercial e fechado. Fica fora do repo
    (`ASIO_SDK_DIR` aponta para `C:\driver xd\asiosdk`). Idem DLLs, `.etl`, `.wav`.
11. **O segredo de licença nunca entra no Git.** O array HMAC vive em `src/UM2LicenseSecret.h`
    (gitignorado); só o `.example.h` é versionado. O gerador de chaves
    (`generate_license.py`, `Gerar_Chave_Cliente_UM2.bat`) é ferramenta do vendedor e não vive
    neste repo. `donate_config.json` (chave PIX) idem. O CI barra os três.

12. **Os streams exclusive são iniciados pela thread de áudio** (já em MMCSS), logo antes do loop de
    espera, e a vigia do render fica ligada. Mover o `Start()` de volta para `start()` (thread do host)
    reabre a armadilha do render: com CPU ocupada, 12/12 partidas presas em ~53% dos callbacks.
    Mecanismo e números em `docs/FATOS-VERIFICADOS.md` ("Armadilha do render no ExclusivePro").
    Toda mudança na partida do stream passa também pelo `measure/scripts/trap_rate.py`.

## Repo e migração
- Repo **privado**, escopo só UM2/UMC22. Código de origem: `C:\driver xd\behringer-um2`.
- A importação segue `MIGRATION.md` à risca (3 commits, passos 🔒 antes do primeiro push).
- Migração concluída (`v0.0.0-baseline`); o Auto-Gain já grava 0,0 dB (`ensure_capture_level_0db`).

## Perguntas ABERTAS — não assuma resposta
Estão em `docs/FATOS-VERIFICADOS.md`. C1, C4 e C7 e a parte do chip de C2 já foram fechados
com datasheet/documentação oficial; **C8 foi fechado por medição** (M6, 1 h: entrada e saída da UM2
derivam 0,002 ppm entre si; os dois estão ~11 ppm abaixo do QPC) — use esses valores. Enquanto um item estiver aberto,
**não escreva código que dependa dele** e não cite número de dossiê como se fosse fato. Abertos:
- **C2** RTL total (o chip contribui 0,66 ms; o resto só medindo). Define o critério de aceite.
- **C3** `ASIOSTInt16LSB` vs `ASIOSTInt32LSB`.
- **C5** Qual API abre o stream. **`IAudioClient3::InitializeSharedAudioStream` é shared mode
  por definição** — não existe combiná-la com exclusive. Exclusive é `IAudioClient::Initialize`
  + `AUDCLNT_SHAREMODE_EXCLUSIVE` + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- **C6** `KSPROPERTY_RTAUDIO_POSITIONREGISTER` é propriedade KS de kernel. Cliente userland
  usa `IAudioClock2::GetDevicePosition`. Os dossiês erram isso; não copie.

## Os dossiês em `docs/research/` são FONTE, não verdade
Foram gerados por pesquisa automatizada e contêm API inexistente (C5, C6), números
inconsistentes entre si (C2) e código com bugs (errata E1–E17 em `sintese-prompt2.md`:
overflow de QPC→ns em ~30 min, `PropVariantClear` em memória de pilha, `ASE_NoClock` errado…). Use-os para localizar fontes primárias — datasheet do PCM2902,
Microsoft Learn, repo do DeepFilterNet — nunca como citação final. Fato só entra em
`FATOS-VERIFICADOS.md` com fonte primária ou medição nesta máquina.

## Skill
Para receitas de API corrigidas (exclusive com retry, RAW, volume 0 dB, QPC→ns, contrato ASIO,
process loopback) e protocolo de medição, use a skill **engenharia-driver-audio-windows**.

## Fluxo de trabalho obrigatório
1. Rodar o harness (`measure/`) e guardar o baseline.
2. Fazer **uma** mudança.
3. Rodar o harness de novo e comparar contra o baseline. **Mexeu em thread, prioridade, espera
   ou timing?** Rode também o M5 sob carga (`--cpu-load 12`, antes e depois) e confira
   callbacks/min contra o esperado e o maior intervalo entre callbacks — o p99 da duração do bloco
   sozinho não pega thread acordando atrasada.
4. Regrediu em qualquer métrica → reverter, não "ajustar depois".
5. Ganho confirmado → commit com os números no corpo da mensagem.

## Build
- MSVC x64, Release, CMake (o `CMakeLists.txt` do projeto atual, na raiz).
- Alvos: `BehringerUM2ASIODriver.dll` (COM/ASIO), `BehringerUM2Bridge.dll`,
  `BehringerUM2Panel.exe` (PyInstaller, `panel/BehringerUM2Panel.spec`), `test_um2_host.exe`.
- `cmake -S . -B build -A x64 -DASIO_SDK_DIR="C:/driver xd/asiosdk/asiosdk_2.3.3_2019-06-14/common"`
- Requer `src/UM2LicenseSecret.h` local (copiar do cofre; formato em `.example.h`).
- Registro COM do ASIO em `HKLM\SOFTWARE\ASIO` (ou HKCU para dev).

## Estilo
- C++20. RAII em tudo que é handle COM/Win32. `HRESULT` sempre checado; nunca descartado.
- Erros de áudio tratados explicitamente: `AUDCLNT_E_DEVICE_INVALIDATED`,
  `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` (com o retry obrigatório),
  `AUDCLNT_E_RESOURCES_INVALIDATED`.
- Respostas e commits em PT-BR; identificadores e comentários de código em inglês.
