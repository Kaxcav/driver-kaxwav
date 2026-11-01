# panel
Painel de controle (backend Python + web). Código vem de `panel/` do projeto atual na migração.
Regra: o painel só escreve o nível do endpoint de captura como 0,0 dB via `SetMasterVolumeLevel` — nunca `SetMasterVolumeLevelScalar` (ADR 0005).
