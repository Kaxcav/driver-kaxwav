<#
  Instala o build do repo driver-kaxwav em Program Files, com backup e verificacao.
  Rodar ELEVADO (o Instalar_KaxWav_UM2.bat cuida disso). Nao mexe em driver USB/INF (invariante 7).

  1. Confere os artefatos do build (build\Release + panel\dist).
  2. Recusa se algum host (DAW) estiver com a DLL carregada; fecha o painel.
  3. Backup: arquivos atuais + export das chaves de registro + manifest.json com MD5
     em %ProgramData%\KaxWav\backups\<data_hora>\ (o Rollback_KaxWav_UM2.ps1 usa isso).
  4. Copia DLL do driver, DLL da bridge e o painel para Program Files.
  5. regsvr32 /s na DLL instalada (DllRegisterServer grava CLSID em HKCU e HKCR e as
     entradas HKLM/HKCU\SOFTWARE\ASIO). Remove a entrada WOW6432Node (a DLL e x64).
  6. Verifica: MD5 instalado == build; InprocServer32 aponta para Program Files;
     test_com_reg.exe instancia pelo COM e mostra o caminho carregado.
  Dois layouts: repo (build\Release + panel\dist) ou pacote de release (artefatos na mesma
  pasta do script, gerado por tools\pack_release.ps1). O pacote e detectado sozinho.
  Uso: Instalar_KaxWav_UM2.ps1 [-RepoRoot D:\DEV\driver-kaxwav] [-DryRun]
#>
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Clsid   = '{77DBBCCD-91C6-4856-A900-FC8D007D161C}'
$Dest    = Join-Path $env:ProgramFiles 'KaxWav\Behringer UM2 ASIO'
# Release package: artifacts sit next to this script (tools\pack_release.ps1).
$Package = Test-Path (Join-Path $PSScriptRoot 'BehringerUM2ASIODriver.dll')
if ($Package) { $RepoRoot = $PSScriptRoot; $Build = $PSScriptRoot; $PanelDir = $PSScriptRoot }
else          { $Build = Join-Path $RepoRoot 'build\Release'; $PanelDir = Join-Path $RepoRoot 'panel\dist' }
$Sources = [ordered]@{
    'BehringerUM2ASIODriver.dll' = Join-Path $Build 'BehringerUM2ASIODriver.dll'
    'BehringerUM2Bridge.dll'     = Join-Path $Build 'BehringerUM2Bridge.dll'
    'BehringerUM2Panel.exe'      = Join-Path $PanelDir 'BehringerUM2Panel.exe'
}
$ComCheck = Join-Path $Build 'test_com_reg.exe'
$Stamp    = Get-Date -Format 'yyyyMMdd_HHmmss'
$Backup   = Join-Path $env:ProgramData "KaxWav\backups\$Stamp"
# Native tools: in Windows PowerShell 5.1, stderr redirected from a native exe becomes an ErrorRecord, which is
# terminating under EAP=Stop (reg export of a missing key aborted the first real install). Run them under Continue.
function Invoke-Native([string]$Exe, [string[]]$Argv) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $o = & $Exe @Argv 2>&1 | ForEach-Object { "$_" }; return @{ code = $LASTEXITCODE; out = ($o -join "`n") } }
    finally { $ErrorActionPreference = $eap }
}
function Md5($p) { if (Test-Path $p) { (Get-FileHash $p -Algorithm MD5).Hash } else { $null } }
function Say($m) { Write-Host "[KaxWav] $m"; if (Test-Path $Backup) { Add-Content (Join-Path $Backup 'install.log') "$(Get-Date -Format s) $m" } }
# The elevated window closes on exit: persist any terminating error (in the backup log and in a fixed file).
trap {
    $msg = "ERRO: $($_.Exception.Message) (linha $($_.InvocationInfo.ScriptLineNumber))"
    Say $msg
    $d = Join-Path $env:ProgramData 'KaxWav'; New-Item -ItemType Directory -Force -Path $d | Out-Null
    Set-Content (Join-Path $d 'install_last_error.txt') "$(Get-Date -Format s) $msg"
    exit 1
}

# --- 1. artefatos
foreach ($k in $Sources.Keys) { if (-not (Test-Path $Sources[$k])) { throw "Falta $($Sources[$k]) - compile Release x64 e o painel (PyInstaller) antes." } }
if (-not (Test-Path $ComCheck)) { throw "Falta $ComCheck (alvo test_com_reg do CMake)." }

# --- 2. quem esta usando a DLL
$users = Get-Process | Where-Object {
    try { $_.Modules | Where-Object { $_.ModuleName -ieq 'BehringerUM2ASIODriver.dll' } } catch { $null }
} | Where-Object { $_.ProcessName -ne 'BehringerUM2Panel' }
if ($users) { throw ("Feche antes de instalar (DLL do driver carregada): " + (($users | ForEach-Object { "$($_.ProcessName) ($($_.Id))" }) -join ', ')) }

if ($DryRun) {
    Write-Host "[DryRun] destino: $Dest"; Write-Host "[DryRun] backup: $Backup"
    foreach ($k in $Sources.Keys) { Write-Host ("[DryRun] {0}: {1} -> instalado {2}" -f $k, (Md5 $Sources[$k]), (Md5 (Join-Path $Dest $k))) }
    Write-Host "[DryRun] InprocServer32 HKCU: $((Get-ItemProperty "HKCU:\Software\Classes\CLSID\$Clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)')"
    return
}

# --- elevacao (o DryRun acima roda sem)
$id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Rode elevado (use o Instalar_KaxWav_UM2.bat).' }

# --- 3. backup
New-Item -ItemType Directory -Force -Path $Backup | Out-Null
Say "Backup em $Backup"
$manifest = [ordered]@{ created = (Get-Date -Format s); dest = $Dest; repo = $RepoRoot
    repo_commit = $(if ($Package) { $v = Join-Path $PSScriptRoot 'VERSION.txt'; if (Test-Path $v) { (Get-Content $v -TotalCount 1) } else { 'package' } }
                    else { (Invoke-Native 'git.exe' @('-C', $RepoRoot, 'rev-parse', '--short', 'HEAD')).out })
    files = [ordered]@{}; registry = @() }
if (Test-Path $Dest) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Backup 'files') | Out-Null
    foreach ($k in $Sources.Keys) {
        $cur = Join-Path $Dest $k
        if (Test-Path $cur) { Copy-Item $cur (Join-Path $Backup "files\$k"); $manifest.files[$k] = Md5 $cur }
    }
}
$regKeys = @(
    "HKCU\Software\Classes\CLSID\$Clsid", "HKLM\SOFTWARE\Classes\CLSID\$Clsid",
    'HKCU\SOFTWARE\ASIO\Behringer UM2 ASIO', 'HKCU\SOFTWARE\ASIO\Behringer U-Phoria ASIO',
    'HKLM\SOFTWARE\ASIO\Behringer UM2 ASIO', 'HKLM\SOFTWARE\ASIO\Behringer U-Phoria ASIO',
    'HKLM\SOFTWARE\WOW6432Node\ASIO\Behringer UM2 ASIO', 'HKCU\Software\BehringerUM2')
$i = 0
foreach ($k in $regKeys) {
    $file = Join-Path $Backup ("reg_{0:D2}.reg" -f $i); $i++
    $r = Invoke-Native 'reg.exe' @('export', $k, $file, '/y')
    $manifest.registry += [ordered]@{ key = $k; existed = ($r.code -eq 0); file = (Split-Path $file -Leaf) }
}
$run = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).'KaxWav UM2 Console'
$manifest.run_key = $run
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Backup 'manifest.json') -Encoding UTF8

# --- 4. copia
Get-Process BehringerUM2Panel -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
foreach ($k in $Sources.Keys) { Copy-Item $Sources[$k] (Join-Path $Dest $k) -Force; Say "copiado $k" }

# --- 5. registro COM/ASIO
$dll = Join-Path $Dest 'BehringerUM2ASIODriver.dll'
$p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 falhou ($($p.ExitCode)) - rode Rollback_KaxWav_UM2.ps1" }
Invoke-Native 'reg.exe' @('delete', 'HKLM\SOFTWARE\WOW6432Node\ASIO\Behringer UM2 ASIO', '/f') | Out-Null  # x64-only DLL
Say 'regsvr32 OK'

# --- 6. verificacao
$ok = $true
foreach ($k in $Sources.Keys) {
    $a = Md5 $Sources[$k]; $b = Md5 (Join-Path $Dest $k)
    Say ("MD5 {0}: build {1} / instalado {2}" -f $k, $a, $b); if ($a -ne $b) { $ok = $false }
}
foreach ($root in 'HKCU:\Software\Classes', 'HKLM:\SOFTWARE\Classes') {
    $p = (Get-ItemProperty "$root\CLSID\$Clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
    Say "InprocServer32 ($root): $p"
    if ($p -ne $dll) { $ok = $false; Say "  diverge da DLL instalada ($dll)" }   # both views must point to it
}
# COM load as an ELEVATED host (FL "as administrator"): elevated processes ignore HKCU\Software\Classes, so this
# exercises the HKLM registration. The .bat repeats the check un-elevated (HKCU wins there).
$out = (Invoke-Native $ComCheck @()).out
Say ("test_com_reg (elevado): " + ($out -replace "`r?`n", ' | '))
if ($out -notmatch [regex]::Escape("LOADED_PATH=$dll")) { $ok = $false; Say 'COM elevado NAO carregou a DLL instalada' }
if ($ok) { Say 'ARQUIVOS, REGISTRO E COM ELEVADO OK (o .bat confere o COM sem elevacao)' } else { Say 'VERIFICACAO FALHOU - rode Rollback_KaxWav_UM2.ps1'; exit 2 }
