# Stability testing: ASan and PageHeap (Stability C4)

Developer-only memory diagnostics for MiniDAWLab (DAL). These modes catch
use-after-free, heap-buffer-overflow and heap corruption that normal Debug
builds miss.

> **WARNING — developer testing only.**
> Never send an ASan build or a PageHeap-enabled machine setup to Conny or any
> tester as a normal test build. The normal tester build is the **Release
> installer** produced by `.\scripts\package-windows.ps1`. ASan/PageHeap builds
> are slow, memory-heavy, and abort on the first detected error.

> **When to run what:** the scenarios and matrix in this document are heavy
> gates, not per-edit checks. For ordinary implementation slices use the tiered
> policy in **`docs/DEVELOPMENT_TEST_POLICY.md`** (Level 0–4) and run only the
> scenarios relevant to the changed subsystem. The full matrix is Level 3:
> explicit request, pre-commit batch, multi-subsystem or crash-fix changes,
> and pre-release only.

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

## 8. Autosave and crash recovery (Stability C5)

Autosave is a **safety net**, not a substitute for Save. It never touches the
user's project file and never changes what Ctrl+S does.

Where the files live:

- Saved project: `<projectFolder>\<projectStem>_autosave.dalproj` (e.g.
  `beakon_test5_autosave.dalproj`, next to the project file so
  `Audio/`-relative clip paths keep resolving). The project-specific name
  means projects sharing a folder get distinct autosaves.
- Never-saved project: `%APPDATA%\MiniDAWLab\autosave.dalproj` (works for
  MIDI-only/internal data; a never-saved project referencing external audio
  may fail the relative-path check — the failure is logged, nothing breaks).
- Pointer file: `%APPDATA%\MiniDAWLab\autosave-location.txt` (line 1 =
  autosave path, line 2 = the project it belongs to). Recovery always goes
  through the pointer, never filename guessing.
- Diagnostics: `%APPDATA%\MiniDAWLab\autosave-diag.log` (tick skips, writes
  with size/elapsed/next interval, failures, recovery outcomes,
  stale-pointer cleanup).

Backward compatibility: autosaves written by older builds were all named
`<projectFolder>\autosave.dalproj`. A pointer that references such a file
still recovers normally (logged as `legacy pointer accepted`), and a
successful manual save also cleans up a legacy-named sibling autosave. The
next autosave after that uses the new project-specific name.

Policy (adaptive interval): an internal tick fires every 60 s, but writes
follow an adaptive schedule so large projects are not autosaved too
aggressively:

- First autosave: roughly 60–120 s after the project becomes dirty (the
  first tick that observes the dirty state schedules a write one minute out).
- After a successful write, the next interval depends on how long that write
  took: `< 250 ms` → 2 min, `250 ms–1 s` → 5 min, `1–3 s` → 10 min,
  `> 3 s` → 15 min (also logged as a slow-write note).
- A write only happens when the project is dirty and nothing blocks it
  (recording/count-in, any modal dialog, stability test mode). Blocked or
  failed writes are logged with a reason and retried on the next tick
  without resetting the schedule; a clean project resets the schedule.

Writes are atomic (temp file + move) and a failed write never deletes a
previous valid autosave and never clears the dirty state. A successful manual
save deletes the autosave (including a legacy-named one) and the pointer,
but never an autosave the pointer attributes to a *different* project.

Automated scenarios:

```powershell
.\build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe --stability-autosave "C:\path\project.dalproj"
.\build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe --stability-recover-autosave "C:\path\project.dalproj"
# or as part of the matrix:
.\scripts\stability-matrix.ps1 -Project "C:\path\project.dalproj" -IncludeAutosave
```

They load the project, make a dirty edit, force an autosave, verify the file
(project-specific `<stem>_autosave.dalproj` name), the pointer (path + owner
lines), and that the original project file is byte-identical afterwards; the
recover variant additionally runs the recovery path in-process, verifies the
recovered project is dirty, contains the edit, and has **no** save path
(so Save goes through Save As and can never silently overwrite the original),
and finally stages a legacy-named `autosave.dalproj` with a matching pointer
to verify old autosaves still recover.

Manual recovery test:

1. Open a project, make an edit (dirty), do not save.
2. Wait for the periodic autosave — first write lands 60–120 s after the
   edit (check `autosave-diag.log` for "write ok" and the chosen
   `nextIntervalSec`), or run the `--stability-autosave` scenario instead.
3. Kill the process (Task Manager), or use `--stability-crash-test` if you
   also want to exercise the crash-dump pipeline.
4. Restart the app normally; choose **Recover** in the prompt.
5. Verify the edit is present, the title/save state is unsaved, and Ctrl+S
   opens Save As. **Ignore** keeps the autosave for the next startup;
   **Delete Autosave** removes the file and pointer.

After a crash with an autosave present, collect `autosave-diag.log` together
with the crash dump and the other logs in section 7.

## 9. Known findings / limitations

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

## 10. Release certification gate (C6)

Before any build is sent to an external tester, run the certification gate,
which wraps everything in this document into one PASS/FAIL sequence with a
written report:

```powershell
.\scripts\certify-release.ps1 -Project "C:\path\project.dalproj" -Iterations 5
# stronger, after memory/lifetime-sensitive changes:
.\scripts\certify-release.ps1 -Project "C:\path\project.dalproj" -IncludeAsan
```

See `docs/RELEASE_CERTIFICATION.md` for the full checklist, what to send to
the tester (Release installer only), and what to archive internally. The
tester-facing release notes template is `docs/TESTER_RELEASE_NOTES_TEMPLATE.md`.
