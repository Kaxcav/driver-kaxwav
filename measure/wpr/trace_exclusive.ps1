<#
  WPR trace (lean profile um2_sched.wprp) around one diag_exclusive.py case. Needs elevation (WPR).
  Output: measure\out\<RunId>\um2_sched.etl + trace.log. Does not change any system setting.
#>
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Cases = 'load:256',
    [int]$Seconds = 60,
    [string]$RunId = ("{0}-wpr-ep" -f (Get-Date -Format 'yyyy-MM-dd'))
)
$ErrorActionPreference = 'Stop'
$out = Join-Path $RepoRoot "measure\out\$RunId"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out 'trace.log'
function Say($m) { Add-Content $log "$(Get-Date -Format o) $m"; Write-Host $m }
function Invoke-Native([string]$Exe, [string[]]$Argv) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $o = & $Exe @Argv 2>&1 | ForEach-Object { "$_" }; return @{ code = $LASTEXITCODE; out = ($o -join "`n") } }
    finally { $ErrorActionPreference = $eap }
}
trap { Say "ERRO: $($_.Exception.Message)"; Invoke-Native 'wpr.exe' @('-cancel') | Out-Null; exit 1 }

$profile = (Join-Path $PSScriptRoot 'um2_sched.wprp') + '!UM2Sched'
Invoke-Native 'wpr.exe' @('-cancel') | Out-Null   # a stale session would block -start
$r = Invoke-Native 'wpr.exe' @('-start', $profile, '-filemode')
Say "wpr -start ($($r.code)): $($r.out)"
if ($r.code -ne 0) { throw "wpr -start falhou" }
$py = Invoke-Native 'python' @((Join-Path $RepoRoot 'measure\scripts\diag_exclusive.py'), '--cases', $Cases,
                               '--seconds', "$Seconds", '--run-id', $RunId)
Say "diag_exclusive: $($py.out)"
$etl = Join-Path $out 'um2_sched.etl'
$r = Invoke-Native 'wpr.exe' @('-stop', $etl)
Say "wpr -stop ($($r.code)): $($r.out)"
Say "FIM"
