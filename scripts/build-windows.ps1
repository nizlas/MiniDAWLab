#Requires -Version 5.1
<#
.SYNOPSIS
  Configures and builds MiniDAWLab with MSVC + Ninja using the same environment as Visual Studio.

.DESCRIPTION
  Invokes VsDevCmd.bat so cl.exe, the MSVC libraries, and Visual Studio's bundled Ninja are on PATH,
  then runs CMake configure + build. Use this from a normal PowerShell window when you do not want to
  open "Developer PowerShell for VS" manually.

.PARAMETER Config
  Debug (default) or Release.

.PARAMETER ConfigureOnly
  Run configure step only (cmake --preset), do not build.

.PARAMETER BuildOnly
  Run build only (cmake --build); assumes configure already ran for the chosen preset.
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',

    [switch] $ConfigureOnly,
    [switch] $BuildOnly
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Install Visual Studio 2022 or later with the Desktop development with C++ workload."
}

$installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installPath) {
    Write-Error "No Visual Studio installation with MSVC x64 tools found (component VC.Tools.x86.x64)."
}

$devCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $devCmd)) {
    Write-Error "VsDevCmd.bat not found at: $devCmd"
}

$preset = if ($Config -eq 'Release') { 'windows-ninja-release' } else { 'windows-ninja-debug' }
$cmake = 'cmake'
$cmakePath = Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'
if (Test-Path $cmakePath) {
    $cmake = "`"$cmakePath`""
}

$repoQuoted = "`"$repoRoot`""

if ($BuildOnly) {
    $inner = "cd /d $repoQuoted && $cmake --build --preset $preset"
} elseif ($ConfigureOnly) {
    $inner = "cd /d $repoQuoted && $cmake --preset $preset"
} else {
    $inner = "cd /d $repoQuoted && $cmake --preset $preset && $cmake --build --preset $preset"
}

$batch = "`"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && $inner"
exit (cmd /c $batch)
