# ADR 0005 — Escalar do endpoint de captura = 1.0

**Status:** Aceito — 2026-10-31

## Contexto
O painel forçava 86% (≈ +13 dB digital) — causa principal do chiado percebido. Depois foi
deixado em 19% (-15,7 dB), que atenua e descarta resolução.

## Decisão
O host só escreve 1.0 no volume do endpoint de captura. Todo ganho é analógico (knob XENYX),
alvo de picos em ~-9 dBFS.
