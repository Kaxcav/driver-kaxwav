# Auditoria & upgrade do driver Behringer UM2 — 31/10/2026

> **STATUS ATUAL (21:56): ROLLBACK APLICADO.** Driver, bridge, IPC e painel voltaram ao código de antes da sessão (`_backup_2026-10-31/`) a pedido do usuário — o chiado no FL piorou com a cadeia de plugins depois das mudanças. Ver seção "Rollback" no fim. As seções abaixo documentam o que foi tentado e o que foi medido.

## Medições (UM2 real, PCM2902 VID_08BB/PID_2902)
- Captura RAW (sem APO): chiado branco plano **-88.5 dBFS** (PSD -133 dB/Hz até 20 kHz) = piso real pré XENYX + ADC (~89 dB SNR do PCM2902).
- Captura shared padrão: passava pelo APO **VocaEffectPack (Voice Clarity)** → banda cortada em ~7 kHz + supressão agressiva (silêncio zerado). Isso deixava o sinal "limpo" para a cadeia de plugins do FL.
- Usuário mudou o endpoint para 2 canais/48 kHz/16-bit e desativou aprimoramentos. CABLE Input/Output ficaram em 192 kHz/24-bit.

## Problemas encontrados
1. Autostart quebrado: tarefa `KaxWav_UM2_Console` sem gatilho, apontando para a pasta de dev.
2. Mic padrão do Windows era o cru (USB Audio CODEC) → DSP não chegava em Discord/navegador/OBS.
3. Bridge somava o Inst 2 sem gate no CABLE e no monitor dos fones (MON ONLY) → chiado do jack Hi-Z vazio; e reinjetava o loopback no monitor.
4. Instalador rodava `pnputil /delete-driver oem65.inf / oem66.inf` fixo: nesta máquina são **VB-Audio Cable** e **Xbox xvdd**. (Corrigido no `.bat` — busca por provedor "Ploytec".)
5. COM apontava para `release_package`, painel rodando era o antigo de `Program Files`.
6. Painel forçava volume do endpoint de captura em 86% na inicialização (~+13 dB de ganho digital após o ADC). **Causa principal do chiado percebido**: com o slider do Windows abaixado o problema sumiu.
7. Discord com AGC ligado: amplificava o ruído de fundo no silêncio e "passava por cima" do knob de GAIN.
8. Instalador `.bat` quebrava com duplo-clique (`-ArgumentList ''` vazio).

## O que foi tentado (revertido no rollback)
- Redutor espectral STFT `UM2HissReducer.h` (N=256 no ASIO, 512 na bridge) — guardado em `_rollback_build/UM2HissReducer.h.disabled`.
- ASIO Int32LSB, captura `AUDCLNT_STREAMOPTIONS_RAW`, IPC v5 (8 KB), gates de broadcast/monitor, curvas de preamp mais suaves, botões BOOT / MIC GLOBAL / RAW no painel.
- Lição: a captura RAW tirou o Voice Clarity do caminho do FL; o chiado natural (-88 dBFS) passou a ser levantado pela compressão/OTT dos plugins. Qualquer redução espectral pesada deve ficar só na bridge (Discord/OBS), nunca na thread ASIO.

## Rollback (21:42–21:56)
- `src/` restaurado byte a byte do backup (`ASIOSTInt16LSB`, IPC 848 bytes). `test_um2_host.cpp` com as adições removidas.
- `panel/panel_backend.py` e `panel/web/` restaurados do backup; **única diferença**: linha 601 `ensure_microphone_gain(0.86)` comentada (forçava o mic do Windows em 86% = chiado).
- Recompilado (Release x64). Instalado em `Program Files\KaxWav\Behringer UM2 ASIO\` e `release_package\`:
  - BehringerUM2ASIODriver.dll — MD5 `C834AE283BA63783F4437E9912ED219F` (169.472 bytes, mesmo tamanho do original)
  - BehringerUM2Bridge.dll — `96CBE2F3C1…`
  - BehringerUM2Panel.exe — `5118859F2C…`
- COM (HKCU) → `Program Files\...\BehringerUM2ASIODriver.dll`.
- Suíte `test_um2_host.exe`: **8/8 aprovados**, exit 0 (SharedFlex e ExclusivePro, 64/144/256/1024). ExclusivePro @64 cai transparente para o caminho compartilhado (PDC 352/608), sem loop de `kAsioResetRequest`.

## Estado da máquina após o rollback
- Painel restaurado rodando headless; autostart HKCU Run → `Program Files\...\BehringerUM2Panel.exe --headless` (mantido).
- Mic padrão do Windows: CABLE Output. Discord: entrada CABLE Output, AGC desligado.
- Slider de captura do Windows: 19% (-15.7 dB), como o usuário deixou. Regra: nunca acima de 54% (= 0 dB).
- Aprimoramentos do Windows no mic: continuam **desligados** (tentativa de religar via IPolicyConfig falhou, hr 0x800706C5). Religar manualmente em Som → Microfone → Propriedades → Avançado se quiser o Voice Clarity de volta no FL.
- Ferramentas mantidas em `tools/` (medição/diagnóstico).

## Próximos passos possíveis
- Tratar o chiado no FL na cadeia: gate/expander como primeiro plugin, antes de compressão/OTT.
- Botão "VOZ IA" (RNNoise) apenas na bridge (Discord/OBS).
- Se o mic for dinâmico com GAIN alto: booster inline (FetHead/Cloudlifter).
