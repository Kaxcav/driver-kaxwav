<#
  Volta a instalacao para um backup feito pelo Instalar_KaxWav_UM2.ps1.
  Rodar ELEVADO (Rollback_KaxWav_UM2.bat). Sem -BackupDir usa o backup mais recente.
  Restaura os arquivos com MD5 do manifest e reimporta as chaves exportadas; chaves que nao
  existiam antes da instalacao sao apagadas. Nao mexe em driver USB/INF.
#>
param([string]$BackupDir)
$ErrorActionPreference = 'Stop'
$Clsid = '{77DBBCCD-91C6-4856-A900-FC8D007D161C}'
$root = Join-Path $env:ProgramData 'KaxWav\backups'
# Same PS 5.1 stderr pitfall as the installer: native tools run under EAP=Continue.
function Invoke-Native([string]$Exe, [string[]]$Argv) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $Exe @Argv 2>&1 | Out-Null; return $LASTEXITCODE } finally { $ErrorActionPreference = $eap }
}
# Latest COMPLETE backup (an install aborted before the manifest leaves a dir without it).
if (-not $BackupDir) { $BackupDir = (Get-ChildItem $root -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'manifest.json') } |
                                    Sort-Object Name -Descending | Select-Object -First 1).FullName }
if (-not $BackupDir -or -not (Test-Path (Join-Path $BackupDir 'manifest.json'))) { throw "Backup nao encontrado em $root" }
$m = Get-Content (Join-Path $BackupDir 'manifest.json') -Raw | ConvertFrom-Json
$id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Rode elevado (Rollback_KaxWav_UM2.bat).' }
$users = Get-Process | Where-Object { try { $_.Modules | Where-Object { $_.ModuleName -ieq 'BehringerUM2ASIODriver.dll' } } catch { $null } } |
         Where-Object { $_.ProcessName -ne 'BehringerUM2Panel' }
if ($users) { throw ("Feche antes: " + (($users | ForEach-Object { $_.ProcessName }) -join ', ')) }
Get-Process BehringerUM2Panel -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
$ok = $true
foreach ($p in $m.files.PSObject.Properties) {
    $src = Join-Path $BackupDir "files\$($p.Name)"; $dst = Join-Path $m.dest $p.Name
    Copy-Item $src $dst -Force
    $h = (Get-FileHash $dst -Algorithm MD5).Hash
    Write-Host ("[KaxWav] {0}: {1} (esperado {2})" -f $p.Name, $h, $p.Value); if ($h -ne $p.Value) { $ok = $false }
}
foreach ($r in $m.registry) {
    Invoke-Native 'reg.exe' @('delete', $r.key, '/f') | Out-Null   # missing key is fine
    if ($r.existed) {
        $c = Invoke-Native 'reg.exe' @('import', (Join-Path $BackupDir $r.file))
        if ($c -ne 0) { Write-Host "[KaxWav] reg import falhou ($c): $($r.key)"; $ok = $false }
    }
}
Write-Host "[KaxWav] InprocServer32 HKCU: $((Get-ItemProperty "HKCU:\Software\Classes\CLSID\$Clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)')"
if ($ok) { Write-Host "[KaxWav] ROLLBACK OK ($BackupDir)" } else { Write-Host '[KaxWav] ROLLBACK COM DIVERGENCIA DE MD5'; exit 2 }
