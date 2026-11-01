# ADR 0002 — Host ASIO sobre WASAPI Exclusive RAW event-driven

**Status:** Aceito — 2026-10-31

## Decisão
O host abre o device via WASAPI em modo exclusivo, event-driven, com `AUDCLNT_STREAMOPTIONS_RAW`,
e expõe ASIO para a DAW.

## Ressalva
A chamada exata de abertura está em aberto (C5): `InitializeSharedAudioStream` é shared mode
e não serve para exclusive. Decidir com medição antes de codar.

## Consequências
- ASIO é camada de compatibilidade, não de desempenho.
- RAW elimina o Voice Clarity e expõe o piso natural (-88,5 dBFS). O tratamento desse piso
  é responsabilidade da cadeia de DSP no ramo de comunicação e do expander no FL — não do
  caminho ASIO.
