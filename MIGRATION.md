# Migração de `C:\driver xd\behringer-um2` para este repositório

Mapeamento feito em 31/10/2026 lendo a árvore real. **O repo é privado** e o código
entra em três commits, nesta ordem. Os passos marcados 🔒 são obrigatórios **antes do
primeiro push com código** — se forem pulados, o segredo entra no histórico do Git e só sai
reescrevendo o histórico.

## 1. Não trazer

| Origem | Motivo | Destino |
|---|---|---|
| 🔒 `tools/generate_license.py`, `Gerar_Chave_Cliente_UM2.bat` | Gerador de chaves PRO com o segredo HMAC embutido. Ferramenta do vendedor | Pasta privada fora de qualquer repo (ex.: `C:\driver xd\vendor-tools\`) |
| 🔒 Array `OBFUSCATED_SECRET` em `src/UM2License.cpp` (linhas 230–232) | É o mesmo segredo. XOR com `(0x5A + i)` não é proteção | `src/UM2LicenseSecret.h`, gitignorado (passo 3) |
| 🔒 `panel/donate_config.json` | Contém chave PIX | `panel/donate_config.example.json` com placeholders |
| `../asiosdk/` | ASIO SDK 2.3.3 (2019), licença só proprietária | Fica onde está; CMake aponta para ele |
| `build/`, `release_package/`, `panel/build/`, `panel/dist/` | Artefatos (≈330 MB) | — |
| `*.exe`, `*.obj` na raiz e em `tools/` | Artefatos | — |
| `_backup_2026-10-31/` | Backup; o Git substitui | — |
| `_rollback_build/` | `src/` lá é **idêntico** ao atual (conferido byte a byte) | Só `UM2HissReducer.h.disabled` vai para o branch `archive/stft-hiss-reducer` |
| `driver_um2.log`, `tools/measurements/*.wav` (≈45 MB) | Dados brutos | `measure/raw/` local (ignorado) |

## 2. Mapeamento

| Origem (`behringer-um2/`) | Destino no repo | Observação |
|---|---|---|
| `CMakeLists.txt`, `Driver.def` | raiz | Ajustar só o `ASIO_SDK_DIR` (passo 3) |
| `src/*` | `src/` (plano) | Separação em módulos é depois da baseline — ver `src/README.md` |
| `test_um2_host.cpp`, `test_um2_probe.cpp`, `test_com_reg.cpp` | raiz na importação → `tests/` no refactor | `test_um2_host.cpp` inclui `"src/UM2Ipc.h"` com caminho relativo à raiz; mover exige ajustar o include |
| `panel/panel_backend.py`, `panel/web/`, `panel/*.spec` | `panel/` | O `.spec` referencia `../build/Release/BehringerUM2Bridge.dll` e `donate_config.json` |
| `tools/*.py` (`analyze_wav`, `api`, `ipc_dump`, `measure_monitor`, `measure_noise`) | `tools/` | Candidatos a `measure/scripts/` |
| `tools/raw_capture_test.cpp`, `tools/set_mic_fx.cpp`, `tools/ver_hwid.cpp` | `tools/` | `set_mic_fx.cpp` usa `IPolicyConfig` (não documentado, falha aqui com `0x800706C5`); `ver_hwid.cpp` inclui `../src/UM2License.h` |
| `tools/measurements/*.json` | `measure/baselines/legacy/` | Medições de 31/10 — histórico |
| `*.bat`, `Registrar_UM2_FL_Studio.reg` (exceto o keygen) | `installer/` | Revisar contra a invariante 7 |
| `Manual_Behringer_UM2_ASIO_Pro_v2.0.pdf`, `docs/manual_um2.html`, `docs/console_screenshot.png` | `docs/manual/` | |

## 3. Mudanças mínimas na importação (só estas)

1. 🔒 **Segredo da licença.** Em `src/UM2License.cpp`, trocar a declaração local do array por
   `#include "UM2LicenseSecret.h"`. Criar:
   - `src/UM2LicenseSecret.h` — o array real (gitignorado; guardar cópia segura fora do repo);
   - `src/UM2LicenseSecret.example.h` — mesmo formato, bytes zerados (versionado).
   - No `CMakeLists.txt`, falhar com mensagem clara se `src/UM2LicenseSecret.h` não existir.
2. **ASIO SDK.** Trocar o caminho fixo por variável de cache, sem copiar o SDK:
   ```cmake
   set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk/asiosdk_2.3.3_2019-06-14/common"
       CACHE PATH "Pasta common/ do Steinberg ASIO SDK")
   ```
3. **Doação.** Criar `panel/donate_config.example.json` com os mesmos campos
   (`creditsTitle`, `creditsSubtitle`, `donateEnabled`, `donateButtonLabel`, `donateUrl`,
   `pixKey`, `donateMessage`) e valores de exemplo.

Nada além disso muda no commit de importação.

## 4. Ordem dos commits
1. **Scaffold** (já feito): docs, regras, `.gitignore`, CI de higiene.
2. **Importação** com as mudanças do passo 3. Tem que compilar e passar `test_um2_host` 8/8.
   Mensagem: `chore: importa código atual (estado pós-rollback 31/10)`.
   > O CI de higiene **vai ficar vermelho** neste commit: o `panel_backend.py` usa
   > `SetMasterVolumeLevelScalar` (linhas 582 e 589). É de propósito — é o bug abaixo.
3. **Correção do bug ativo** (abaixo) → CI verde. Tag `v0.0.0-baseline` neste commit.
4. Só então começa a Fase 0 do `docs/ROADMAP.md`.

## 5. Bug ativo encontrado na leitura
O rollback de 31/10 comentou só a linha 601 do `panel_backend.py`. A linha **1675**, dentro do
endpoint `POST /api/auto-gain` (botão Auto-Gain do painel), ainda chama
`core_audio.ensure_microphone_gain(0.86)` — que escreve `SetMasterVolumeLevelScalar(0.86)`, ou seja,
slider em 86% ≈ +13 dB de ganho digital. **Cada clique em Auto-Gain traz o chiado de volta.**
A função `ensure_microphone_gain` inteira viola o ADR 0005 e deve ser substituída por uma que
escreva 0,0 dB com `SetMasterVolumeLevel`.

## 6. Antes do primeiro push com código
```powershell
git status --ignored          # SDK, DLLs, backups, UM2LicenseSecret.h, keygen e donate_config.json como ignorados
git grep -n "OBFUSCATED_SECRET"   # só pode aparecer no .example.h e no UM2License.cpp (sem bytes)
bash tools/check_hygiene.sh   # (Git Bash)
```

## Recomendação para depois (não bloqueia a migração)
HMAC é simétrico: a DLL distribuída precisa do segredo para validar, então quem tem a DLL
pode extraí-lo e gerar chaves. A correção definitiva é assinatura assimétrica (Ed25519):
a DLL leva só a chave pública, o keygen fica com a privada.
