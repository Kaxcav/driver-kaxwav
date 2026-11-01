# src

| Pasta | Conteúdo | Regra |
|---|---|---|
| `asio/` | COM + `IASIO`, double buffer, latências, posição | Latência reportada = latência medida |
| `core/` | Stream WASAPI, ring buffer SPSC, relógio | Callback: copia e retorna |
| `dsp/` | Expander, ASRC, adaptador de supressão | Nunca chamado da thread de áudio |
| `bridge/` | Consumidor do fan-out → apps de comunicação | Falha aqui nunca afeta a DAW |
| `ipc/` | Protocolo versionado host ↔ painel | Leitura nunca bloqueia o callback |
| `common/` | Telemetria, log assíncrono, QPC, erros | Log só fora do caminho crítico |

A distribuição do código atual nestas pastas é feita na migração (`MIGRATION.md`).
