# Migração do código existente para este repositório

O código atual vive na máquina local (pasta de dev do KaxWav). Esta é a ordem para trazê-lo
sem contaminar o histórico.

## 1. Não trazer
| Origem | Motivo |
|---|---|
| ASIO SDK (onde quer que esteja vendorizado) | Proprietário Steinberg, sem redistribuição → `third_party/asio-sdk/` local |
| `release_package/`, `*.dll`, `*.exe`, `*.pdb` | Artefato de build |
| `_backup_2026-10-31/`, `_rollback_build/` | Backup local; o histórico do Git substitui |
| `*.etl`, `*.wav` de medição | Dados brutos → `measure/raw/` (ignorado) |

O que deve sobreviver do `_rollback_build/` (ex.: `UM2HissReducer.h.disabled`) vira um **branch**
`archive/stft-hiss-reducer`, não um arquivo na `main`.

## 2. Mapeamento de pastas
| Origem atual | Destino |
|---|---|
| `src/` — objeto COM / `IASIO` | `src/asio/` |
| `src/` — cliente WASAPI, buffers | `src/core/` |
| `src/` — bridge / CABLE | `src/bridge/` |
| `src/` — IPC (struct de 848 bytes) | `src/ipc/` |
| `panel/panel_backend.py`, `panel/web/` | `panel/` |
| `tools/` (medição/diagnóstico) | `tools/` → candidatos a `measure/scripts/` |
| `test_um2_host.cpp` | `tests/` |
| instalador `.bat` | `installer/` |
| arquivos de projeto (`.sln`/`.vcxproj`/CMake) | raiz ou `src/`, ajustando caminhos do SDK para `third_party/asio-sdk/` |

O mapeamento fino de `src/` (qual arquivo vai para qual módulo) se decide olhando a árvore real.

## 3. Ordem dos commits
1. **Scaffold** (este commit): docs, regras, `.gitignore`, CI de higiene.
2. **Código como está**, só movido para as pastas novas — sem mudança de comportamento.
   Precisa compilar e passar `test_um2_host` 8/8. Mensagem: `chore: importa código atual (estado pós-rollback 31/10)`.
3. Tag `v0.0.0-baseline` nesse commit — é o ponto de referência de todas as medições.
4. Só então começa a Fase 0 do `docs/ROADMAP.md`.

## 4. Antes do primeiro push do código
```powershell
git status --ignored            # conferir que SDK, DLLs e backups aparecem como ignorados
bash tools/check_hygiene.sh     # (Git Bash) tem de passar
```
