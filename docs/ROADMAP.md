# Roadmap

| Fase | Escopo | Aceite | Status |
|---|---|---|---|
| **0** | Fechar C1–C6 em `FATOS-VERIFICADOS.md` | Todos com fonte primária ou medição | Aberto |
| **1** | Harness de medição (`measure/`) + baseline do código atual | M1–M8 rodando com um comando | Aberto |
| **2a** | Escalar do endpoint = 1.0, ganho no XENYX | M2/M4 ≥ baseline | Aberto |
| **2b** | RAW + expander RMS antes de qualquer compressão | Sem degrau em 7 kHz; piso estável | Aberto |
| **2c** | VB-CABLE em 48 kHz; captura por processo só no OBS (ADR 0003) | 48 kHz ponta a ponta; sweep sem degradação | Aberto |
| **3** | Fan-out SPSC + ASRC r8brain/PI | 8 h sem glitch com DAW em carga | Aberto |
| **4** | Supressão neural só no ramo de comunicação | Latência do ramo medida; caminho DAW inalterado (M7) | Aberto (depende de C4) |

Critério de latência (M1) fica em aberto até C2 fechar.
