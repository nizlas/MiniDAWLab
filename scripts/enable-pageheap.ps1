# =============================================================================
# enable-pageheap.ps1 - enable full PageHeap for MiniDAWLab.exe (Stability C4)
# =============================================================================
# DEVELOPER TESTING ONLY. PageHeap makes the app slow and memory-heavy; it is
# used to catch use-after-free / heap corruption around delete/load/undo.
# Never leave it enabled after a test session (run disable-pageheap.ps1).
#
# Usage (elevated PowerShell required):
#   .\scripts\enable-pageheap.ps1
#
# What it does:
#   - Finds gflags.exe (Windows SDK "Debugging Tools for Windows") and runs:
#       gflags /p /enable MiniDAWLab.exe /full
#   - If gflags.exe is not installed, writes the equivalent Image File
#     Execution Options registry values directly.
#   - Prints the resulting PageHeap status.
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
    & $gflags /p /enable $ImageName /full | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Error "gflags failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Host 'gflags.exe not found - writing Image File Execution Options registry values directly.' -ForegroundColor DarkYellow
    Write-Host '(To get gflags.exe, install "Debugging Tools for Windows" from the Windows SDK installer:'
    Write-Host ' https://learn.microsoft.com/windows-hardware/drivers/debugger/ - typically installs to'
    Write-Host ' C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe)'
    Set-PageHeapRegistry -ImageName $ImageName -Enable
}

Write-Host ''
Show-PageHeapStatus -ImageName $ImageName
Write-Host ''
Write-Warning "PageHeap is now ENABLED for $ImageName. The app will be slow and use much more memory."
Write-Warning "When done testing, run: .\scripts\disable-pageheap.ps1"
