#!/usr/bin/env bash
# Repository hygiene gate. Fails on anything that must never be committed.
set -uo pipefail
fail=0
tracked() { git ls-files -- "$@"; }

# 1. Steinberg ASIO SDK files (proprietary, not redistributable)
sdk=$(git ls-files | grep -Ei '(^|/)(asio\.h|asio\.cpp|asiosys\.h|asiodrivers\.(h|cpp)|asiolist\.(h|cpp)|iasiodrv\.h|combase\.(h|cpp)|dllentry\.cpp|register\.cpp)$' || true)
if [ -n "$sdk" ]; then echo "ERRO: arquivos do ASIO SDK versionados:"; echo "$sdk"; fail=1; fi

# 2. Binaries and raw measurement data
bin=$(tracked '*.dll' '*.exe' '*.sys' '*.pdb' '*.lib' '*.etl' '*.wav' '*.flac' '*.msi')
if [ -n "$bin" ]; then echo "ERRO: binarios/dados brutos versionados:"; echo "$bin"; fail=1; fi

# 3. Driver-store removal by fixed index
idx=$(git grep -nIE 'delete-driver[[:space:]]+oem[0-9]+\.inf' -- ':!tools/check_hygiene.sh' ':!docs/**' ':!CLAUDE.md' ':!installer/README.md' || true)
if [ -n "$idx" ]; then echo "ERRO: remocao de INF por indice fixo:"; echo "$idx"; fail=1; fi

# 4. Endpoint volume by slider position (ADR 0005): scalar 1.0 = max gain, not unity
vol=$(git grep -nI 'SetMasterVolumeLevelScalar' -- ':!tools/check_hygiene.sh' ':!docs/**' ':!*.md' || true)
if [ -n "$vol" ]; then echo "ERRO: SetMasterVolumeLevelScalar proibido (ADR 0005) - use SetMasterVolumeLevel(0.0f):"; echo "$vol"; fail=1; fi

# 5. License secret / keygen / personal data (repo privado, mas nunca no historico)
sec=$(git grep -nIE 'OBFUSCATED_SECRET[[:space:]]*(\[[[:space:]]*[0-9]*[[:space:]]*\])?[[:space:]]*=[[:space:]]*(\{|bytes)' -- ':!*.example.h' ':!*.example.py' ':!tools/check_hygiene.sh' || true)
if [ -n "$sec" ]; then echo "ERRO: segredo HMAC de licenca versionado (use src/UM2LicenseSecret.h, gitignorado):"; echo "$sec"; fail=1; fi
kg=$(git ls-files | grep -Ei '(^|/)(generate_license\.py|Gerar_Chave_Cliente_UM2\.bat|license\.key|donate_config\.json|UM2LicenseSecret\.h|um2_license_secret\.py)$' || true)
if [ -n "$kg" ]; then echo "ERRO: keygen/licenca/config pessoal versionado:"; echo "$kg"; fail=1; fi

[ $fail -eq 0 ] && echo "Higiene OK."
exit $fail
