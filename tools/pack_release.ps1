<#
  Monta o pacote de release do KaxWav UM2 a partir de um build ja feito (nao compila).
  Pre-requisitos: cmake --build build --config Release; panel\dist\BehringerUM2Panel.exe (PyInstaller);
  src\UM2LicenseSecret.h e panel\um2_license_secret.py REAIS (com o exemplo zerado nenhuma chave valida).
  Saida: release_package\KaxWav-UM2-<versao>\ + .zip + SHA256SUMS.txt (release_package/ e gitignorado).
  Whitelist explicita: nada fora da lista entra no zip (segredo, keygen e donate_config nunca).
  Uso: tools\pack_release.ps1 [-Version v0.1.0] [-Publish]   (-Publish: tag + gh release create)
#>
param([string]$Version, [switch]$Publish)
$ErrorActionPreference = 'Stop'
$Repo  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Build = Join-Path $Repo 'build\Release'
if (-not $Version) { $Version = (& git -C $Repo describe --tags --always --dirty 2>$null); if (-not $Version) { throw 'Passe -Version' } }
if ($Version -match 'dirty') { throw "Arvore suja ($Version): commite antes de empacotar." }

$secretH  = Join-Path $Repo 'src\UM2LicenseSecret.h'
$secretPy = Join-Path $Repo 'panel\um2_license_secret.py'
foreach ($f in $secretH, $secretPy) {
    if (-not (Test-Path $f)) { throw "Falta $f (copie do cofre)." }
    if ((Get-Content $f -Raw) -notmatch '0x(?!00)[0-9A-Fa-f]{2}') { throw "$f esta zerado (exemplo): as chaves nao validariam." }
}

$Artifacts = [ordered]@{
    'BehringerUM2ASIODriver.dll' = Join-Path $Build 'BehringerUM2ASIODriver.dll'
    'BehringerUM2Bridge.dll'     = Join-Path $Build 'BehringerUM2Bridge.dll'
    'test_com_reg.exe'           = Join-Path $Build 'test_com_reg.exe'
    'BehringerUM2Panel.exe'      = Join-Path $Repo 'panel\dist\BehringerUM2Panel.exe'
}
$Scripts = 'Instalar_KaxWav_UM2.bat', 'Instalar_KaxWav_UM2.ps1', 'Rollback_KaxWav_UM2.bat', 'Rollback_KaxWav_UM2.ps1',
           'Iniciar_Painel_UM2.bat', 'Verificar_Host_Elevado.ps1'
foreach ($k in $Artifacts.Keys) {
    $p = $Artifacts[$k]; if (-not (Test-Path $p)) { throw "Falta $p - compile Release x64 e o painel antes." }
    # A stale build is the classic release bug: refuse binaries older than the last commit.
    $commitTime = [DateTimeOffset]::FromUnixTimeSeconds([int64](& git -C $Repo log -1 --format=%ct)).LocalDateTime
    if ((Get-Item $p).LastWriteTime -lt $commitTime) { throw "$k e mais antigo que o ultimo commit - recompile." }
}

$Name = "KaxWav-UM2-$Version"
$Out  = Join-Path $Repo 'release_package'
$Dir  = Join-Path $Out $Name
$Zip  = Join-Path $Out "$Name.zip"
if (Test-Path $Dir) { Remove-Item $Dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
foreach ($k in $Artifacts.Keys) { Copy-Item $Artifacts[$k] (Join-Path $Dir $k) }
foreach ($s in $Scripts) { Copy-Item (Join-Path $Repo "installer\$s") (Join-Path $Dir $s) }
Copy-Item (Join-Path $Repo 'installer\LEIA-ME.txt') (Join-Path $Dir 'LEIA-ME.txt')
Copy-Item (Join-Path $Repo 'NOTICE.md') (Join-Path $Dir 'NOTICE.md')
Set-Content (Join-Path $Dir 'VERSION.txt') "$Version $(& git -C $Repo rev-parse --short HEAD)" -Encoding ASCII

# Sanity: nothing outside the whitelist, nothing secret.
$bad = Get-ChildItem $Dir -Recurse -File | Where-Object { $_.Name -match '(?i)secret|generate_license|Gerar_Chave|donate_config|license\.key|\.pdb$' }
if ($bad) { throw "Arquivo proibido no pacote: $($bad.Name -join ', ')" }

$sums = Get-ChildItem $Dir -File | Sort-Object Name | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name }
Set-Content (Join-Path $Dir 'SHA256SUMS.txt') $sums -Encoding ASCII
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path (Join-Path $Dir '*') -DestinationPath $Zip
$zipHash = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
Set-Content (Join-Path $Out "$Name.zip.sha256") "$zipHash  $Name.zip" -Encoding ASCII
Write-Host "[pack] $Zip"; Write-Host "[pack] SHA256 $zipHash"

if ($Publish) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'gh CLI nao encontrado.' }
    $notes = @"
KaxWav UM2 $Version - host ASIO para Behringer U-PHORIA UM2 (Windows 11 x64).

Instalar: extraia o zip, rode Instalar_KaxWav_UM2.bat e aceite o UAC. Leia o LEIA-ME.txt.
SHA-256 do zip: $zipHash
"@
    if (-not (& git -C $Repo tag -l $Version)) { & git -C $Repo tag $Version; & git -C $Repo push origin $Version }
    & gh release create $Version $Zip "$Zip.sha256" --repo Kaxcav/driver-kaxwav --title "KaxWav UM2 $Version" --notes $notes
    if ($LASTEXITCODE -ne 0) { throw "gh release create falhou ($LASTEXITCODE)" }
}
