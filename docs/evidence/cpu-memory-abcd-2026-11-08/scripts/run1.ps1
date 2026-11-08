# One measured run: a fresh per-run copy of the case's project, the case's exe, the shared perf
# profile arguments, and an external sampler (every 250 ms) of DAL plus every helper process:
# cumulative CPU seconds, private bytes and working set. Never closes anything it did not start.
param([string]$Case, [string]$Mode, [string]$Tag)
$S = Split-Path -Parent $MyInvocation.MyCommand.Path
$D = "$env:TEMP\dal-cmp3"
$exeA = "C:\Users\nicla\development\MiniDAWLab-rs-base\build\ninja-release-base-vs\MiniDAWLab_artefacts\Release\MiniDAWLab.exe"
$exeB = "C:\Users\nicla\development\MiniDAWLab\dist\DanielssonsAudioLab-1.4.1\MiniDAWLab.exe"
$cases = @{
  A = @{ Exe = $exeA; Dir = "$D\TSE_pt2_261031"; Src = "AB.dalproj" }
  B = @{ Exe = $exeB; Dir = "$D\TSE_pt2_261031"; Src = "AB.dalproj" }
  C = @{ Exe = $exeB; Dir = "$D\TSE_pt2_261106"; Src = "C.dalproj" }
  D = @{ Exe = $exeB; Dir = "$D\TSE_pt2_261031"; Src = "D.dalproj" }
}
$c = $cases[$Case]
$common = "--seconds 30 --warmup 5 --start-seconds 17 --idle --idle-gap 0"
if ($Mode -eq "ctl") { $common += " --profile-off" }
$log = "$env:APPDATA\MiniDAWLab\stability-run.log"
$out = "$S\runs\$Tag"; New-Item -ItemType Directory -Force $out | Out-Null

$busy = Get-Process | Where-Object { $_.Name -match '^(MiniDAWLab|Cubase.*|cl|link|ninja|cmake|MSBuild|ISCC)$' }
if ($busy) { "blocked before ${Tag}: " + (($busy | ForEach-Object { "$($_.Name)($($_.Id))" }) -join ', '); exit 2 }

$proj = Join-Path $c.Dir "run-$Tag.dalproj"
Copy-Item (Join-Path $c.Dir $c.Src) $proj -Force
$crashBefore = @(Get-ChildItem "$env:APPDATA\MiniDAWLab\crash-dumps" -File -ErrorAction SilentlyContinue).Count
$proxyFilesBefore = @(Get-ChildItem (Join-Path $c.Dir "InstrumentProxies") -File).Count
$startLines = (Get-Content $log -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
"case=$Case mode=$Mode exe=$($c.Exe) sha256=$((Get-FileHash $c.Exe).Hash) project=$proj args=$common" | Out-File "$out\meta.txt"

$p = Start-Process -FilePath $c.Exe -ArgumentList "--stability-perf-profile `"$proj`" $common" -PassThru
"start=$($p.StartTime.ToString('o')) pid=$($p.Id)" | Out-File -Append "$out\meta.txt"
$rows = New-Object System.Collections.Generic.List[string]
$deadline = (Get-Date).AddMinutes(8)
while (-not $p.HasExited) {
  $t = (Get-Date).ToString('o')
  foreach ($q in @(Get-Process -Name MiniDAWLab -ErrorAction SilentlyContinue)) {
    try { $rows.Add("$t $($q.Id) $($q.TotalProcessorTime.TotalSeconds.ToString('F4')) $($q.PrivateMemorySize64) $($q.WorkingSet64) $($q.PeakWorkingSet64)") } catch {}
  }
  if ((Get-Date) -gt $deadline) { "TIMEOUT: run $Tag still running after 8 min; left running, series stops" | Out-File -Append "$out\meta.txt"; break }
  Start-Sleep -Milliseconds 250
}
$rows | Out-File "$out\samples.txt"
Get-Content $log | Select-Object -Skip $startLines | Out-File "$out\run.log"
$exit = if ($p.HasExited) { $p.ExitCode } else { "running" }
# every process from this run must be gone before the next one
$waitUntil = (Get-Date).AddSeconds(60)
while ((Get-Process -Name MiniDAWLab -ErrorAction SilentlyContinue) -and (Get-Date) -lt $waitUntil) { Start-Sleep -Milliseconds 500 }
$left = @(Get-Process -Name MiniDAWLab -ErrorAction SilentlyContinue).Count
$crashAfter = @(Get-ChildItem "$env:APPDATA\MiniDAWLab\crash-dumps" -File -ErrorAction SilentlyContinue).Count
$proxyFilesAfter = @(Get-ChildItem (Join-Path $c.Dir "InstrumentProxies") -File).Count
"exit=$exit leftProcesses=$left newCrashFiles=$($crashAfter - $crashBefore) proxyFiles=$proxyFilesBefore->$proxyFilesAfter" | Out-File -Append "$out\meta.txt"
Remove-Item $proj -ErrorAction SilentlyContinue
"$Tag exit=$exit left=$left newCrashFiles=$($crashAfter - $crashBefore) proxyFiles=$proxyFilesBefore->$proxyFilesAfter"
if ($left -gt 0 -or $exit -eq "running") { exit 3 }
exit 0
