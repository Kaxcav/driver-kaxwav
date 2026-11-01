# third_party

Dependências baixadas localmente. Este diretório só versiona este README.

## ASIO SDK (obrigatório, não versionado)
- Nesta máquina o SDK já está em `C:\driver xd\asiosdk\asiosdk_2.3.3_2019-06-14\` — fora do
  repo. O CMake recebe o caminho por `-DASIO_SDK_DIR=...` (pasta `common/`).
- Alternativa: extrair em `third_party/asio-sdk/` (gitignorado).
- Licença: o 2.3.3 é só proprietário. Desde 29/10/2025 o SDK é dual GPLv3/proprietário; como o
  produto é comercial e fechado, vale o termo proprietário. **Nunca force-add** — o CI barra.

## Outros (a definir na Fase 3/4)
- r8brain-free-src (MIT) — provável submodule.
- DeepFilterNet — confirmar licença do modelo e forma de integração.
