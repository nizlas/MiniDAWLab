# =============================================================================
# stability-matrix.ps1 - run the Stability C2 scenario matrix against a project
# =============================================================================
# Usage:
#   .\scripts\stability-matrix.ps1 -Project "C:\path\project.dalproj"
#   .\scripts\stability-matrix.ps1 -Project ... -Iterations 10 -IncludeMixdown
#   .\scripts\stability-matrix.ps1 -Project ... -Exe "C:\path\MiniDAWLab.exe"
#
# Runs load-loop, delete-loop and smoke (plus mixdown wav/mp3 with -IncludeMixdown)
# through the in-process stability scenario runner, collects exit codes
# (0 = PASS, 1 = FAIL) and prints a summary. Each scenario has a hard timeout;
# a timed-out app is killed and counted as FAIL.
#
# Logs (all under %APPDATA%\MiniDAWLab):
#   stability-run.log, project-load-diag.log, track-delete-diag.log,
#   mixdown-diag.log, last-operation.txt, crash-dumps\
# =============================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [string]$Exe = '',

    [int]$Iterations = 5,

    [switch]$IncludeMixdown,

    [int]$TimeoutSec = 900
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $Exe = Join-Path $repoRoot 'build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe'
}
if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Error "Executable not found: $Exe (build with .\scripts\build-windows.ps1 -Config Debug)"
}
if (-not (Test-Path -LiteralPath $Project)) {
    Write-Error "Project file not found: $Project"
}
$Project = (Resolve-Path -LiteralPath $Project).Path

$appData = Join-Path $env:APPDATA 'MiniDAWLab'
$runLog = Join-Path $appData 'stability-run.log'
$crashDumps = Join-Path $appData 'crash-dumps'

# Count crash dumps before the run so new ones can be reported afterwards.
$dumpCountBefore = 0
if (Test-Path -LiteralPath $crashDumps) {
    $dumpCountBefore = @(Get-ChildItem -LiteralPath $crashDumps -Filter '*.dmp' -ErrorAction SilentlyContinue).Count
}

# Stability C3: baseline invariant failure count (the log accumulates across runs).
$invariantLogPath = Join-Path $appData 'stability-invariant.log'
$invariantFailsBefore = 0
if (Test-Path -LiteralPath $invariantLogPath) {
    $invariantFailsBefore = @(Select-String -LiteralPath $invariantLogPath -Pattern 'INVARIANT FAIL' -ErrorAction SilentlyContinue).Count
}

function Invoke-StabilityScenario {
    param(
        [string]$Name,
        [string[]]$Arguments
    )
    Write-Host ""
    Write-Host ("=== {0} ===" -f $Name) -ForegroundColor Cyan
    Write-Host ("    {0} {1}" -f $Exe, ($Arguments -join ' '))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Warning "Timeout after $TimeoutSec s - killing process."
        try { $proc.Kill() } catch { }
        $sw.Stop()
        return [pscustomobject]@{ Name = $Name; ExitCode = -1; Pass = $false; Seconds = [int]$sw.Elapsed.TotalSeconds; Note = 'TIMEOUT' }
    }
    $sw.Stop()
    $code = $proc.ExitCode
    $pass = ($code -eq 0)
    $color = if ($pass) { 'Green' } else { 'Red' }
    Write-Host ("    exit code {0} ({1}) in {2}s" -f $code, $(if ($pass) { 'PASS' } else { 'FAIL' }), [int]$sw.Elapsed.TotalSeconds) -ForegroundColor $color
    return [pscustomobject]@{ Name = $Name; ExitCode = $code; Pass = $pass; Seconds = [int]$sw.Elapsed.TotalSeconds; Note = '' }
}

$results = @()
$results += Invoke-StabilityScenario -Name 'load-loop' -Arguments @('--stability-load-loop', "`"$Project`"", '--iterations', "$Iterations")
$results += Invoke-StabilityScenario -Name 'delete-loop' -Arguments @('--stability-delete-loop', "`"$Project`"", '--iterations', "$Iterations")
$results += Invoke-StabilityScenario -Name 'smoke' -Arguments @('--stability-smoke', "`"$Project`"")
if ($IncludeMixdown) {
    $results += Invoke-StabilityScenario -Name 'mixdown-wav' -Arguments @('--stability-mixdown', "`"$Project`"", '--format', 'wav')
    $results += Invoke-StabilityScenario -Name 'mixdown-mp3' -Arguments @('--stability-mixdown', "`"$Project`"", '--format', 'mp3')
}

Write-Host ""
Write-Host "================ Stability matrix summary ================" -ForegroundColor Cyan
$allPass = $true
foreach ($r in $results) {
    if (-not $r.Pass) { $allPass = $false }
    $status = if ($r.Pass) { 'PASS' } else { 'FAIL' }
    $color = if ($r.Pass) { 'Green' } else { 'Red' }
    $note = if ($r.Note) { " ($($r.Note))" } else { '' }
    Write-Host ("  {0,-14} {1}  exit={2}  {3}s{4}" -f $r.Name, $status, $r.ExitCode, $r.Seconds, $note) -ForegroundColor $color
}

$dumpCountAfter = 0
if (Test-Path -LiteralPath $crashDumps) {
    $dumpCountAfter = @(Get-ChildItem -LiteralPath $crashDumps -Filter '*.dmp' -ErrorAction SilentlyContinue).Count
}
if ($dumpCountAfter -gt $dumpCountBefore) {
    Write-Host ""
    Write-Warning "New crash dump(s) were written during the run: $crashDumps (symbolize with scripts\symbolize-crash.ps1)"
}

# Stability C3: surface runtime invariant failures written during this run.
if (Test-Path -LiteralPath $invariantLogPath) {
    $invariantFails = @(Select-String -LiteralPath $invariantLogPath -Pattern 'INVARIANT FAIL' -ErrorAction SilentlyContinue)
    $newInvariantFails = $invariantFails.Count - $invariantFailsBefore
    if ($newInvariantFails -gt 0) {
        Write-Host ""
        Write-Warning "INVARIANT FAILURES during this run in ${invariantLogPath}: $newInvariantFails line(s)"
        $invariantFails | Select-Object -Last ([Math]::Min($newInvariantFails, 10)) | ForEach-Object { Write-Host ("  " + $_.Line) -ForegroundColor Red }
        $allPass = $false
    }
}

Write-Host ""
Write-Host "Logs:" -ForegroundColor Cyan
Write-Host "  $runLog"
Write-Host "  $(Join-Path $appData 'project-load-diag.log')"
Write-Host "  $(Join-Path $appData 'track-delete-diag.log')"
Write-Host "  $(Join-Path $appData 'mixdown-diag.log')"
Write-Host "  $(Join-Path $appData 'last-operation.txt')"
Write-Host "  $crashDumps"

Write-Host ""
if ($allPass) {
    Write-Host "MATRIX RESULT: PASS" -ForegroundColor Green
    exit 0
} else {
    Write-Host "MATRIX RESULT: FAIL" -ForegroundColor Red
    exit 1
}
