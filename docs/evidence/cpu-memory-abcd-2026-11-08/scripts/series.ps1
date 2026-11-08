# Two rounds in opposite order: A B C D (profiled, then control) and D C B A (control, then profiled).
$S = Split-Path -Parent $MyInvocation.MyCommand.Path
$plan = @()
foreach ($c in 'A', 'B', 'C', 'D') { $plan += ,@($c, 'prof', "r1-$c-prof"); $plan += ,@($c, 'ctl', "r1-$c-ctl") }
foreach ($c in 'D', 'C', 'B', 'A') { $plan += ,@($c, 'ctl', "r2-$c-ctl"); $plan += ,@($c, 'prof', "r2-$c-prof") }
foreach ($r in $plan) {
  "$(Get-Date -Format HH:mm:ss) start $($r[2])"
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$S\run1.ps1" -Case $r[0] -Mode $r[1] -Tag $r[2]
  if ($LASTEXITCODE -ne 0) { "series stopped at $($r[2]) (rc=$LASTEXITCODE)"; exit 1 }
  Start-Sleep -Seconds 5
}
"$(Get-Date -Format HH:mm:ss) series done"
