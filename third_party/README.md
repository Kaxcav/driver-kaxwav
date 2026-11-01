# third_party

Dependências baixadas localmente. Este diretório só versiona este README.

## ASIO SDK (obrigatório, não versionado)
1. Baixar em https://www.steinberg.net/developers/ (aceitar a licença).
2. Extrair para `third_party/asio-sdk/` de forma que exista `third_party/asio-sdk/common/asio.h`.
3. `third_party/asio-sdk/` está no `.gitignore`. **Nunca force-add.** O check de higiene do CI falha
   se arquivos do SDK aparecerem no repositório.

## Outros (a definir na Fase 3/4)
- r8brain-free-src (MIT) — provável submodule.
- DeepFilterNet — confirmar licença do modelo e forma de integração.
