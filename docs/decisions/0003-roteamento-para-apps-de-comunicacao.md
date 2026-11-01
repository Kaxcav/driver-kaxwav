# ADR 0003 — Roteamento para apps de comunicação

**Status:** Proposto — 2026-10-31 (corrige a síntese do Prompt 1)

## Contexto
Os dois dossiês do Prompt 1 afirmam que Process Loopback (`ActivateAudioInterfaceAsync` +
`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) elimina o VB-CABLE. **Isso só vale num sentido.**
Process Loopback *captura a saída de um processo*. O Discord faz o inverso: ele *lê um
dispositivo de captura* (microfone) e não aceita um processo como entrada.

## Decisão
- **Discord (precisa de um microfone):** continua exigindo um endpoint de captura virtual.
  Mantém o VB-CABLE, mas com **CABLE Input/Output em 48 kHz** (hoje estão em 192 kHz/24-bit),
  eliminando o resampling em cascata 48k → 192k → 48k. Custo zero, ganho imediato.
- **OBS (captura de aplicativo):** pode usar captura por processo em vez de cabo virtual.
- **Endpoint virtual próprio** (substituir o VB-CABLE de vez) exigiria driver kernel
  (SysVAD) e cai sob o ADR 0001. Fora de escopo.

## A verificar
Medir o resampling atual: sweep pelo caminho host → CABLE → captura, a 192 kHz e a 48 kHz.
