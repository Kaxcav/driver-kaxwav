# measure — harness de medição

Implementa `docs/MEASUREMENT-PROTOCOL.md` (M1–M8). Fase 1 do roadmap.

- `scripts/` — scripts de medição e o runner único.
- `baselines/` — resultados versionados, `<data>-<commit>.json`. **Commitados.**
- `raw/`, `out/` — capturas brutas e artefatos (`.wav`, `.etl`). **Gitignorados.**

Ferramentas de diagnóstico já existentes em `tools/` são candidatas a virar parte deste harness.
