# =============================================================================
# certify-release.ps1 - pre-tester release certification gate (Stability C6)
# =============================================================================
# Runs the standard certification sequence and writes a Markdown report:
#   1. environment summary (git commit/branch/dirty, OS, paths)
#   2. Debug build
#   3. Debug stability matrix (-IncludeMixdown -IncludeAutosave)
#   4. log checks (invariants, gate timeouts, crash dumps, autosave, mixdown)
#   5. optional ASan targeted scenarios        (-IncludeAsan)
#   6. optional PageHeap matrix                (-IncludePageHeap, elevated PS)
#   7. Release build + package                 (unless -SkipPackage)
#   8. Release artifact verification (installer, zip, symbols, no PDB leak)
#   9. optional Release smoke                  (-IncludeReleaseSmoke)
#  10. report + log copies under dist\certification\<timestamp>\
#
# Exit code 0 = certification PASS (build approved for tester distribution),
# 1 = FAIL. Never deletes user logs; all checks are delta-based against
# baselines captured at start.
#
# Usage:
#   .\scripts\certify-release.ps1 -Project "C:\path\beakon_test5.dalproj"
#   .\scripts\certify-release.ps1 -Project ... -Iterations 5 -IncludeAsan
#   .\scripts\certify-release.ps1 -Project ... -IncludePageHeap   (elevated PS)
# =============================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [string]$Version = '',

    [int]$Iterations = 5,

    [switch]$IncludeAsan,

    [switch]$IncludePageHeap,

    [switch]$IncludeReleaseSmoke,

    [switch]$SkipPackage,

    [int]$TimeoutSec = 3600,

    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if (-not (Test-Path -LiteralPath $Project -PathType Leaf)) {
    # Fail before the ~100 s build+matrix work: a folder passes a bare Test-Path but makes
    # every stability scenario abort in-app with "project file not found".
    Write-Error "Reference project must be an existing .dalproj file, not a folder: $Project"
}
if ([System.IO.Path]::GetExtension($Project) -ne '.dalproj') {
    Write-Error "Reference project must be a .dalproj file: $Project"
}
$Project = (Resolve-Path -LiteralPath $Project).Path

if (-not $Version) {
    $raw = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    if ($raw -match 'project\s*\(\s*MiniDAWLab\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,2})') {
        $Version = $Matches[1]
    } else {
        $Version = 'unknown'
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot "dist\certification\$stamp"
}
$logDir = Join-Path $OutDir 'logs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$reportPath = Join-Path $OutDir 'CERTIFICATION_REPORT.md'

$appData = Join-Path $env:APPDATA 'MiniDAWLab'
$crashDumps = Join-Path $appData 'crash-dumps'
$debugExe = Join-Path $repoRoot 'build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe'
$asanExe = Join-Path $repoRoot 'build\ninja-asan\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe'
$releaseExe = Join-Path $repoRoot 'build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe'

# --- check collection ---------------------------------------------------------
$script:checks = @()
$script:commandsRun = @()
function Add-Check {
    param([string]$Name, [string]$Status, [string]$Detail = '', [int]$Seconds = 0)
    $script:checks += [pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail; Seconds = $Seconds }
    $color = switch ($Status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ("[{0,-7}] {1}{2}" -f $Status, $Name, $(if ($Detail) { " - $Detail" } else { '' })) -ForegroundColor $color
}

function Invoke-LoggedProcess {
    # Runs a child powershell/exe, streams to console AND a transcript file, returns exit code.
    param([string]$FilePath, [string[]]$Arguments, [string]$TranscriptFile, [int]$TimeoutSeconds = 7200)
    $script:commandsRun += ("{0} {1}" -f $FilePath, ($Arguments -join ' '))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $repoRoot
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $stderr = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill() } catch { }
        Set-Content -LiteralPath $TranscriptFile -Value "TIMEOUT after $TimeoutSeconds s`n"
        return -1
    }
    $proc.WaitForExit() # flush async readers / ensure ExitCode is populated
    $outText = $stdout.Result
    $errText = $stderr.Result
    Set-Content -LiteralPath $TranscriptFile -Value ($outText + "`n--- stderr ---`n" + $errText)
    if ($outText) { Write-Host $outText }
    if ($errText) { Write-Host $errText -ForegroundColor DarkYellow }
    return $proc.ExitCode
}

function Get-PatternCount {
    param([string]$Path, [string]$Pattern)
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    return @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue).Count
}

# --- 1. environment summary ---------------------------------------------------
Write-Host "================ Release certification (C6) ================" -ForegroundColor Cyan
$startedAt = Get-Date
$gitCommit = ''
$gitBranch = ''
$gitDirty = 'unknown (git not available)'
try {
    $gitCommit = (& git -C $repoRoot rev-parse --short HEAD 2>$null) -join ''
    $gitBranch = (& git -C $repoRoot rev-parse --abbrev-ref HEAD 2>$null) -join ''
    $dirtyLines = @(& git -C $repoRoot status --porcelain 2>$null)
    $gitDirty = if ($dirtyLines.Count -gt 0) { "DIRTY ($($dirtyLines.Count) changed/untracked paths)" } else { 'clean' }
} catch { }
$osVersion = try { (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).Version } catch { [System.Environment]::OSVersion.VersionString }

Write-Host "Version:   $Version"
Write-Host "Project:   $Project"
Write-Host "Git:       $gitBranch @ $gitCommit ($gitDirty)"
Write-Host "OS:        $osVersion"
Write-Host "OutDir:    $OutDir"
Write-Host "Options:   Iterations=$Iterations ASan=$IncludeAsan PageHeap=$IncludePageHeap SkipPackage=$SkipPackage ReleaseSmoke=$IncludeReleaseSmoke"
Write-Host ""

# --- baselines for delta-based log checks (never clear user logs) -------------
$baseline = @{
    invariantFails = Get-PatternCount (Join-Path $appData 'stability-invariant.log') 'INVARIANT FAIL'
    runFails       = Get-PatternCount (Join-Path $appData 'stability-run.log') 'RESULT: FAIL'
    gateTimeouts   = Get-PatternCount (Join-Path $appData 'track-delete-diag.log') 'timeout=YES'
    mixdownFails   = Get-PatternCount (Join-Path $appData 'mixdown-diag.log') 'FAIL'
    autosaveWrites = Get-PatternCount (Join-Path $appData 'autosave-diag.log') 'write ok'
    autosaveFails  = Get-PatternCount (Join-Path $appData 'autosave-diag.log') 'write FAIL'
    dumpCount      = if (Test-Path -LiteralPath $crashDumps) { @(Get-ChildItem -LiteralPath $crashDumps -Filter '*.dmp' -ErrorAction SilentlyContinue).Count } else { 0 }
}
$certStartTime = Get-Date

# --- 2. Debug build ------------------------------------------------------------
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$code = Invoke-LoggedProcess 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'build-windows.ps1'), '-Config', 'Debug') (Join-Path $logDir 'build-debug.txt') 1800
$sw.Stop()
Add-Check 'Debug build' $(if ($code -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$code" ([int]$sw.Elapsed.TotalSeconds)
if ($code -ne 0) {
    # Everything downstream depends on a working build; report and bail out early.
    Write-Warning 'Debug build failed - aborting certification (report still written).'
}

$debugBuildOk = ($code -eq 0)

# --- 3. Debug stability matrix ---------------------------------------------------
if ($debugBuildOk) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $code = Invoke-LoggedProcess 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'stability-matrix.ps1'), '-Project', $Project, '-Iterations', "$Iterations", '-IncludeMixdown', '-IncludeAutosave', '-TimeoutSec', "$TimeoutSec") (Join-Path $logDir 'matrix-debug.txt') ($TimeoutSec * 8)
    $sw.Stop()
    Add-Check 'Debug stability matrix (mixdown + autosave)' $(if ($code -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$code" ([int]$sw.Elapsed.TotalSeconds)
} else {
    Add-Check 'Debug stability matrix (mixdown + autosave)' 'SKIPPED' 'Debug build failed'
}

# --- 5. optional ASan ------------------------------------------------------------
if ($IncludeAsan) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $code = Invoke-LoggedProcess 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'build-windows.ps1'), '-Config', 'Asan') (Join-Path $logDir 'build-asan.txt') 1800
    $sw.Stop()
    Add-Check 'ASan build' $(if ($code -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$code" ([int]$sw.Elapsed.TotalSeconds)

    if ($code -eq 0) {
        $asanScenarios = @(
            @{ Name = 'ASan smoke';       Args = @('--stability-smoke', $Project);                          Log = 'asan-smoke' },
            @{ Name = 'ASan delete-loop'; Args = @('--stability-delete-loop', $Project, '--iterations', '2'); Log = 'asan-delete-loop' },
            @{ Name = 'ASan autosave';    Args = @('--stability-autosave', $Project);                       Log = 'asan-autosave' }
        )
        foreach ($s in $asanScenarios) {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $transcript = Join-Path $logDir "$($s.Log).txt"
            $code = Invoke-LoggedProcess $asanExe $s.Args $transcript $TimeoutSec
            $sw.Stop()
            $asanErrors = Get-PatternCount $transcript 'ERROR: AddressSanitizer'
            $status = if ($code -eq 0 -and $asanErrors -eq 0) { 'PASS' } else { 'FAIL' }
            Add-Check $s.Name $status "exit=$code, ASan reports=$asanErrors" ([int]$sw.Elapsed.TotalSeconds)
        }
    } else {
        Add-Check 'ASan scenarios' 'SKIPPED' 'ASan build failed'
    }
} else {
    Add-Check 'ASan targeted scenarios' 'SKIPPED' 'not requested (-IncludeAsan)'
}

# --- 6. optional PageHeap ---------------------------------------------------------
if ($IncludePageHeap) {
    . (Join-Path $PSScriptRoot 'pageheap-common.ps1')
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Add-Check 'PageHeap matrix' 'FAIL' 'requires an elevated (Administrator) PowerShell'
    } else {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-LoggedProcess 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'stability-matrix.ps1'), '-Project', $Project, '-Iterations', '2', '-PageHeap', '-TimeoutSec', "$TimeoutSec") (Join-Path $logDir 'matrix-pageheap.txt') ($TimeoutSec * 8)
        $sw.Stop()
        Add-Check 'PageHeap matrix' $(if ($code -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$code" ([int]$sw.Elapsed.TotalSeconds)
        $stillEnabled = Get-PageHeapEnabled -ImageName 'MiniDAWLab.exe'
        if ($stillEnabled) {
            try { & (Join-Path $PSScriptRoot 'disable-pageheap.ps1') | Out-Null } catch { }
            $stillEnabled = Get-PageHeapEnabled -ImageName 'MiniDAWLab.exe'
        }
        Add-Check 'PageHeap disabled afterward' $(if (-not $stillEnabled) { 'PASS' } else { 'FAIL' }) $(if ($stillEnabled) { 'STILL ENABLED - run disable-pageheap.ps1' } else { 'disabled' })
    }
} else {
    Add-Check 'PageHeap matrix' 'SKIPPED' 'not requested (-IncludePageHeap)'
}

# --- 7. Release build + package -----------------------------------------------------
$setupPath = Join-Path $repoRoot "dist\DanielssonsAudioLab-$Version-Setup.exe"
$zipPath = Join-Path $repoRoot "dist\DanielssonsAudioLab-$Version.zip"
$symbolsDir = Join-Path $repoRoot "dist\symbols\DanielssonsAudioLab-$Version"
$stageDir = Join-Path $repoRoot "dist\DanielssonsAudioLab-$Version"
if (-not $SkipPackage) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $code = Invoke-LoggedProcess 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'package-windows.ps1'), '-Version', $Version) (Join-Path $logDir 'package-release.txt') 3600
    $sw.Stop()
    Add-Check 'Release build + package' $(if ($code -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$code" ([int]$sw.Elapsed.TotalSeconds)

    # --- 8. artifact verification ---
    $setupOk = (Test-Path -LiteralPath $setupPath) -and ((Get-Item -LiteralPath $setupPath).Length -gt 0)
    Add-Check 'Installer exists' $(if ($setupOk) { 'PASS' } else { 'FAIL' }) $setupPath
    $zipOk = (Test-Path -LiteralPath $zipPath) -and ((Get-Item -LiteralPath $zipPath).Length -gt 0)
    Add-Check 'Zip exists' $(if ($zipOk) { 'PASS' } else { 'FAIL' }) $zipPath
    $symbolsOk = (Test-Path -LiteralPath (Join-Path $symbolsDir 'MiniDAWLab.exe')) -and (Test-Path -LiteralPath (Join-Path $symbolsDir 'MiniDAWLab.pdb'))
    Add-Check 'Symbols archived (exe + PDB)' $(if ($symbolsOk) { 'PASS' } else { 'FAIL' }) $symbolsDir
    $pdbLeak = @(Get-ChildItem -Path $stageDir -Filter '*.pdb' -Recurse -ErrorAction SilentlyContinue).Count
    Add-Check 'No PDB inside staged tree/installer payload' $(if ($pdbLeak -eq 0) { 'PASS' } else { 'FAIL' }) "PDB files in staged tree: $pdbLeak"
    Add-Check 'Symbolization script exists' $(if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'symbolize-crash.ps1')) { 'PASS' } else { 'FAIL' }) 'scripts\symbolize-crash.ps1'
} else {
    Add-Check 'Release build + package' 'SKIPPED' '-SkipPackage'
}

# --- 9. optional Release smoke -----------------------------------------------------
if ($IncludeReleaseSmoke) {
    if (Test-Path -LiteralPath $releaseExe) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-LoggedProcess $releaseExe @('--stability-smoke', $Project) (Join-Path $logDir 'release-smoke.txt') $TimeoutSec
        $sw.Stop()
        Add-Check 'Release smoke' $(if ($code -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$code" ([int]$sw.Elapsed.TotalSeconds)
    } else {
        Add-Check 'Release smoke' 'FAIL' "Release exe not found: $releaseExe"
    }
} else {
    Add-Check 'Release smoke' 'SKIPPED' 'not requested (-IncludeReleaseSmoke); manual smoke recommended'
}

# --- 4. log checks, delta vs baseline (covers Debug matrix + ASan + PageHeap runs) ---
$newRunFails = (Get-PatternCount (Join-Path $appData 'stability-run.log') 'RESULT: FAIL') - $baseline.runFails
Add-Check 'No FAIL results in stability-run.log' $(if ($newRunFails -eq 0) { 'PASS' } else { 'FAIL' }) "new 'RESULT: FAIL' lines: $newRunFails"

$newInvariantFails = (Get-PatternCount (Join-Path $appData 'stability-invariant.log') 'INVARIANT FAIL') - $baseline.invariantFails
Add-Check 'No invariant failures' $(if ($newInvariantFails -eq 0) { 'PASS' } else { 'FAIL' }) "new INVARIANT FAIL lines: $newInvariantFails"

$newGateTimeouts = (Get-PatternCount (Join-Path $appData 'track-delete-diag.log') 'timeout=YES') - $baseline.gateTimeouts
Add-Check 'No gate timeout regressions' $(if ($newGateTimeouts -eq 0) { 'PASS' } else { 'FAIL' }) "new timeout=YES lines: $newGateTimeouts"

$newMixdownFails = (Get-PatternCount (Join-Path $appData 'mixdown-diag.log') 'FAIL') - $baseline.mixdownFails
Add-Check 'No failed mixdown exports' $(if ($newMixdownFails -eq 0) { 'PASS' } else { 'FAIL' }) "new FAIL lines in mixdown-diag.log: $newMixdownFails"

$newAutosaveWrites = (Get-PatternCount (Join-Path $appData 'autosave-diag.log') 'write ok') - $baseline.autosaveWrites
$newAutosaveFails = (Get-PatternCount (Join-Path $appData 'autosave-diag.log') 'write FAIL') - $baseline.autosaveFails
Add-Check 'Autosave wrote and did not fail' $(if ($newAutosaveWrites -gt 0 -and $newAutosaveFails -eq 0) { 'PASS' } else { 'FAIL' }) "new 'write ok': $newAutosaveWrites, new 'write FAIL': $newAutosaveFails"

# --- crash dump check (whole certification window) ----------------------------------
$dumpCountAfter = if (Test-Path -LiteralPath $crashDumps) { @(Get-ChildItem -LiteralPath $crashDumps -Filter '*.dmp' -ErrorAction SilentlyContinue).Count } else { 0 }
$newDumps = $dumpCountAfter - $baseline.dumpCount
Add-Check 'No new crash dumps' $(if ($newDumps -eq 0) { 'PASS' } else { 'FAIL' }) "new dumps: $newDumps"

# --- Part D: preserve logs ------------------------------------------------------------
$logsToCopy = @('stability-run.log', 'stability-invariant.log', 'track-delete-diag.log',
                'mixdown-diag.log', 'autosave-diag.log', 'project-load-diag.log', 'last-operation.txt')
foreach ($name in $logsToCopy) {
    $src = Join-Path $appData $name
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $logDir $name) -Force
    }
}
if ($newDumps -gt 0 -and (Test-Path -LiteralPath $crashDumps)) {
    $dumpOut = Join-Path $OutDir 'crash-dumps'
    New-Item -ItemType Directory -Path $dumpOut -Force | Out-Null
    Get-ChildItem -LiteralPath $crashDumps -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $certStartTime } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dumpOut $_.Name) -Force }
}

# --- 10. report -------------------------------------------------------------------------
$failCount = @($script:checks | Where-Object { $_.Status -eq 'FAIL' }).Count
$approved = ($failCount -eq 0)
$totalSeconds = [int]((Get-Date) - $startedAt).TotalSeconds

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# Release certification report - Danielssons Audio Lab $Version")
[void]$md.AppendLine('')
[void]$md.AppendLine("**Verdict: $(if ($approved) { 'PASS - approved for tester distribution' } else { 'FAIL - do NOT send this build to testers' })**")
[void]$md.AppendLine('')
[void]$md.AppendLine("| | |")
[void]$md.AppendLine("|---|---|")
[void]$md.AppendLine("| Date | $($startedAt.ToString('yyyy-MM-dd HH:mm:ss')) (total $totalSeconds s) |")
[void]$md.AppendLine("| Version | $Version |")
[void]$md.AppendLine("| Git | $gitBranch @ $gitCommit ($gitDirty) |")
[void]$md.AppendLine("| OS | $osVersion |")
[void]$md.AppendLine("| Reference project | $Project |")
[void]$md.AppendLine("| Iterations | $Iterations |")
[void]$md.AppendLine("| ASan / PageHeap / ReleaseSmoke | $IncludeAsan / $IncludePageHeap / $IncludeReleaseSmoke |")
[void]$md.AppendLine('')
[void]$md.AppendLine('## Checks')
[void]$md.AppendLine('')
[void]$md.AppendLine('| Check | Status | Detail | Seconds |')
[void]$md.AppendLine('|---|---|---|---|')
foreach ($c in $script:checks) {
    [void]$md.AppendLine("| $($c.Name) | $($c.Status) | $($c.Detail) | $($c.Seconds) |")
}
[void]$md.AppendLine('')
[void]$md.AppendLine('## Artifacts')
[void]$md.AppendLine('')
[void]$md.AppendLine("- Installer (send to tester): ``$setupPath``")
[void]$md.AppendLine("- Zip: ``$zipPath``")
[void]$md.AppendLine("- Symbols (keep internal, never send): ``$symbolsDir``")
[void]$md.AppendLine("- Logs from this run: ``$logDir``")
[void]$md.AppendLine('')
[void]$md.AppendLine('## Commands run')
[void]$md.AppendLine('')
foreach ($cmd in $script:commandsRun) {
    [void]$md.AppendLine("- ``$cmd``")
}
[void]$md.AppendLine('')
[void]$md.AppendLine('## Known limitations')
[void]$md.AppendLine('')
[void]$md.AppendLine('- Third-party VST3 plugins (AmpliTube, Steinberg, ...) can crash intermittently inside their own code; such crashes are diagnosed via minidump + symbolization, not blocked by this gate.')
[void]$md.AppendLine('- ASan/PageHeap runs are developer diagnostics; their builds are never tester artifacts.')
[void]$md.AppendLine('- Release smoke is optional/manual unless -IncludeReleaseSmoke was passed.')
[void]$md.AppendLine('- See docs/RELEASE_CERTIFICATION.md and docs/STABILITY_TESTING.md.')
Set-Content -LiteralPath $reportPath -Value $md.ToString() -Encoding UTF8

Write-Host ''
Write-Host "================ Certification summary ================" -ForegroundColor Cyan
foreach ($c in $script:checks) {
    $color = switch ($c.Status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ("  {0,-45} {1,-8} {2}" -f $c.Name, $c.Status, $c.Detail) -ForegroundColor $color
}
Write-Host ''
Write-Host "Report: $reportPath"
if ($approved) {
    Write-Host 'CERTIFICATION RESULT: PASS - approved for tester distribution (send the Release installer only).' -ForegroundColor Green
    exit 0
} else {
    Write-Host "CERTIFICATION RESULT: FAIL ($failCount failing check(s)) - do NOT send this build to testers." -ForegroundColor Red
    exit 1
}
