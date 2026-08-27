# =============================================================================
# run-stability-with-pageheap.ps1 - stability matrix under full PageHeap (C4)
# =============================================================================
# DEVELOPER TESTING ONLY. Enables full PageHeap for MiniDAWLab.exe, runs the
# stability matrix, then ALWAYS disables PageHeap again (also on failure/Ctrl+C
# via finally). PageHeap is slow and very memory-heavy; expect each scenario to
# take several times longer than a normal run, and prefer small -Iterations.
#
# Usage (elevated PowerShell required):
#   .\scripts\run-stability-with-pageheap.ps1 -Project "C:\path\project.dalproj"
#   .\scripts\run-stability-with-pageheap.ps1 -Project ... -Iterations 3 -IncludeMixdown
# =============================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [string]$Exe = '',

    [int]$Iterations = 3,

    [switch]$IncludeMixdown,

    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'pageheap-common.ps1')
Assert-PageHeapElevation

Write-Warning 'PageHeap mode: the app runs MUCH slower and uses far more memory. This is for developer certification only.'

$matrix = Join-Path $PSScriptRoot 'stability-matrix.ps1'
$exitCode = 1

Write-Host ''
Write-Host '--- Enabling full PageHeap for MiniDAWLab.exe ---' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'enable-pageheap.ps1') | Write-Host

try {
    $matrixArgs = @{ Project = $Project; Iterations = $Iterations; TimeoutSec = $TimeoutSec }
    if ($Exe) { $matrixArgs.Exe = $Exe }
    if ($IncludeMixdown) { $matrixArgs.IncludeMixdown = $true }
    & $matrix @matrixArgs
    $exitCode = $LASTEXITCODE
}
finally {
    Write-Host ''
    Write-Host '--- Disabling PageHeap for MiniDAWLab.exe ---' -ForegroundColor Cyan
    try {
        & (Join-Path $PSScriptRoot 'disable-pageheap.ps1') | Write-Host
    } catch {
        Write-Warning "Failed to disable PageHeap automatically: $($_.Exception.Message)"
        Write-Warning 'Run .\scripts\disable-pageheap.ps1 manually from an elevated PowerShell.'
    }
}

exit $exitCode
