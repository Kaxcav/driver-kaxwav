# ADR 0001 — Não escrever driver kernel-mode

**Status:** Aceito — 2026-10-31

## Contexto
O PCM2902 é UAC1 / USB 1.1 Full Speed com service interval fixo de 1 ms. Foi avaliado
substituir `usbaudio.sys` por um miniport PortCls/WaveRT próprio, e criar um endpoint
virtual derivado do SysVAD.

## Decisão
Não escrever nenhum `.sys`. Usar o driver de classe da Microsoft e trabalhar em userland.

## Consequências
- Ganho de latência descartado é marginal: o gargalo é o frame USB de 1 ms e o group delay
  dos filtros do chip, não o kernel.
- Evita cert EV (US$287–685/ano), Partner Center/attestation, restrições HVCI e risco de BSOD.
- Reabrir só com medição mostrando que o caminho userland não atinge o RTL físico do hardware.
