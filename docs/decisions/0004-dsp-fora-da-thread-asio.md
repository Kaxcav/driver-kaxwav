# ADR 0004 — DSP fora da thread de áudio

**Status:** Aceito — 2026-10-31

## Contexto
Um redutor espectral STFT (N=256) na thread ASIO foi implementado e revertido em 2026-10-31:
artefatos e risco de deadline.

## Decisão
Todo DSP de custo variável roda na thread consumidora do fan-out SPSC. O callback ASIO copia
e retorna. O caminho da DAW é bit-perfect.
