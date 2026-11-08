# Updated C only: two repetitions, mirrored (profiled then control, then control then profiled).
# Records the original's hash, the device settings hash and the autosave log size before and after.
$S = Split-Path -Parent $MyInvocation.MyCommand.Path
$orig = "C:\Users\nicla\Dropbox\TWA Shared\DALprojects\Saved Projects\TSE_pt2_261106\TSE_pt2.dalproj"
$dev = "$env:APPDATA\MiniDAWLab\audio-device.xml"
$auto = "$env:APPDATA\MiniDAWLab\autosave-diag.log"
$state = { "original=$((Get-FileHash $orig).Hash) $((Get-Item $orig).LastWriteTime.ToString('o')) device=$((Get-FileHash $dev).Hash) autosaveLog=$((Get-Item $auto).Length) folderFiles=$((Get-ChildItem (Split-Path $orig) -File).Count)" }
"before: " + (& $state)
foreach ($r in @(@('prof', 'c3-r1-prof'), @('ctl', 'c3-r1-ctl'), @('ctl', 'c3-r2-ctl'), @('prof', 'c3-r2-prof'))) {
  "$(Get-Date -Format HH:mm:ss) start $($r[1])"
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$S\run2.ps1" -Case C -Mode $r[0] -Tag $r[1]
  if ($LASTEXITCODE -ne 0) { "series stopped at $($r[1]) (rc=$LASTEXITCODE)"; break }
  Start-Sleep -Seconds 5
}
"after: " + (& $state)
"$(Get-Date -Format HH:mm:ss) done"

