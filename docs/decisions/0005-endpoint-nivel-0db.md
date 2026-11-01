# ADR 0005 — Nível do endpoint de captura = 0,0 dB

**Status:** Aceito — 2026-10-31 (revisado: a primeira versão dizia "escalar 1.0", o que é ambíguo e perigoso)

## Contexto
O painel forçava o volume de captura em 86% (≈ +13 dB digital) — causa principal do chiado
percebido. Depois foi deixado em 19% (-15,7 dB), que atenua e descarta resolução.
Ganho unitário (0 dB) fica em ~54% do slider nesta máquina.

## A armadilha
`IAudioEndpointVolume::SetMasterVolumeLevelScalar(fLevel)` recebe um valor **normalizado e
audio-tapered de 0.0 a 1.0, onde 1.0 é o máximo da faixa** (Microsoft Learn). Ou seja,
"scalar 1.0" = slider em 100% = ganho digital máximo. Os dossiês de pesquisa falam em
"escalar 1.0 (54%)", misturando ganho linear com posição de slider. Implementar isso ao pé da
letra recria o bug do chiado.

## Decisão
- O host só escreve o nível do endpoint de captura com
  `SetMasterVolumeLevel(0.0f, nullptr)` (em dB), após `GetVolumeRange` confirmar que 0 dB
  está na faixa.
- `SetMasterVolumeLevelScalar` é proibido no código do projeto.
- Todo ganho é analógico (knob XENYX), alvo de picos em ~-9 dBFS.

## Verificação
`GetMasterVolumeLevel` lê 0,00 dB após a inicialização do host; M2 (piso) não muda em
relação ao baseline com o slider em 54%.
