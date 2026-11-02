<#
  Plano D, verificacao do lado elevado (rodar ELEVADO, depois do Instalar_KaxWav_UM2.ps1):
   1. host elevado instancia o driver pelo COM (test_com_reg) -> LOADED_PATH = DLL instalada;
   2. com o painel FECHADO, um host elevado (um2_asio_probe --com 1) cria as secoes Local\BehringerUM2* e o
      descritor delas tem de deixar o painel (nao elevado) abrir: rotulo Medium no-write-up + ACE do usuario.
  Resultado em %ProgramData%\KaxWav\verificar_host_elevado.log. Nao muda nada no sistema.
#>
param([string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
$ErrorActionPreference = 'Stop'
$log = Join-Path $env:ProgramData 'KaxWav\verificar_host_elevado.log'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
function Say($m) { Add-Content $log "$(Get-Date -Format s) $m"; Write-Host $m }
function Invoke-Native([string]$Exe, [string[]]$Argv) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $o = & $Exe @Argv 2>&1 | ForEach-Object { "$_" }; return @{ code = $LASTEXITCODE; out = ($o -join "`n") } }
    finally { $ErrorActionPreference = $eap }
}
trap { Say "ERRO: $($_.Exception.Message)"; exit 1 }
$bin = Join-Path $RepoRoot 'build\Release'
$id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
Say "elevado: $($id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"
$r = Invoke-Native (Join-Path $bin 'test_com_reg.exe') @()
Say ("1. test_com_reg (elevado): " + ($r.out -replace "`r?`n", ' | '))

Get-Process BehringerUM2Panel -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
$probe = Start-Process (Join-Path $bin 'um2_asio_probe.exe') -ArgumentList '--com','1','--mode','1','--buffer','256','--seconds','6' -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
$r = Invoke-Native 'python' @((Join-Path $RepoRoot 'measure\scripts\ipc_sd_check.py'))
Say ("2. secoes criadas pelo host elevado (exit $($r.code)): " + ($r.out -replace "`r?`n", ' | '))
$probe.WaitForExit(15000) | Out-Null
Say "FIM"
