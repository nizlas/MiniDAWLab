# A1/A2 parallel strips — local Windows verification (2026-10-09)

Build, focused tests and a controlled A/B of draft-PR #6 (`origin/cursor/parallel-audio-readahead-plan-da54`)
against main 1.2.1. No code was changed, merged or packaged. Raw log extract, crash
classifications and binary hashes: `docs/evidence/parallel-a1a2-2026-10-09/`.

## Builds

| | Revision | Exe | SHA-256 |
|---|---|---|---|
| A — baseline | `f11c0fb` (main 1.2.1: parallel instrument generation, serial strips) | `C:\Users\nicla\development\MiniDAWLab\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` | `DA8D3A58…B777E` = the 1.2.1 release artifact (`dist\symbols\DanielssonsAudioLab-1.2.1\MiniDAWLab.exe`, built at `f11c0fb` from a clean tree; tree still clean) |
| B — candidate | `10374e2` (A1 parallel audio-row strips + A2 combined instrument generation+strip jobs) | `C:\Users\nicla\development\MiniDAWLab-pr6-a2\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` (detached worktree at `10374e2`) | `9849DD84…EEAC6` |

Both Release, Ninja/MSVC, ASIO SDK (`MINIDAW_ASIO_SDK_DIR` → the main checkout's `external_tools/asiosdk`,
"ASIO backend ENABLED"), same JUCE source. Both report version 1.2.1 — the version string does
not distinguish them; use path + hash. The candidate worktree has no `Tools\lame` (MP3 export
only).

## Focused tests (candidate)

`AudioStripParallelFocusedTests` 85 checks, 0 failures; `InstrumentParallelFocusedTests` 39, 0.

## Conditions (identical for A and B)

Fresh copy of `C:\Users\nicla\Music\TSE_pt2_100tracks\TSE_pt2_100tracks.dalproj` (saved
2026-10-07 22:18, v28, sha1 `3f585fc5f701`; 36 audio rows **unmuted**, 48 instruments, 16 MIDI,
2 visual groups, no Off / Solo) at `%TEMP%\dal-perf-100tracks-ab` with Audio + InstrumentProxies
(references verified); original untouched. ASIO Fireface USB, 48 000 Hz, **actual buffer 512**
(logged), **7 render workers + callback** (logged), mixer closed, main window visible, no Cubase,
one DAL instance, no compilation. 180 BPM 4/4 → bar = 1.333 s. Primary window: start bar 14
(17.333 s), 4 s warm-up, 19 s measured = **bars 17–30 (21.3–40.3 s)**, before the cycle wrap at
41 s. `--stability-perf-profile <copy> --seconds 19 --warmup 4 --start-seconds 17.333`.

## A/B, bars 17–30

| Measure | A `f11c0fb` | B `10374e2` |
|---|---|---|
| Blocks / real time | 1 784 / 19.02 s music in 19.02 s wall (start interval 10.666 ms, 0 late) | 1 786 / 19.05 s in 19.05 s (10.666 ms, 0 late) |
| Callback min / **mean** / max | 1.63 / **5.64** / 9.34 ms (52.9 % / 87.6 %) | 1.38 / **2.30** / 4.77 ms (21.5 % / 44.7 %) |
| Deadline overruns (≥ 100 %) / near (> 70 %) | **0** / 292 | **0** / 0 |
| Render-pool section wall (dispatch → join), mean / max | 1.56 / 3.33 ms (48 jobs/block) | 2.14 / 4.53 ms (**84 jobs/block** = 48 instrument + 36 audio-row strips) |
| Callback idle wait at join, mean / max | 0.07 / 0.76 ms | 0.05 / 0.61 ms |
| Serial insert processing on the callback | 3.92 ms mean / 7.16 max (18 AmpliTube 4 + 16 Pro-Q 3 + 16 Mono Delay, 44.6 calls/block) | moved into the jobs — none on the callback |
| Phase `clip-render` / `instrument-mix` | 3.58 / 2.00 ms | 2.20 / 0.04 ms (see category note) |
| DAL remainder (outside plug-in calls / section) | 0.165 / 0.425 ms | 0.156 / 0.364 ms |
| Processed | 48 instruments live, 98 plug-in instances with calls, 44.6 insert calls/block | same: 48 / 98 / 44.5 |
| Output | peak 0.755, Stereo Out 0.641/0.755, overs 0/0, non-finite 0, RMS 0.1129/0.1083 | peak 0.755, 0.645/0.755, 0/0, 0, 0.1126/0.1080 |
| Process CPU | 166 % of one core | 177 % of one core |

**Accounting check (no double counting):** A: section wall 1.557 + callback-thread inserts 3.916 +
remainder 0.165 = 5.638 = callback mean. B: section wall 2.140 + remainder 0.156 = 2.296 = callback
mean. **Category semantics changed in B:** `category insert-plugin` (4.54 ms mean, 13.6 max) is now
summed CPU across the worker threads like `instrument-plugin` always was — it is no longer
callback time and must not be added to the callback total (its "shareOfCallback 198 %" is that
artefact); `phase clip-render(incl. audio inserts)` now spans the parallel section (2.20 ≈ section
wall) instead of serial insert work, and `phase instrument-mix(incl. inserts)` is only the summing
step (0.04 ms). The per-instance table and the "by plug-in" shares are summed CPU in both builds.

## Cycle boundary (candidate correctness check, plus baseline reference)

`--start-seconds 35 --warmup 2 --seconds 15` (wrap at 41 s inside the window; transport continued
from the left locator: playhead 1 778 304 → 531 712 in both builds; 48 instruments live, overs 0,
non-finite 0, no proxy):

| | A `f11c0fb` | B `10374e2` |
|---|---|---|
| Callback mean / max | 5.95 / **38.5 ms (361 %)** | 2.17 / **13.1 ms (122 %)** |
| Overruns / late starts | **3** / 2 | **1** / 0 |
| Worst block | inserts 37.0 ms on the callback, section 1.3 | section wall 12.9 ms with 82.8 ms summed insert CPU across workers |

The wrap produces one very expensive insert block in **both** builds (the 18 AmpliTube 4 chains
reacting to the transport discontinuity — consistent with the plan's "plugin transport context"
note; not isolated per instance here). It is pre-existing and smaller in B (parallel), but still
one deadline miss in B — expect an audible click at the wrap in both builds. Not introduced by
A1/A2; next step if pursued: isolate which chains spike at the wrap (per-instance max in the
profile is only reported for instruments).

## Crashes

Both measurement runs ended with the known **AmpliTube 4 teardown crash** after their PASS line
(`AmpliTube 4.vpa+0x7675E`, 0xC0000005, last operation "app shutdown begin") — identical module
offset and phase in A and B; no crash during load or playback in any of the four runs (exit code 0
from the scenario, dumps written during shutdown only).

## Not covered here

Listening (the user's), Monitor / Record on a row with the candidate, long soak, `--instrument-workers 0`
bit-identity checks (correctness reference only, per the plan's corrected recipe).
