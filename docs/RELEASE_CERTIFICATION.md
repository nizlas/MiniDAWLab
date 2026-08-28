# Release certification (Stability C6)

A build may only be sent to an external/manual tester (e.g. Conny) after it has
passed the certification gate described here. The gate is automated by
`scripts\certify-release.ps1` and answers one question:

> "Is this build safe enough to send to a tester?"

"Certified enough for tester" means: the build compiles cleanly, survives the
full Debug stability matrix (load/delete/undo-redo/mixdown/autosave/recovery)
with zero invariant failures, zero new crash dumps and zero audio-gate
timeouts, and the Release installer + matching PDB symbols were produced and
archived. It does **not** mean bug-free — third-party plugins can still crash
in their own code; that is what the crash-dump + symbolization pipeline (C1)
is for.

## How to run

From the repo root, in a normal PowerShell:

```powershell
# Standard certification (required before every tester build):
.\scripts\certify-release.ps1 -Project "C:\path\beakon_test5.dalproj" -Iterations 5

# Stronger certification (recommended after memory/lifetime-sensitive changes):
.\scripts\certify-release.ps1 -Project "C:\path\beakon_test5.dalproj" -Iterations 5 -IncludeAsan

# Strongest (requires an elevated PowerShell for PageHeap):
.\scripts\certify-release.ps1 -Project "C:\path\beakon_test5.dalproj" -IncludeAsan -IncludePageHeap
```

Parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `-Project` | (required) | Reference `.dalproj` used by all stability scenarios |
| `-Version` | parsed from `CMakeLists.txt` | Version stamped on artifacts/report |
| `-Iterations` | 5 | Iterations for load/delete/undo-redo loops in the Debug matrix |
| `-IncludeAsan` | off | Build ASan and run targeted scenarios (smoke, delete-loop x2, autosave) |
| `-IncludePageHeap` | off | Short matrix under PageHeap (needs Administrator); always disabled afterward |
| `-IncludeReleaseSmoke` | off | Run the Release exe with `--stability-smoke` (otherwise: manual smoke) |
| `-SkipPackage` | off | Skip Release build/package/artifact checks (not valid for a real tester gate) |
| `-TimeoutSec` | 3600 | Per-scenario timeout (ASan/PageHeap are ~10x slower than Debug) |
| `-OutDir` | `dist\certification\<timestamp>` | Where report + log copies are written |

Exit code 0 = PASS, 1 = FAIL. The report is written either way to
`<OutDir>\CERTIFICATION_REPORT.md`.

## Required green checks (certification PASS)

All of these must be PASS; any FAIL means the build is **not** approved:

1. Debug build OK (`build-windows.ps1 -Config Debug`).
2. Debug stability matrix PASS with `-IncludeMixdown -IncludeAutosave`.
3. Zero new `INVARIANT FAIL` lines in `stability-invariant.log`.
4. Zero new `RESULT: FAIL` lines in `stability-run.log`.
5. Zero new `timeout=YES` gate timeouts in `track-delete-diag.log`.
6. Zero new `FAIL` lines in `mixdown-diag.log`.
7. Autosave wrote at least once with zero `write FAIL` in `autosave-diag.log`.
8. Zero new crash dumps in `%APPDATA%\MiniDAWLab\crash-dumps` during the run.
9. Release build + package OK (`package-windows.ps1`).
10. Installer `dist\DanielssonsAudioLab-<version>-Setup.exe` exists and is nonzero.
11. Matching `MiniDAWLab.exe` + `MiniDAWLab.pdb` archived under `dist\symbols\DanielssonsAudioLab-<version>\`.
12. No PDB inside the staged tree / installer payload.
13. `scripts\symbolize-crash.ps1` exists.

## Optional stronger checks

- `-IncludeAsan`: ASan build + smoke/delete-loop/autosave scenarios must exit 0
  with no `ERROR: AddressSanitizer` output. Run after changes to buffers,
  routing, plugin lifetime, or anything touched by C2B/C4B-class bugs.
- `-IncludePageHeap`: short matrix under full PageHeap; the script verifies
  PageHeap is disabled again afterward (and force-disables it if not).
- `-IncludeReleaseSmoke`: `--stability-smoke` against the Release exe. If not
  used, do a quick manual smoke of the installed build: install, open the
  reference project, play, save, close.

If `-IncludeAsan` or `-IncludePageHeap` is supplied, those checks become
required for PASS.

### Triaging an ASan FAIL

Open the failing transcript in `<OutDir>\logs\` (e.g. `asan-delete-loop.txt`)
and look at the report's stack traces:

- Frames in **our code or JUCE** (`MiniDAWLab`, `src\...`, `juce_...`): a real
  blocker. Root-cause and fix before shipping (as with C2B/C4B).
- **All frames inside a third-party plugin DLL** (e.g.
  `AmpliTube 4.vpa`/`.vst3`) on a thread the plugin created itself: this is
  plugin-internal and, per the policy in `docs/STABILITY_TESTING.md` §9, not a
  certification blocker by itself. Re-run the scenario to confirm it is
  intermittent, note it under "known limitations" in the tester release notes,
  and proceed with a re-run that passes (or run certification without
  `-IncludeAsan` and record the plugin report internally).

## What to send to the tester

- **Only** the Release installer: `dist\DanielssonsAudioLab-<version>-Setup.exe`.
- Plus the filled-in release notes (see `docs/TESTER_RELEASE_NOTES_TEMPLATE.md`).

Never send:

- the ASan build (`build\ninja-asan\...`) — developer diagnostics only,
- any PDB file or the `dist\symbols\` folder,
- Debug builds or the certification logs.

## What to keep internally (per certified release)

- The installer and zip that were shipped.
- The matching exe/PDB pair under `dist\symbols\DanielssonsAudioLab-<version>\`
  (required to symbolize tester crash dumps later — never regenerate; the PDB
  must match the shipped exe byte-for-byte).
- `<OutDir>\CERTIFICATION_REPORT.md` and `<OutDir>\logs\`.
- Any crash dumps that occurred (copied to `<OutDir>\crash-dumps\`).

## What to collect from the tester after a crash

1. The whole `%APPDATA%\MiniDAWLab\crash-dumps\` folder (dumps + metadata).
2. `%APPDATA%\MiniDAWLab\last-operation.txt`.
3. The project file, if shareable.
4. Which third-party plugins were loaded, if relevant.

Then symbolize with `scripts\symbolize-crash.ps1` against the archived symbols
for that version. See `docs/STABILITY_TESTING.md` for the wider stability
tooling (matrix, ASan, PageHeap, autosave/recovery testing).

## Log handling

The certification script never deletes user logs. All checks are delta-based:
line/dump counts are captured before the run and only new occurrences count.
Relevant `%APPDATA%\MiniDAWLab` logs are copied into `<OutDir>\logs\` at the
end, and crash dumps created during the window into `<OutDir>\crash-dumps\`.
