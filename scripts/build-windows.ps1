#Requires -Version 5.1
<#
.SYNOPSIS
  Configures and builds MiniDAWLab with MSVC + Ninja using the same environment as Visual Studio.

.DESCRIPTION
  Invokes VsDevCmd.bat so cl.exe, the MSVC libraries, and Visual Studio's bundled Ninja are on PATH,
  then runs CMake configure + build. Use this from a normal PowerShell window when you do not want to
  open "Developer PowerShell for VS" manually.

  **Where the .exe is:** JUCE does not put MiniDAWLab.exe in the root of the build tree. After a
  successful build, from the **repo root** in PowerShell run (note the `.\` — required in PS):
  - Debug:   `.\build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe`
  - Release: `.\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe`

.PARAMETER Preset
  Optional. Use `windows-ninja-debug` (default) or `windows-ninja-release` to match the CMake
  presets in `CMakePresets.json`. If set, this overrides `-Config`.

.PARAMETER Config
  Debug (default) or Release. Ignored if `-Preset` is set.

.PARAMETER ConfigureOnly
  Run configure step only (cmake --preset), do not build.

.PARAMETER BuildOnly
  Run build only (cmake --build); assumes configure already ran for the chosen preset.
#>
param(
    [Parameter(Mandatory = $false)]
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',

    [Parameter(Mandatory = $false)]
    [ValidateSet('windows-ninja-debug', 'windows-ninja-release')]
    [string] $Preset,

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

# Resolve cmake.exe: not always on PATH in a plain PowerShell; prefer standalone CMake, else VS-bundled.
$cmakeCandidates = @(
    (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe'),
    (Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
)
$resolvedCmake = $null
foreach ($p in $cmakeCandidates) {
    if (Test-Path -LiteralPath $p) {
        $resolvedCmake = $p
        break
    }
}
if (-not $resolvedCmake) {
    Write-Error @"
Could not find cmake.exe. Install CMake from https://cmake.org/download/ and/or add it to PATH, or install the Visual Studio "CMake tools for Windows" / C++ workload component so CMake is under:
  Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
"@
}

$cmake = "`"$resolvedCmake`""
Write-Host "Using CMake: $resolvedCmake" -ForegroundColor DarkGray

if ($PSBoundParameters.ContainsKey('Preset') -and $Preset) {
    $cmakePreset = $Preset
} else {
    $cmakePreset = if ($Config -eq 'Release') { 'windows-ninja-release' } else { 'windows-ninja-debug' }
}

# Match CMakePresets.json; JUCE app output (see juce_add_gui_app)
$buildLeaf = if ($cmakePreset -eq 'windows-ninja-release') { 'ninja-release' } else { 'ninja-debug' }
$artefactConfig = if ($cmakePreset -eq 'windows-ninja-release') { 'Release' } else { 'Debug' }
Write-Host "Preset: $cmakePreset" -ForegroundColor DarkGray
Write-Host "After a successful build, from this repo in PowerShell run:" -ForegroundColor Cyan
Write-Host "  .\build\$buildLeaf\MiniDAWLab_artefacts\$artefactConfig\MiniDAWLab.exe" -ForegroundColor Cyan
Write-Host "  (PowerShell needs .\ in front; bare 'build\...' is not a path to an executable.)" -ForegroundColor DarkGray

$repoQuoted = "`"$repoRoot`""
# After `cd /d` the repo, %CD% is the repo — print the JUCE .exe path (cmd /c echo line).
$exeHintCmd = "echo === MiniDAWLab.exe (JUCE output) === && echo   %CD%\build\${buildLeaf}\MiniDAWLab_artefacts\${artefactConfig}\MiniDAWLab.exe"
# JUCE: `MiniDAWLab_artefacts\<Debug|Release>\MiniDAWLab.exe`. `--target MiniDAWLab` forces the app.
$buildTarget = '--target MiniDAWLab'

if ($BuildOnly) {
    $inner = "cd /d $repoQuoted && $cmake --build --preset $cmakePreset $buildTarget && $exeHintCmd"
} elseif ($ConfigureOnly) {
    $inner = "cd /d $repoQuoted && $cmake --preset $cmakePreset"
} else {
    $inner = "cd /d $repoQuoted && $cmake --preset $cmakePreset && $cmake --build --preset $cmakePreset $buildTarget && $exeHintCmd"
}

# Run the chain from a one-line .cmd on disk. Reasons:
# 1) `call` to VsDevCmd.bat is required in a .cmd; without it, the batch "hands off" and nothing
#    after VsDev runs (no CMake, instant return, no output).
# 2) A single /C argument to cmd is reliable; long lines passed from PowerShell can be misparsed.
$batch = "call `"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && $inner"
$expectedExe = Join-Path $repoRoot "build\${buildLeaf}\MiniDAWLab_artefacts\${artefactConfig}\MiniDAWLab.exe"
$tempCmd = Join-Path $env:TEMP "MiniDAWLab-build-$PID-$([IO.Path]::GetRandomFileName()).cmd"
Set-Content -Path $tempCmd -Value $batch -Encoding Oem
try {
    Write-Host "Running configure/build (this may take several minutes on a clean tree)..." -ForegroundColor DarkGray
    $p = Start-Process -FilePath $env:ComSpec -ArgumentList @('/C', $tempCmd) -NoNewWindow -Wait -PassThru
    $code = $p.ExitCode
} finally {
    Remove-Item -LiteralPath $tempCmd -Force -ErrorAction SilentlyContinue
}

if ($code -ne 0) {
    Write-Error "Build failed with exit code $code (see output above)."
    exit $code
}
if (-not $ConfigureOnly) {
    if (-not (Test-Path -LiteralPath $expectedExe)) {
        Write-Error "Build completed with code 0 but the executable is missing:`n  $expectedExe`n`nDelete build\$buildLeaf and re-run, or read errors above the exit code."
        exit 1
    }
    Write-Host "OK: " -NoNewline -ForegroundColor Green
    Write-Host $expectedExe
}
exit 0
