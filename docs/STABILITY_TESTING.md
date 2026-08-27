# Stability testing: ASan and PageHeap (Stability C4)

Developer-only memory diagnostics for MiniDAWLab (DAL). These modes catch
use-after-free, heap-buffer-overflow and heap corruption that normal Debug
builds miss.

> **WARNING — developer testing only.**
> Never send an ASan build or a PageHeap-enabled machine setup to Conny or any
> tester as a normal test build. The normal tester build is the **Release
> installer** produced by `.\scripts\package-windows.ps1`. ASan/PageHeap builds
> are slow, memory-heavy, and abort on the first detected error.

---

## 1. What ASan is and when to use it

AddressSanitizer (`/fsanitize=address`, MSVC) instruments every memory access
at compile time. When the app reads or writes freed/out-of-bounds memory, ASan
prints a detailed report (access stack, allocation stack, free stack) to
stderr and aborts.

Use ASan when you want *root-cause quality* reports for memory bugs:

- load-loop
- delete-loop, preferably on plugin-light or plugin-free projects
- UI/session/routing scenarios

Caveats:

- The app runs **several times slower** (a smoke run that takes ~1 minute
  normally takes ~10 minutes under ASan).
- ASan may report or crash **inside third-party VST3 plugins** (Steinberg,
  BBC, FabFilter...). Plugin internals are not our code; do not make ASan
  certification depend on them unless a plugin-specific report is proven
  reproducible and relevant.

## 2. What PageHeap is and when to use it

PageHeap (part of Windows Application Verifier / GFlags) is an OS-level heap
mode: each allocation is placed at the end of a page with a protected guard
page after it, and freed pages are protected. Use-after-free and heap overrun
then crash **immediately at the faulting instruction** instead of corrupting
memory silently. No special build is needed — it works with the normal Debug
or Release exe.

Use PageHeap for catching use-after-free / heap corruption around
delete/load/undo:

- delete-loop
- load-loop
- smoke

It works with plugin-heavy projects too, but it is slow and very
memory-heavy (every allocation costs at least one page).

## 3. How to build ASan

The CMake preset `windows-ninja-asan` (in `CMakePresets.json`) builds Debug
with `/fsanitize=address`, full PDB symbols, `/INCREMENTAL:NO` linking and
**without** `/RTC1` (runtime checks are incompatible with ASan). Existing
Debug/Release presets are untouched.

```powershell
.\scripts\build-windows.ps1 -Config Asan
# equivalent manual commands (from a VS developer prompt):
#   cmake --preset windows-ninja-asan
#   cmake --build --preset windows-ninja-asan --target MiniDAWLab
```

Output exe:

```
build\ninja-asan\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe
```

The build script copies the dynamic ASan runtime DLLs
(`clang_rt.asan_dbg_dynamic-x86_64.dll`, `clang_rt.asan_dynamic-x86_64.dll`)
next to the exe so it starts from a plain PowerShell window. If you build with
the manual cmake commands instead, run the exe from a VS developer prompt or
copy those DLLs from
`...\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\` yourself.

To capture an ASan report, redirect stderr, e.g.:

```powershell
& .\build\ninja-asan\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe --stability-smoke "C:\path\project.dalproj" 2> asan-report.txt
```

Or run the whole matrix against the ASan exe:

```powershell
.\scripts\stability-matrix.ps1 -Project "C:\path\project.dalproj" -Exe ".\build\ninja-asan\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe" -TimeoutSec 3600
```

(The matrix prints `Mode: ASan build = YES ...` when it detects the ASan exe.)

## 4. How to run the stability matrix normally

```powershell
.\scripts\build-windows.ps1 -Config Debug
.\scripts\stability-matrix.ps1 -Project "C:\path\project.dalproj" -Iterations 5 -IncludeMixdown
```

Exit code 0 = all scenarios passed and no new invariant failures.

## 5. How to run the stability matrix under PageHeap

From an **elevated** PowerShell (PageHeap settings live under HKLM):

```powershell
# all-in-one wrapper: enables PageHeap, runs the matrix, always disables again
.\scripts\run-stability-with-pageheap.ps1 -Project "C:\path\project.dalproj" -Iterations 3

# or via the matrix switch
.\scripts\stability-matrix.ps1 -Project "C:\path\project.dalproj" -PageHeap -TimeoutSec 3600

# or manually
.\scripts\enable-pageheap.ps1     # gflags /p /enable MiniDAWLab.exe /full
.\scripts\stability-matrix.ps1 -Project "C:\path\project.dalproj"
.\scripts\disable-pageheap.ps1    # gflags /p /disable MiniDAWLab.exe
```

The enable/disable scripts use `gflags.exe` from the Windows SDK "Debugging
Tools for Windows" if installed (typically
`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe`); otherwise
they write/remove the equivalent Image File Execution Options registry values
directly. Both print the resulting status. **Always confirm the status line
says `disabled` when you are done** — PageHeap left enabled makes every future
launch of MiniDAWLab.exe slow.

## 6. How to interpret results

- **ASan reports** (stderr): the first block names the bug class
  (`heap-use-after-free`, `heap-buffer-overflow`...) and the accessing
  thread/stack; the following blocks show where the memory was freed and
  allocated. Frames in `src\...` are ours; frames only inside plugin DLLs are
  third-party noise. The process aborts after the first report
  (`==<pid>==ABORTING`), so a scenario that dies mid-run with a report is a
  FAIL with the root cause already in hand.
- **PageHeap crashes**: the app crashes (access violation) at the exact
  faulting instruction instead of printing a report. The crash handler writes
  a minidump as usual — symbolize it (see below); the faulting stack *is* the
  bug site for use-after-free/overrun.
- **Minidumps**: `%APPDATA%\MiniDAWLab\crash-dumps\*.dmp`, symbolize with
  `.\scripts\symbolize-crash.ps1`.
- **stability-run.log** (`%APPDATA%\MiniDAWLab\`): per-step begin/end lines
  from the scenario runner; the last `step begin` without a matching
  `step end ok` shows which step was executing when the app died.
- **stability-invariant.log** (`%APPDATA%\MiniDAWLab\`): `INVARIANT FAIL`
  lines from the C3 runtime invariant checks; the matrix fails when new lines
  appear during a run.

## 7. What to do after a failure

1. Save the crash dump(s) from `%APPDATA%\MiniDAWLab\crash-dumps\`.
2. Save the logs: `stability-run.log`, `stability-invariant.log`,
   `project-load-diag.log`, `track-delete-diag.log`, `last-operation.txt`
   and, for ASan, the captured stderr report.
3. Symbolize the dump:

   ```powershell
   .\scripts\symbolize-crash.ps1 -Dump "%APPDATA%\MiniDAWLab\crash-dumps\<file>.dmp"
   ```

4. If PageHeap was used, disable it and confirm the status:

   ```powershell
   .\scripts\disable-pageheap.ps1
   ```

## 8. Known findings / limitations

- ASan works with this MSVC/JUCE/Ninja setup (verified 2026-08-27, VS 2026
  MSVC 14.50). `/RTC1` must stay off in the ASan preset; incremental linking
  is disabled.
- The very first ASan smoke run found a real race: the audio thread could
  read a `RoutingBusScratchSlot` buffer while the message thread resized the
  scratch pool in `PlaybackEngine::ensureRoutingBusScratchPool` (called from
  `rebuildRoutingPlanFromSession` during undo/redo/delete). **Fixed in C4B**:
  the pool is now grow-only with shared slots that each published
  `RoutingPlan` co-owns (`RoutingPlan::busScratchOwners`), so buffers can
  never be freed while a plan referencing them is still live on the audio
  thread. ASan smoke and delete-loop pass clean since the fix.
- ASan reports that are entirely inside third-party plugin DLLs are not
  certification blockers by themselves.
- PageHeap `/full` can push memory usage very high on plugin-heavy projects;
  if the machine runs out of memory, retry with a smaller project or fewer
  iterations.
