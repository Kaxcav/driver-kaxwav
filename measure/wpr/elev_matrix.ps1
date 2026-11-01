<#
  Isolates "elevated host" vs "WPR running" for the ExclusivePro render starvation (DIAGNOSTIC).
  Elevated part: run E0 (elevated, no WPR); start WPR; signal the non-elevated side (file wpr_on) and wait
  for it (file nonelev_done); run E1 (elevated, WPR on); stop WPR. The non-elevated run N1 (WPR on) is
  launched by the operator when wpr_on appears.
#>
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Dll,
    [string]$RunId = 'elev-matrix',
    [int]$Seconds = 30
)
$ErrorActionPreference = 'Stop'
$out = Join-Path $RepoRoot "measure\out\$RunId"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out 'matrix.log'
function Say($m) { Add-Content $log "$(Get-Date -Format o) $m" }
function Invoke-Native([string]$Exe, [string[]]$Argv) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $o = & $Exe @Argv 2>&1 | ForEach-Object { "$_" }; return @{ code = $LASTEXITCODE; out = ($o -join "`n") } }
    finally { $ErrorActionPreference = $eap }
}
trap { Say "ERRO: $($_.Exception.Message)"; Invoke-Native 'wpr.exe' @('-cancel') | Out-Null; exit 1 }
$lp = Join-Path $RepoRoot 'measure\scripts\load_prio_ab.py'
$r = Invoke-Native 'python' @($lp, '--dll', $Dll, '--buffers', '256', '--classes', 'normal', '--reps', '1', '--seconds', "$Seconds", '--run-id', "$RunId-E0-elev-semWPR")
Say "E0 elevado sem WPR: $($r.out)"
Invoke-Native 'wpr.exe' @('-cancel') | Out-Null
$r = Invoke-Native 'wpr.exe' @('-start', ((Join-Path $PSScriptRoot 'um2_sched.wprp') + '!UM2Sched'), '-filemode')
Say "wpr -start ($($r.code))"
Set-Content (Join-Path $out 'wpr_on') 'on'
$deadline = (Get-Date).AddSeconds(150)
while (-not (Test-Path (Join-Path $out 'nonelev_done')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
Say "nao-elevado terminou: $(Test-Path (Join-Path $out 'nonelev_done'))"
$r = Invoke-Native 'python' @($lp, '--dll', $Dll, '--buffers', '256', '--classes', 'normal', '--reps', '1', '--seconds', "$Seconds", '--run-id', "$RunId-E1-elev-comWPR")
Say "E1 elevado com WPR: $($r.out)"
$r = Invoke-Native 'wpr.exe' @('-stop', (Join-Path $out 'matrix.etl'))
Say "wpr -stop ($($r.code))"
Say "FIM"
