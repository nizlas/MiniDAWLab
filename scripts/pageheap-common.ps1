# =============================================================================
# pageheap-common.ps1 - shared helpers for the PageHeap scripts (Stability C4)
# =============================================================================
# Dot-sourced by enable-pageheap.ps1 / disable-pageheap.ps1 /
# run-stability-with-pageheap.ps1. Not meant to be run directly.
#
# PageHeap is configured through the Image File Execution Options (IFEO)
# registry key. `gflags /p /enable <image> /full` sets:
#   GlobalFlag    = 0x02000000  (FLG_HEAP_PAGE_ALLOCS)
#   PageHeapFlags = 0x3         (full page heap)
# under HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\
#   Image File Execution Options\<image>
# The registry fallback below writes/removes exactly those values.
# =============================================================================

$script:IfeoRoot = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
$script:PageHeapGlobalFlag = 0x02000000

function Assert-PageHeapElevation {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Error 'This script must be run from an elevated (Administrator) PowerShell window: PageHeap settings live under HKLM.'
    }
}

function Find-GflagsExe {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\gflags.exe",
        "${env:ProgramFiles(x86)}\Windows Kits\11\Debuggers\x64\gflags.exe",
        "$env:ProgramFiles\Windows Kits\10\Debuggers\x64\gflags.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    $onPath = Get-Command gflags.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

function Set-PageHeapRegistry {
    param(
        [Parameter(Mandatory = $true)][string]$ImageName,
        [switch]$Enable
    )
    $key = Join-Path $script:IfeoRoot $ImageName
    if ($Enable) {
        if (-not (Test-Path -LiteralPath $key)) {
            New-Item -Path $key -Force | Out-Null
        }
        # Preserve any other GlobalFlag bits that may already be set.
        $existing = 0
        $current = Get-ItemProperty -LiteralPath $key -Name 'GlobalFlag' -ErrorAction SilentlyContinue
        if ($current -and $current.GlobalFlag) {
            try { $existing = [Convert]::ToInt32(("$($current.GlobalFlag)" -replace '^0x', ''), 16) } catch { $existing = 0 }
        }
        $newFlags = $existing -bor $script:PageHeapGlobalFlag
        Set-ItemProperty -LiteralPath $key -Name 'GlobalFlag' -Value ('0x{0:x8}' -f $newFlags) -Type String
        Set-ItemProperty -LiteralPath $key -Name 'PageHeapFlags' -Value '0x3' -Type String
        Write-Host ("Registry: enabled full PageHeap for {0} (GlobalFlag=0x{1:x8}, PageHeapFlags=0x3)" -f $ImageName, $newFlags)
    } else {
        if (Test-Path -LiteralPath $key) {
            Remove-ItemProperty -LiteralPath $key -Name 'GlobalFlag' -ErrorAction SilentlyContinue
            Remove-ItemProperty -LiteralPath $key -Name 'PageHeapFlags' -ErrorAction SilentlyContinue
            # Remove the key if it is now empty (gflags leaves an empty key too; harmless either way).
            $remaining = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
            $names = @()
            if ($remaining) {
                $names = @($remaining.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { $_.Name })
            }
            if ($names.Count -eq 0 -and @(Get-ChildItem -LiteralPath $key -ErrorAction SilentlyContinue).Count -eq 0) {
                Remove-Item -LiteralPath $key -ErrorAction SilentlyContinue
            }
        }
        Write-Host "Registry: PageHeap values removed for $ImageName"
    }
}

function Get-PageHeapEnabled {
    param([Parameter(Mandatory = $true)][string]$ImageName)
    $key = Join-Path $script:IfeoRoot $ImageName
    if (-not (Test-Path -LiteralPath $key)) { return $false }
    $props = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
    if (-not $props -or -not $props.GlobalFlag) { return $false }
    $raw = "$($props.GlobalFlag)" -replace '^0x', ''
    try {
        $value = [Convert]::ToInt32($raw, 16)
    } catch {
        try { $value = [int]$props.GlobalFlag } catch { return $false }
    }
    return (($value -band $script:PageHeapGlobalFlag) -ne 0)
}

function Show-PageHeapStatus {
    param([Parameter(Mandatory = $true)][string]$ImageName)
    $enabled = Get-PageHeapEnabled -ImageName $ImageName
    if ($enabled) {
        Write-Host "PageHeap status for ${ImageName}: ENABLED" -ForegroundColor Yellow
    } else {
        Write-Host "PageHeap status for ${ImageName}: disabled" -ForegroundColor Green
    }
    $key = Join-Path $script:IfeoRoot $ImageName
    if (Test-Path -LiteralPath $key) {
        $props = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($props) {
            if ($props.PSObject.Properties['GlobalFlag'])    { Write-Host "  GlobalFlag    = $($props.GlobalFlag)" -ForegroundColor DarkGray }
            if ($props.PSObject.Properties['PageHeapFlags']) { Write-Host "  PageHeapFlags = $($props.PageHeapFlags)" -ForegroundColor DarkGray }
        }
    }
}
