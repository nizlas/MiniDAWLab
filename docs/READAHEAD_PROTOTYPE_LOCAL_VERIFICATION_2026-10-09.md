# Read-ahead prototype — local Windows verification at 512 samples (2026-10-09)

Draft-PR #7 (`origin/cursor/readahead-prototype-da54`, `d1f5baf`) measured on the same
100-track project as `PARALLEL_A1A2_LOCAL_VERIFICATION_2026-10-09.md`. No merge, no packaging,
main untouched, the original project untouched (sha1 `3f585fc5f701` before and after).

Raw log (20:00–20:16) and the two shutdown-crash reports that have a module/offset:
`docs/evidence/readahead-prototype-2026-10-09/`.

## What was tested

| | |
|---|---|
| Prototype revision | `d1f5baf` |
| Tested revision | `82a7573` = `d1f5baf` + two **diagnostics-only** commits on `local/pr7-readahead-verification-2026-10-09` (`8034aa9` logs read-ahead counters and owned rows from the perf-profile scenario; `82a7573` adds `--stability-readahead-transitions` / `--reopen-check`). No DSP and no transition-model change. |
| Exe | `C:\Users\nicla\development\MiniDAWLab-pr7-readahead\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` |
| SHA-256 | `59406815…6A3525` |
| Project copy | `%TEMP%\dal-perf-100tracks-pr7\TSE_pt2_100tracks.dalproj` (byte-identical to the original): 36 audio rows unmuted, 48 live instruments, 50 inserts (18 AmpliTube 4 Pre, 16 Pro-Q 3, 16 DAL Mono Delay), cycle on, right locator 41.0 s, 180 BPM 4/4 |
| Device (every run) | ASIO Fireface USB, 48 000 Hz, **actual `buf 512`**, 7 workers + callback. Cubase closed, one DAL instance, mixer closed |

Focused tests at `d1f5baf` before the diagnostics commits: `ReadAheadPrototypeFocusedTests`, 156 checks, 0 failures.

A = the same binary **without** `--experimental-readahead`. B = **with** it, default depth 3.
One comparable run per cell. Summed plug-in CPU is never added to callback wall time.

## 1. Passage, bars 17–30 (warm-up 4 s, window 19 s)

| | A — off | B — on |
|---|---|---|
| Blocks | 1 787 × 512 | 1 787 × 512 |
| Callback ms min / mean / max | 1.300 / **2.386** / **5.884** | 1.314 / **2.025** / **3.541** |
| Budget share mean / max (budget 10.667 ms) | 22.4 % / 55.2 % | **19.0 % / 33.2 %** |
| nearOverruns / overruns | 0 / **0** | 0 / **0** |
| Start interval mean / max; late starts | 10.666 / 10.961 ms; 0 | 10.666 / 10.830 ms; 0 |
| Generation section wall mean / max | 2.231 / 5.659 ms | 1.877 / 3.340 ms |
| Join idle mean / max | 0.058 / 0.663 ms | 0.051 / 0.658 ms |
| Pool jobs / block | 84 | **68** (the 16 owned rows left the pool) |
| Transport | 1 025 519 → 1 940 463 = 19.06 s in 19.06 s | same |
| Instruments live | 48 | 48 |
| Output peak / rms L/R / overs / nonFinite | 0.770 / 0.1128/0.1081 / 0 / 0 | 0.754 / 0.1131/0.1084 / 0 / 0 |
| Exit | 0, no dump | scenario PASS, exit 0; a dump was written at "app shutdown begin" with **no** exception text, so it is not classified (see crashes) |

**Startup vs the stable window (B).** Counters are cumulative. At the window start (after the
4 s warm-up): adopted 23, missed 23, missAbandons 7, discardResets 7. At the window end:
missed still 23, missAbandons still 7, staleDiscarded still 23. So the **19 s passage itself
missed nothing and abandoned nothing**; the 23 missed segments and 7 abandons are the warm-up,
where rows were adopted, fell behind, and left the mode. The owned set was the same 16 rows at
both ends of the window.

**What those 16 rows are.** Cap is 16, filled in timeline order, not by cost:
**7 AmpliTube 4 chains + 9 insert-free "Track 1" copies**. 11 of the 18 AmpliTube chains stayed
on the live path. The wrap run (below) settled on 8 + 8 instead — the set depends on which rows
the warm-up abandons. Either way at most 8 of 18 heavy chains are covered, and about half the
slots hold rows with no insert.

The lower callback time is not dropped audio: 0 misses in the window, rms within 0.001 of A,
peak the same class, transport in real time.

## 2. Cycle wrap (start 35 s, warm-up 2 s, window 15 s)

| | A — off | B — on |
|---|---|---|
| Callback mean / max | 2.184 / **13.490** ms | 1.854 / **7.439** ms |
| Budget share mean / max | 20.5 % / **126.5 %** | 17.4 % / **69.7 %** |
| Overruns / late starts | **1 / 1** | **0 / 0** |
| Worst block | 13.490 ms: insert CPU summed 87.946, section wall 13.368, remainder 0.122 | 7.439 ms: insert CPU summed 41.532, section wall 7.300 |
| Pool jobs / block | 84 | 68 |
| Playhead | 1 777 280 → 530 688 (wrapped) | 1 778 304 → 532 736 (wrapped) |
| Read-ahead counters across warm-up **and** window | — | adopted 16, **missed 0, stale 0, abandons 0, discards 0** |
| Output peak / rms / overs / nonFinite | 0.566 / 0.0799/0.0743 / 0 / 0 | 0.543 / 0.0799/0.0743 / 0 / 0 |
| Owned rows | — | 16: **8 AmpliTube + 8 insert-free** |
| Exit | 0, no dump | 0, no dump |

The wrap spike is the known AmpliTube burst at the transport discontinuity. With 8 of the 18
chains pre-rendered, the callback's worst block fell from 1.3 budgets to 0.7. The other 10
chains still spike inside the pool (section wall 7.3 ms). rms is identical, misses are zero, so
the shorter callback is not silence. This run adopted cleanly from the start (no warm-up misses),
unlike the passage run.

## 3. Transitions (production handlers, both modes)

`--stability-readahead-transitions` on a sibling copy. B with the flag, A as the same walk
without it. Counter expectations apply to B only. One OBSERVATION line per deviation; the
scenario's PASS covers hard errors only.

**B, stable steps after the warm-up — all with missed = 0:**

| Step | Handler | What the counters did |
|---|---|---|
| Warm-up 6 s | seek 17.333 s + play | **OBSERVATION:** missed 18, missAbandons 3 (startup only) |
| Pause 1.5 s, resume 3 s | transport controller (Space path) | ownership kept, discard 0, miss 0. Resume is a continuation |
| Seek to 25 s; stop + seek 0 + restart | `requestSeek` / stop intent | discardResets 16 and stale +48 per discontinuity (depth 3 × 16 rows, the documented discard), then re-adopted 16. miss 0 |
| Seek to 39 s, then the wrap | same | pre-wrap discard as above, miss 0. **Wrap itself: produced = consumed, discard 0, miss 0** — gapless |
| Monitor ON, Track 4 (AmpliTube) | mixer strip cell click | monitor=ON, row **left** the owned set (discardResets 1, stale +3 = the queued depth), miss 0 |
| Monitor OFF | same cell | monitor off, miss 0. The row was **not** re-adopted within 3 s — the 16 slots were full |
| Arm the same row 6 s, then disarm | mixer strip Arm cell | the row was already not owned; it **stayed unadopted** the whole time. The "drain a row that is owned at the moment of arming" path was therefore not hit |
| Save while playing | Ctrl+S path | file written (8 409 336 bytes). drainReleases 16 of 16, re-adopted 16, discard 0, **miss 0**. Playhead kept moving |
| Save while paused | pause, then Ctrl+S | file written. While paused: drain 0, discard 0, playhead still, ownership kept. The following resume counted 16 gapless drain releases and 16 re-adoptions, **miss 0** — ownership was rebuilt rather than kept as a pure continuation, without a silent gap |
| Autosave while playing | existing `forceAutosaveNow`, after one undoable rename to make the project dirty | wrote `…-readahead-test_autosave.dalproj` (8 410 391 bytes, age 2.6 s). drain 16, re-adopted 16, discard 0, miss 0 |

Whole B walk: callback max 13.878 ms, **1 overrun** (the walk includes a wrap and several seeks),
peak 0.771, overs 0, nonFinite 0. The A walk over the same ground: max 14.563 ms, also 1 overrun,
peak 0.759, overs 0, nonFinite 0, and 0 observations (no read-ahead to deviate). Saves and the
autosave wrote the file in A as well.

**Reopen in a fresh process** (`--reopen-check`), B's save and then A's save, against a sidecar
captured before the save:

| | B (read-ahead on while saving) | A (off) |
|---|---|---|
| Tracks | 101 = 101 | 101 = 101 |
| Session rows (routing, fader, pan, mute, clips, sends, inputs) | 0 differ of 101 | 0 differ of 101 |
| Insert identity / order / slot count | 0 differ of 50 | 0 differ of 50 |
| Insert state-byte digest | **18** differ | **18** differ |
| Insert #0 parameter 0, set to 0.37 / 0.63 before the playing save | **not restored** (reads 0.000) | **not restored** (reads 0.000) |

The parameter is AmpliTube's `"Param 1"` (2 096 parameters). It fails to round-trip **without**
read-ahead too, so this probe says nothing about the prototype's capture window; it is how this
plug-in/parameter behaves on the direct path. The 18 state-byte diffs are the same in both modes
(one per AmpliTube chain — the plug-in re-serializes after being played). Routing, clips and
insert identity were preserved in both modes.

## Crashes (separate from playback)

Playback results above were all collected under `RESULT: PASS`.

* **Known shutdown crash, classified** (module `AmpliTube 4.vpa`, offset `0x7675E`, 0xC0000005,
  last operation `app shutdown begin` — the same triple as the earlier 512 runs): the A
  transitions process and the A reopen process, both after the scenario had passed.
* **Not classified:** the B passage process and the B reopen process wrote a minidump plus
  `app shutdown begin` but **no exception text** (no module, no offset). Same phase, not the
  same evidence, so not called the known fault.
* B transitions, A passage, A wrap, B wrap: no dump.
* **In-process reopen after playback** (seen at 48 samples, both with and without the flag,
  before this session): crash at `apply: before removeAllPlugins` during the load,
  `AmpliTube 4.vpa+0x7675E`. Same module and offset, **different phase** from the shutdown
  crash, and it happens with the flag off, so it is not caused by PR #7. Not repeated at 512;
  reopen was moved to a fresh process.

## Conclusion

At 512 samples the prototype does useful work for the rows it actually owns, and it does not buy
that by dropping audio. On this project that is 7–8 of the 18 AmpliTube chains (the cap of 16 is
filled in timeline order, so half the slots are insert-free guitar-duplicate rows and 10–11 heavy
chains stay live). Stable playback and the cycle wrap ran with **zero missed segments**; the wrap
spike went from one overrun (13.5 ms) to none (7.4 ms). The warm-up is the weak part: ~20 missed
segments and a handful of abandons while the single worker catches up, after which those rows are
back on the live path. Pause/resume, seek, stop/restart, the wrap, monitor handover, save while
playing and while paused, and autosave all behaved as the model describes at 512, with no missed
segments; arming was only checked for a row that was already not owned. Save/reopen preserved
session rows and insert identity identically to the direct path. Worth continued listening and
testing. Not worth changing the selection, the cap, the depth or the thread model on the back of
this run — the adoption order is the obvious limit, and it was left as it is on purpose.

## Listening

```
C:\Users\nicla\development\MiniDAWLab-pr7-readahead\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe --experimental-readahead
```

Same binary without the flag is the direct path. Depth stays 3 (the default); nothing else is
required. Project: the original, or the untouched copy
`%TEMP%\dal-perf-100tracks-pr7\TSE_pt2_100tracks.dalproj` — not the `…-readahead-test` sibling,
which this session saved over.

Listen for: the first seconds after play (the measured warm-up misses), a fader or plug-in tweak
on an adopted row (documented late by up to 3 blocks, ~32 ms), monitor on an adopted guitar row,
and one cycle wrap. The wrap was inside budget in the measurement; 10–11 AmpliTube chains are
still live there.

## Follow-up the same evening (priming, the two dumps, DAL Mono Delay)

Still on `local/pr7-readahead-verification-2026-10-09`. Depth 3, cap 16 and timeline-order
selection were not changed. Read-ahead stays experimental and off by default. Original project
sha1 `3f585fc5f701` still unchanged after these runs.

### Startup gaps

The previous warm-up misses were not steady-state overload. `audioThread_beginBlock` promoted
every `Scheduled` row to `Ahead` on the next block, and an empty ring was then a counted miss.
Up to 16 rows were scheduled in one callback; the single worker cannot finish every first
segment in the gap before that callback, so the rows it had not reached went silent and, after
two misses, were abandoned. At 512 the worker keeps up once the set has settled, which is why
the measured window itself had zero misses.

A row now stays on the direct path until its first segment is queued. If the ring is empty and
the worker has not entered the chain, the prime is declined (live render, no miss) and may be
offered again. If the worker is already inside the plugin, the callback does not render that
instance and does not wait; a segment that is still absent at consume is a counted miss. The
steady-state miss/abandon policy for a row that is already ahead is unchanged.

`ReadAheadPrototypeFocusedTests`: 170 checks, 0 failures, including a worker that never runs
(every block audible, `missed=0`) and a real worker held off until it is inside the first
plugin call (the sibling row stays live, `gOverlaps=0`). DAL Mono Delay save is a separate
invocation of the same exe (`--mono-delay-save`), not part of that 170.

### Same 100-track project, first Play included

Exe rebuilt from this follow-up, SHA-256
`18DC252C94C1D55B5EED539579F45D2EB1BAB4CB4C5FFB6359FDDC42C37CC3A1`.
ASIO Fireface USB, 48 kHz, actual `buf 512`, Cubase closed. `--experimental-readahead`, warmup
0, so the cumulative counters at the window start already include the blocks since Play.
No new flag-off control: the flag-off path still constructs no read-ahead object.

| | Passage, start 17.333 s, 19 s | Cycle wrap, start 35 s, 12 s |
|---|---|---|
| Counters at window start | adopted 42, produced 374, consumed 326, **missed 0, abandons 0** | adopted 16, produced 416, consumed 368, **missed 0, abandons 0** |
| Counters at window end | adopted 42, produced 28998, consumed 28950, **missed 0, abandons 0** | adopted 16, produced 18485, consumed 18448, **missed 0, abandons 0** |
| Owned | 16 (8 AmpliTube 4 + 8 insert-free Track 1 copies) | 16, same shape |
| Callback mean/max | 2.107 / 3.850 ms (19.8 / 36.1% of 10.667 ms) | 1.847 / 9.312 ms (17.3 / 87.3%) |
| Overruns / late starts | 0 / 0 | 0 / 0 (one block over 70% of budget, none at 100%) |
| Transport | 843759 → 1759215, wall 19.07 s | 1692288 → 301312 (wrapped), wall 12.03 s |
| Output | peak 0.757, overs 0, non-finite 0 | peak 0.568, overs 0, non-finite 0 |

`adopted=42` against 16 owned rows is declined-and-reoffered primes, not misses. Both runs
exited the scenario with RESULT PASS and then hit the known AmpliTube shutdown fault below.

### The two dumps that had no module/offset

Parsed with the minidump exception stream and the exe/PDB from the run that wrote them
(SHA-256 `59406815…6A3525`, before this follow-up was linked). Both are access violations
`0xC0000005`, faulting inside `AmpliTube 4.vpa` at offset `0x7675E`, parameter 0 = read.
The host frames on the faulting thread are the same chain as the two dumps that already had
a text report (pids 24656 and 38544):

`VST3PluginInstance::~VST3PluginInstance` → `PluginInsertHost::releaseResources` →
`PluginInsertHost::~PluginInsertHost` → `PlaybackEngine` destructor →
`MiniDAWLabApplication::shutdown` → `WinMain`.

No `ReadAheadRenderer` frame. Last operation on both is `app shutdown begin`. They are the
known AmpliTube teardown fault, not a read-ahead lifetime bug. The missing `.txt` is the
in-process report writer not finishing the metadata file; the dump already contained the
exception record. This follow-up's two shutdowns (pids 20524 and 48772) wrote the text report
as well, same module and offset `0x7675E`, same phase.

### DAL Mono Delay save / reopen

AmpliTube "Param 1" still reads back 0.000 after reopen in both the flagged run and the
direct-path control from the earlier session, so that probe does not prove parameter restore.
It was not investigated further.

A one-audio-row project (not the original) loaded the installed DAL Mono Delay as a Pre
insert with Sync off, delay time 250.0 ms, feedback 20.0%, mix 35.0%, both cuts off. With
read-ahead on, the row was adopted (`produced>0`, `missed=0`, `missAbandons=0`) before the
writes. Save while playing, save while paused, and the autosave bracket all go through
`ScopedPluginStateCaptureWindow` + `Session::saveProjectToFile` — the same bracket
`ProjectIoCoordinator::writeAutosaveNow` uses — and the autosave case restores the current
project path afterwards. Each file was opened in a fresh process.

All six files (read-ahead and direct, three writes each) restored the six parameters
bit-for-bit on the normalized value and the plug-in's own text. An impulse through the
reloaded insert peaked at sample 12000 (250.0 ms) with amplitude 0.350 (the 35% mix),
within 0 samples of the expected delay, in every file.
