# Roadmap

| Fase | Escopo | Aceite | Status |
|---|---|---|---|
| **M** | Migração (`MIGRATION.md`): importação + correção do Auto-Gain (`panel_backend.py:1675`) + tag `v0.0.0-baseline` | Build idêntico, 8/8, CI verde | Aberto |
| **0** | Fechar os abertos de `FATOS-VERIFICADOS.md` (C2, C3, C5, C6, C8) | Fonte primária ou medição | C1, C4, C7, C8 fechados |
| **1** | Harness de medição (`measure/`) + baseline do código atual | M1–M8 rodando com um comando | Feito (M1/M3/M4 aguardam cabo) |
| **2a** | Nível do endpoint = 0,0 dB (ADR 0005), ganho no XENYX | M2/M4 ≥ baseline | Aberto |
| **2b** | RAW + expander RMS antes de qualquer compressão | Sem degrau em 7 kHz; piso estável | Aberto |
| **2c** | VB-CABLE em 48 kHz; captura por processo só no OBS (ADR 0003) | 48 kHz ponta a ponta; sweep sem degradação | Aberto |
| **3** | Fan-out SPSC + ASRC r8brain/PI | 8 h sem glitch com DAW em carga | Aberto |
| **4** | Supressão neural só no ramo de comunicação | Latência do ramo medida; caminho DAW inalterado (M7) | Aberto (depende de C4) |

Critério de latência (M1) fica em aberto até C2 fechar.
