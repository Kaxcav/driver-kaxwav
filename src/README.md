# src

## Na importação (commit 2 da `MIGRATION.md`)
O código entra **plano**, exatamente como está em `behringer-um2/src/` — mesmo layout,
mesmo `CMakeLists.txt` — para o primeiro build no repo ser idêntico ao que passa 8/8 hoje.
Única mudança obrigatória: o segredo de licença sai do `UM2License.cpp` para
`UM2LicenseSecret.h` (gitignorado).

| Arquivo | Papel | Módulo-alvo |
|---|---|---|
| `dllmain.cpp`, `Driver.def` | Entrada COM da DLL ASIO | `asio/` |
| `AsioDriver.cpp/.h` (95 KB) | `IASIO` + stream WASAPI + buffers — monolito | dividir em `asio/` e `core/` |
| `UM2Ipc.h` | Struct de memória compartilhada (`Local\BehringerUM2SharedMemory`, 848 bytes) | `ipc/` |
| `UM2Dsp.h` (35 KB) | DSP — incluído pelo `AsioDriver.h` **e** pela bridge | `dsp/` |
| `UM2Loopback.h` | Loopback — incluído pelo `AsioDriver.h` e pela bridge | `bridge/` |
| `UM2Bridge.cpp/.h` | DLL da bridge (Discord / mic virtual) | `bridge/` |
| `UM2License.cpp/.h` | Validação HMAC da licença PRO | `license/` |

## Depois da tag `v0.0.0-baseline`
A divisão em módulos é o primeiro refactor, sem mudança de comportamento, medida contra o
baseline. Atenção: o `AsioDriver.h` inclui `UM2Dsp.h` e `UM2Loopback.h` — o caminho da DAW
depende hoje de código de DSP/loopback. Separar isso é parte do ADR 0004.

| Módulo | Regra |
|---|---|
| `asio/` | Latência reportada = latência medida |
| `core/` | Callback: copia e retorna |
| `dsp/` | Nunca chamado da thread de áudio |
| `bridge/` | Falha aqui nunca afeta a DAW |
| `ipc/` | Leitura nunca bloqueia o callback |
| `license/` | Segredo nunca versionado |
