#Requires -Version 5.1
<#
.SYNOPSIS
  Stages a Release tree under dist, downloads the Microsoft VC++ x64 redistributable once, builds a
  versioned zip, and (when Inno Setup’s ISCC.exe is on PATH or in a default install path) compiles
  a Windows Setup bundle.

.DESCRIPTION
  - Parses the project version from the top-level CMakeLists.txt (project(MiniDAWLab VERSION …)).
  - Optionally invokes scripts\build-windows.ps1 -Config Release (default: on).
  - Copies MiniDAWLab.exe and optional docs into dist\DanielssonsAudioLab-<version>\.
  - Ensures dist\vendor\vc_redist.x64.exe from https://aka.ms/vc14/vc_redist.x64.exe (one-time).
  - Writes dist\DanielssonsAudioLab-<version>.zip
  - If ISCC is found, runs: ISCC /DAppVersion=<version> installer\MiniDAWLab.iss
    Output: dist\DanielssonsAudioLab-<version>-Setup.exe (see installer\MiniDAWLab.iss)
#>
param(
    [Parameter(Mandatory = $false)]
    [string] $Version,

    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'

# User-facing app name is "Danielssons Audio Lab" (Main.cpp / installer AppName). Bundle/zip/setup
# filenames use an ASCII token; executable and CMake target remain MiniDAWLab.
$packageBundleName = 'DanielssonsAudioLab'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cmakeLists = Join-Path $repoRoot 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $cmakeLists)) {
    Write-Error "CMakeLists.txt not found at: $cmakeLists"
}

if (-not $Version) {
    $raw = Get-Content -LiteralPath $cmakeLists -Raw
    if ($raw -notmatch 'project\s*\(\s*MiniDAWLab\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,2})') {
        Write-Error "Could not parse project version from CMakeLists.txt (expected: project(MiniDAWLab VERSION a.b.c))."
    }
    $Version = $Matches[1]
}

$releaseExe = Join-Path $repoRoot "build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe"
$distRoot = Join-Path $repoRoot 'dist'
$vendorDir = Join-Path $distRoot 'vendor'
$vcUrl = 'https://aka.ms/vc14/vc_redist.x64.exe'
$vcName = 'vc_redist.x64.exe'
$vcPath = Join-Path $vendorDir $vcName
$stageDir = Join-Path $distRoot "$packageBundleName-$Version"
$zipPath = Join-Path $distRoot "$packageBundleName-$Version.zip"

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release
}

if (-not (Test-Path -LiteralPath $releaseExe)) {
    Write-Error @"
Release executable not found:
  $releaseExe
Run (from repo): .\scripts\build-windows.ps1 -Config Release
Or pass -SkipBuild if you have already built Release.
"@
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
New-Item -ItemType Directory -Path $vendorDir -Force | Out-Null

$needRedist = $true
if (Test-Path -LiteralPath $vcPath) {
    $len = (Get-Item -LiteralPath $vcPath).Length
    if ($len -gt 0) { $needRedist = $false }
}
if ($needRedist) {
    Write-Host "Downloading VC++ x64 redistributable to: $vcPath" -ForegroundColor Cyan
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try {
        Invoke-WebRequest -Uri $vcUrl -OutFile $vcPath -UseBasicParsing
    } catch {
        Write-Error "Failed to download $vcUrl : $_"
    }
    if (-not (Test-Path -LiteralPath $vcPath) -or (Get-Item -LiteralPath $vcPath).Length -eq 0) {
        Write-Error "Downloaded redistributable is missing or empty: $vcPath"
    }
} else {
    Write-Host "Using existing: $vcPath" -ForegroundColor DarkGray
}

if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

Copy-Item -LiteralPath $releaseExe -Destination (Join-Path $stageDir 'MiniDAWLab.exe') -Force
$readmes = @('README.md', 'PROJECT_BRIEF.md')
foreach ($f in $readmes) {
    $p = Join-Path $repoRoot $f
    if (Test-Path -LiteralPath $p) {
        Copy-Item -LiteralPath $p -Destination (Join-Path $stageDir $f) -Force
    }
}

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
# includeBaseDirectory: true → one top-level folder in the zip (matches dist\DanielssonsAudioLab-<ver>\)
[IO.Compression.ZipFile]::CreateFromDirectory($stageDir, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Host "Wrote: $zipPath" -ForegroundColor Green

function Get-IsccPath {
    $onPath = Get-Command ISCC -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    )
    if ($env:LOCALAPPDATA) {
        $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    }
    foreach ($p in $candidates) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

$iscc = Get-IsccPath

$iss = Join-Path $repoRoot 'installer\MiniDAWLab.iss'
if (-not (Test-Path -LiteralPath $iss)) {
    Write-Error "Inno script not found: $iss"
}

if ($iscc) {
    Write-Host "Found ISCC.exe: $iscc" -ForegroundColor Cyan
    Write-Host "Compiling installer..." -ForegroundColor Cyan
    $argList = @("/DAppVersion=$Version", $iss)
    $p = Start-Process -FilePath $iscc -ArgumentList $argList -NoNewWindow -Wait -PassThru -WorkingDirectory $repoRoot
    if ($p.ExitCode -ne 0) {
        Write-Error "ISCC failed with exit code $($p.ExitCode)."
    }
    $setup = Join-Path $distRoot "$packageBundleName-$Version-Setup.exe"
    if (Test-Path -LiteralPath $setup) {
        Write-Host "Wrote: $setup" -ForegroundColor Green
    } else {
        Write-Warning "ISCC returned 0 but expected output was not found: $setup"
    }
} else {
    Write-Warning "Inno Setup 6 (ISCC.exe) not found. Installed: install Inno Setup 6, then re-run. Zip and staged tree are still under dist\."
    Write-Host "  Manual: `"${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe`" /DAppVersion=$Version `"$iss`""
    if ($env:LOCALAPPDATA) {
        $la = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
        Write-Host "  Or (per-user / winget): `"$la`" /DAppVersion=$Version `"$iss`""
    }
    exit 0
}
exit 0
