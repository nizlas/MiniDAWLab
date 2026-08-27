# =============================================================================
# disable-pageheap.ps1 - disable PageHeap for MiniDAWLab.exe (Stability C4)
# =============================================================================
# Usage (elevated PowerShell required):
#   .\scripts\disable-pageheap.ps1
#
# Runs `gflags /p /disable MiniDAWLab.exe` (or removes the equivalent registry
# values if gflags.exe is not installed) and prints the resulting status.
# =============================================================================

[CmdletBinding()]
param(
    [string]$ImageName = 'MiniDAWLab.exe'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'pageheap-common.ps1')

Assert-PageHeapElevation

$gflags = Find-GflagsExe
if ($gflags) {
    Write-Host "Using gflags: $gflags" -ForegroundColor DarkGray
    & $gflags /p /disable $ImageName | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Error "gflags failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Host 'gflags.exe not found - removing Image File Execution Options registry values directly.' -ForegroundColor DarkYellow
    Set-PageHeapRegistry -ImageName $ImageName
}

Write-Host ''
Show-PageHeapStatus -ImageName $ImageName
