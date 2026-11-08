# CPU and memory: before resource saving, 1.4.1, and the small working variant (2026-11-08)

A bounded comparison of three cases plus one control variant, on this desktop only. No production
code, project, user setting or audio-device setting was changed. All runs used temp copies under
`%TEMP%\dal-cmp3`; the original project files have the same SHA-256 before and after
(261031 `3F111DBC…EC26`, 261106 `E5CEF985…E259`), and `audio-device.xml` is unchanged
(`D6B41E20…90A5`, last written 2026-11-04).

| Case | Binary | Project |
|---|---|---|
| A | 8472feb (before PR #12), Release, ASIO | TSE_pt2_261031 |
| B | DAL 1.4.1 release exe (a71d986) | TSE_pt2_261031 |
| C | DAL 1.4.1 release exe | TSE_pt2_261106 (most rows Off) |
| D | DAL 1.4.1 release exe | 261031 with only the rows that are Off in 261106 set Off |

A→B is the effect of the resource-saving work on the same project. B→C is the difference to the
small working variant. B→D isolates the Off effect from the other differences between the two
project files (section 2). **B→C and B→D come out the same within the run spread**, so the
B→C difference is the Off effect.

## 1. Conditions

- **Machine**: Intel Core Ultra 9 275HX, 24 logical cores, 97.6 GB RAM.
- **Device** (logged by every run): ASIO Fireface USB, 48 000 Hz, 512-sample buffer (the existing
  setting; nothing was requested), 10.667 ms block budget, 7 render workers (8 threads including
  the callback).
- **Concurrency**: no other DAL, Cubase or compiler process was running at the start of any run
  (checked before each run). Each run was a new process, and every DAL and helper process had
  exited before the next run started.
- **Binaries**:
  - **B/C/D**: `dist\DanielssonsAudioLab-1.4.1\MiniDAWLab.exe`, SHA-256 `C497140F…238C`. This is
    the same file as the Release build, the zip and the symbols folder. 1.4.1, Release, ASIO.
  - **A**: built from the clean base worktree at 8472feb, with **the same compiler as the 1.4.1
    release** (MSVC 14.50.35717, VS 18 Community), Release, `/O2 /Ob2`, ASIO SDK enabled. SHA-256
    `965D3B14…4603`, FileVersion 1.4.0. The earlier base exe in that worktree had been built with
    BuildTools 14.51, so it was not used, to keep the compiler out of A→B. Build script:
    `docs/evidence/cpu-memory-abcd-2026-11-08/scripts/build-base-same-compiler.bat`.
- **Procedure**: the existing `--stability-perf-profile` scenario, identical arguments in both
  versions: `--seconds 30 --warmup 5 --start-seconds 17 --idle --idle-gap 0`. The control runs add
  `--profile-off`.
- **Window state**: in every run the mixer window was closed by the scenario and no plug-in editor
  was open. 261031 was saved with the MIDI editor open, and 261106 at another screen position.
  These UI fields were made identical in the temp copies so the window state could not differ
  between cases:
  - in the 261031 copies (A/B and D), the MIDI editor workspace was set closed;
  - in the C copy, only the main window position (x/y) was changed, because 261106 stores no MIDI
    editor workspace.
  Section 7 lists every field the C copy changed.
- **Run order**: two rounds in opposite order, each case with a profiled and a control run:
  - round 1: A, B, C, D, profiled then control;
  - round 2: D, C, B, A, control then profiled.
  That is 16 runs, 4 per case. Tables give the mean and (min–max).
- **Crashes**: no crash dumps from any run, and every run exited with code 0. No proxy files were
  written (InstrumentProxies count unchanged).

### Measurement windows

| Phase | What | Where it comes from |
|---|---|---|
| Load | Process start → project load step finished (synchronous load call, including plug-ins and the AmpliTube helper). Peaks of DAL+helpers summed **per sample** (simultaneous), every 250 ms | External sampler |
| Before first Play | 30 s stopped, never played, after the scenario's fixed 8 s settle | Scenario window + external sampler |
| Playback | Start at 17.0 s, 5 s warm-up, measured 22.0–52.0 s of the arrangement. The cycle (0–70.67 s) is on but **not reached**; the transport advanced 30.0 s in 30.0 s wall in every run. This passage is where both drum rows (3: 4–52.7 s, 25: from 36 s) and both cello rows (45: to 59.8 s, 40: 22.3–54.0 s) have notes | Scenario window + external sampler |
| After Stop | External CPU and memory: **0–20 s and 30–50 s** after Stop, exactly. Profiled callback figures come from the scenario's windows **0–30 s and 30–60 s** after Stop, because the existing scenario uses one window length; the callback rows below are labelled with those spans | Sampler / scenario |

## 2. The two projects

Same tracks, same order, same notes and audio clips, same routing and inserts. Differences
(`projdiff.txt`, `instr.txt`):

- **Power**: 44 rows On in 261031 are Off in 261106. Track 27 is Off in both.
- **Mutes**:
  - Track 40 (HALion Sonic, a cello reference) is **muted in 261031 and unmuted in 261106**.
  - Nine audio rows that are Off in 261106 were also muted there.
  - VB3-II copy 2 (track 23) is muted in both.
- **Groove Agent SE (tracks 3 and 25)**: plug-in state re-saved in 261106 (about 3 KB smaller).
- **Other plug-in state**: AmpliTube states differ only in a 47-byte span, the per-save GUID
  AmpliTube writes.
- **Proxy**: VB3-II (track 5) has a re-rendered proxy. It is not selected; no proxy was in use in
  any run.
- **Proxy mode**: Groove Agent rows and MIDI 1 store `manual` explicitly. 1.4.1 reads the missing
  value in 261031 as Manual too.
- **UI fields**: selected track, playhead, main window position, and the MIDI editor workspace
  (normalised, see above).

**On in 261106 (C):**

| Track | Instrument | Notes |
|---|---|---|
| 3 | Groove Agent SE (drums) | |
| 25 | Groove Agent SE (drums, copy) | |
| 45 | HALion Sonic + DAL Mono Delay (cello reference) | |
| 40 | HALion Sonic + DAL Mono Delay (cello reference) | unmuted in 261106 |
| 5 | VB3-II + Pro-Q 3 | no notes in the measured passage (its notes start at 54.5 s) |
| 23 | VB3-II copy 2 + Pro-Q 3 | **muted** |

The master is also On, and all audio rows are Off. That is 6 instruments and 4 insert instances.
Two VB3-II rows are On in addition to the expected drums and two cellos.

**D** = 261031 with exactly those 44 rows set Off (track `off` and, where 261106 has it, the
instrument `powerOn: false`). Its mutes, states and proxies are as in 261031, so track 40 is
muted in D. D was made by editing the JSON; it never loaded a plug-in it then switched Off.
C and D have the same On rows (asserted when the file was written).

## 3. Results

CPU and memory come from the **control runs** (detail profiling off), sampled externally from DAL
plus every helper process together. Callback figures come from the **profiled runs**; the
engine's own load meter in the control runs is shown too. Detail profiling is not free: the
profiled runs' CPU is listed separately and is 0–0.1 core higher. Neither kind of run is free of
measurement overhead; the external sampler costs CPU in its own process, which is not counted.

CPU is in cores (1.0 = one logical core fully busy) and as % of all 24 logical cores.

### Load

| | A | B | C | D | A→B | B→C | B→D |
|---|---|---|---|---|---|---|---|
| Process start → loaded (s) | 17.9 (15.6–20.6) | 16.8 (15.6–19.7) | 5.7 (5.7–5.8) | 5.7 (5.6–5.8) | −6 % | −66 % | −66 % |
| Load call itself (s) | 14.8 (12.5–17.3) | 13.9 (12.7–16.6) | 2.8 (2.7–2.9) | 2.8 (2.7–2.9) | −6 % | −80 % | −80 % |
| Peak private, simultaneous total, to loaded (MB) | 4494 | 4524 | 1120 | 1110 | +1 % | −75 % | −75 % |
| Peak working set, simultaneous total, to loaded (MB) | 4283 | 4333 | 1050 | 1042 | +1 % | −76 % | −76 % |
| Same, including the 8 s settle: private / WS (MB) | 5028 / 4736 | 5023 / 4751 | 1383 / 1250 | 1363 / 1226 | 0 % | −72 % / −74 % | −73 % / −74 % |
| CPU during the 8 s settle (cores) | 1.08 | 0.73 | 0.50 | 0.49 | −32 % | −31 % | −32 % |

Load time depends on the OS file cache. These are warm-cache runs, not cold starts. The A/B
spread comes from round 2, where both A and one B run loaded about 4 s slower.

### Before first Play (30 s stopped)

| | A | B | C | D | A→B | B→C | B→D |
|---|---|---|---|---|---|---|---|
| **CPU total (cores)** | **0.87** (0.76–0.97) | **0.48** (0.45–0.50) | **0.36** (0.35–0.37) | **0.37** (0.35–0.39) | **−45 %** | −24 % | −23 % |
| % of 24 logical | 3.6 % | 2.0 % | 1.5 % | 1.5 % | | | |
| DAL / helpers (cores) | 0.87 / – | 0.46 / 0.014 | 0.36 / – | 0.37 / – | | | |
| **Private total (MB)** | **5006** | **4854** | **1318** | **1320** | −3 % | **−73 %** | −73 % |
| DAL / helpers private (MB) | 5006 / – | 2149 / 2705 | 1318 / – | 1320 / – | | | |
| Working set total (MB) | 4713 | 4578 | 1181 | 1183 | −3 % | −74 % | −74 % |
| CPU, profiled runs (cores) | 0.84 | 0.56 | 0.37 | 0.40 | | | |
| Callback mean / max (ms) | 3.83 / 5.85 | 0.96 / 1.87 | 0.60 / 1.27 | 0.65 / 1.20 | −75 % / −68 % | −37 % / −32 % | −32 % / −36 % |
| Budget mean / max (%) | 35.9 / 54.9 | 8.9 / 17.5 | 5.7 / 11.9 | 6.0 / 11.2 | | | |
| Callback mean / max, control runs (ms) | 3.97 / 6.01 | 0.93 / 1.89 | 0.57 / 1.35 | 0.61 / 1.21 | | | |

### Playback (22–52 s, 30 s)

| | A | B | C | D | A→B | B→C | B→D |
|---|---|---|---|---|---|---|---|
| **CPU total (cores)** | **0.99** (0.94–1.03) | **0.73** (0.70–0.75) | **0.44** (0.42–0.46) | **0.43** (0.42–0.44) | **−26 %** | **−40 %** | −40 % |
| % of 24 logical | 4.1 % | 3.0 % | 1.8 % | 1.8 % | | | |
| DAL / helpers (cores) | 0.99 / – | 0.71 / 0.011 | 0.44 / – | 0.43 / – | | | |
| **Private total (MB)** | **5007** | **4856** | **1320** | **1321** | −3 % | **−73 %** | −73 % |
| DAL / helpers private (MB) | 5007 / – | 2151 / 2705 | 1320 / – | 1321 / – | | | |
| Working set total (MB) | 4715 | 4580 | 1183 | 1184 | −3 % | −74 % | −74 % |
| CPU, profiled runs (cores) | 1.00 | 0.83 | 0.44 | 0.41 | | | |
| Callback mean / max (ms) | 0.92 / 1.69 | 0.71 / 1.38 | 0.67 / 1.29 | 0.69 / 1.30 | −22 % / −19 % | −7 % / −7 % | −3 % / −6 % |
| Budget mean / max (%) | 8.6 / 15.9 | 6.7 / 12.9 | 6.2 / 12.1 | 6.4 / 12.2 | | | |
| Callback mean / max, control runs (ms) | 0.93 / 1.65 | 0.74 / 1.42 | 0.67 / 1.36 | 0.71 / 1.29 | | | |

### After Stop

| | A | B | C | D | A→B | B→C | B→D |
|---|---|---|---|---|---|---|---|
| **CPU 0–20 s (cores)** | **0.84** (0.71–0.96) | **0.73** (0.73–0.73) | **0.38** | **0.36** | −13 % | **−48 %** | −50 % |
| **CPU 30–50 s (cores)** | **0.85** (0.78–0.93) | **0.68** (0.67–0.70) | **0.39** | **0.39** | −20 % | **−42 %** | −42 % |
| % of 24 logical, 0–20 / 30–50 | 3.5 / 3.6 % | 3.0 / 2.8 % | 1.6 / 1.6 % | 1.5 / 1.6 % | | | |
| Helpers 0–20 / 30–50 (cores) | – | 0.015 / 0.012 | – | – | | | |
| Private total 0–20 / 30–50 (MB) | 5011 / 5007 | 4859 / 4855 | 1323 / 1319 | 1325 / 1321 | −3 % | −73 % | −73 % |
| Working set total 0–20 / 30–50 (MB) | 4719 / 4715 | 4584 / 4580 | 1187 / 1183 | 1188 / 1184 | −3 % | −74 % | −74 % |
| CPU, profiled runs 0–20 / 30–50 (cores) | 0.86 / 0.83 | 0.81 / 0.79 | 0.41 / 0.37 | 0.39 / 0.38 | | | |
| Callback mean / max, 0–30 s (ms) | 3.93 / 6.12 | 3.57 / 6.55 | 0.59 / 1.28 | 0.65 / 1.21 | −9 % / +7 % | −83 % / −80 % | −82 % / −82 % |
| Callback mean / max, 30–60 s (ms) | 3.87 / 6.66 | 3.62 / 6.29 | 0.60 / 1.41 | 0.66 / 1.30 | −6 % / −5 % | −83 % / −78 % | −82 % / −79 % |
| Budget max 0–30 / 30–60 (%) | 57.3 / 62.4 | 61.4 / 59.0 | 12.1 / 13.3 | 11.3 / 12.1 | | | |

Effect tails were not assumed to be over: master output still had a peak of about 0.04 in the
0–30 s window in every case, and 0.000 in the 30–60 s window.

### Realtime safety, every window and run

- No block over 70 % of budget, except one block at 70.8 % in r2-B-ctl, 0–30 s after Stop.
- No overruns.
- No late callback starts (> 1.25 period); the profiled runs measure this.
- No proxy underruns.
- No read-ahead misses.

### Loaded and processed

| | A | B | C | D |
|---|---|---|---|---|
| Instruments loaded / processed (live hosts) | 14 / 14 | 14 / 14 | 6 / 6 | 6 / 6 |
| Insert instances loaded | 58 | 58 (10 AmpliTube in the helper) | 4 | 4 |
| Insert stages resting before Play (`restingNow`) | (no rest in A) | 11 of 40 | 0 of 4 | 0 of 4 |
| Insert stages resting at end of playback / after Stop | – | 1 / 1 | 0 / 0 | 0 / 0 |
| Insert stage-blocks rested : processed, playback window incl. warm-up | – | 17.7 k : 32.0 k | 0 : 6.6 k | 0 : 6.6 k |
| Same, 30 s after Stop | – | 2.9 k : 40.0 k | 0 : 5.7 k | 0 : 5.7 k |
| Instrument blocks rested : processed, playback | – | 0 : 46.3 k | 0 : 19.9 k | 0 : 19.9 k |
| Read-ahead rows owned during playback | 16 | 16 | 0 (no audio row On) | 0 |
| Proxy selected / proxy jobs | 0 / none | 0 / none | 0 / none | 0 / none |

What the counters mean:
- **Stage-blocks** = one insert stage for one audio block, counted as either processed or rested.
- **`restingNow`** = stages resting when the line was printed.
- Instruments never rest by design.

### Sound and transport (automatic checks)

- **Transport**: advanced 30.0 s during each 30 s playback window.
- **Master output, playback window**:

| Case | Peak | RMS L |
|---|---|---|
| A | 0.56 | 0.071 |
| B | 0.56 | 0.071 |
| C | 0.51 | 0.072 |
| D | 0.45 | 0.066 |

- **C vs D**: only track 40's mute differs between them, and D's RMS is lower. That is consistent
  with track 40 (cello) sounding in C.
- **All six instrument hosts** processed in C and D.
- **Not verified**: whether each drum and cello row sounds on its own. The existing diagnostics
  have no per-track meter in this scenario. These are automatic checks, not listening.

## 4. Conclusion

**A→B, same project.** The resource-saving work lowers CPU most while stopped before Play: total
CPU drops 45 % (0.87 → 0.48 core) and the audio callback 75 % (3.8 → 1.0 ms). During playback
CPU falls 26 % (0.99 → 0.73 core). After Stop the gain shrinks to 13–20 % in CPU, and the callback
stays near A's level (3.6 vs 3.9 ms). Before Play, 11 of B's 40 insert stages rest. After one
playback only one does, and the 11 Pro-R instances keep processing for at least 60 s after Stop.
This was observed only; the cause was not investigated (no new optimisation work).

Total memory hardly changes (−3 %, 5.0 → 4.85 GB private). DAL itself drops from 5.0 to 2.15 GB,
because AmpliTube's 2.7 GB moves to the helper process. That isolation is a stability change, not
a memory saving. Load time and load peak are the same within the spread.

**B→C, the small variant.** Memory falls by about three quarters:
- 4.85 → 1.32 GB private and 4.58 → 1.18 GB working set;
- load peak 4.5 → 1.1 GB;
- project loaded in 5.7 s instead of 16.8 s.

CPU falls 24 % stopped before Play, 40 % during playback and 42–48 % after Stop. The callback
after Stop falls about 80 %. **B→D shows the same numbers**, so the saving comes from Off. The
other project differences have no measurable effect here: track 40's mute, the Groove Agent
states, the proxy and the UI fields.

**What still dominates the load in C:**
- **VB3-II** is the largest plug-in cost: two instances, about 0.78 ms of summed plug-in time per
  block out of about 1.3 ms, both while stopped and while playing. One of them is muted, and the
  other has no notes in the passage. Instruments never rest, and neither is Off.
- Groove Agent SE (0.29 ms) and HALion Sonic (0.21 ms) come next during playback.
- The audio callback itself is only about 0.06–0.07 core of C's 0.36–0.44 core. Most of the
  process CPU is outside the callback (render workers' waiting and the UI/message thread). This
  run does not break that down further.

## 5. Variation and limits

- **Spread**: round 2 ran later and was slower for A and B.
  - Load took up to 20.6 s, against 15.6 s in round 1.
  - A's stopped CPU was 0.97 against 0.76 core, and B's profiled playback 0.89 against 0.77 core.
  - C and D varied little: within ±0.03 core, ±0.1 s load and ±20 MB.
  - Every case had runs in both rounds in mirrored order, so the A→B and B→C conclusions do not
    depend on the order. Relative figures under about ±10 % for A and B are within noise.
- **No p99** of callback time: the existing diagnostics give mean, max, blocks over 70 % and
  overruns, but no percentile. No diagnostic code was added for this.
- **After-Stop callback windows** are 0–30 s and 30–60 s (scenario window length). External CPU
  and memory use exactly 0–20 s and 30–50 s.
- **Memory peaks** are 250 ms samples of the summed processes, so a shorter spike can be missed.
  The OS per-process peak working set is in each run's `samples.txt`.
- **Profiling**: the control runs switch detail profiling off but still run the stability
  scenario and its engine load meter. They are not overhead-free. Profiled and control CPU are
  shown separately.
- **Base binary**: A was rebuilt from 8472feb with the release compiler for this comparison. It is
  not the distributed 1.4.0 installer exe (dae9c68).
- **Window state**: the MIDI editor workspace and main window position were made identical in
  the temp copies. The mixer and plug-in editors were closed by the scenario in every run.
- **Project deviation**: 261106 has two VB3-II rows On, in addition to the expected drums and two
  cello references.
- **Other computers**: this is a 24-core desktop with a Fireface at 512 samples. No conclusion
  about Conny's laptop follows from these numbers: core count, clock, memory and audio driver
  differ.
- **No listening** was done. The sound checks are automatic (master meter, processed hosts,
  transport advance).

## 7. Which file case C measured (checked 2026-11-08, read-only)

Added after the question of whether C represents the real 261106 project. Checked only by reading
project files and existing logs; DAL was not started.

**Files and hashes**

| File | Path | SHA-256 | Written |
|---|---|---|---|
| Original | `C:\Users\nicla\Dropbox\TWA Shared\DALprojects\Saved Projects\TSE_pt2_261106\TSE_pt2.dalproj` | `E5CEF985078F5A390039B79A2ECDC5A0BDCF1BE80F20388EF98CAEBBBAC8E259` | 2026-11-06 07:27:50 |
| Unchanged temp copy | `%TEMP%\dal-cmp3\TSE_pt2_261106\TSE_pt2.dalproj` | same as the original | same |
| Measured source | `%TEMP%\dal-cmp3\TSE_pt2_261106\C.dalproj` | `5BFA8F791AF1372B7D378CA06F2A69AC71C1740C0EB858493BD8791FA5B6A90B` | 2026-11-08 20:13 |

- **Original unchanged**: the original had the same hash before copying (20:0x), after the series
  (21:0x) and again at this check. No file in the original folder was written after 2026-11-06.
- **C.dalproj** was written by `variants.py` from the copied 261106 file. It is not derived from
  261031, D or any older all-Off fixture.
- **Per run**: each C run loaded its own copy `run-<tag>.dalproj` of C.dalproj, deleted after the
  run.

**Changes in C.dalproj against the original**: exactly two fields, `mainWindow.x` (−2414 → 33)
and `mainWindow.y` (321 → 282). There are no Off, Mute, state, clip or routing changes.

**Not mixed up with D or the all-Off fixture:**

| Property | Original 261106 | C copy | D | Old all-Off fixture (`TSE_pt2-alloff.dalproj`) |
|---|---|---|---|---|
| Rows Off | 45 of 52 | 45 (identical per track) | 45 | 51 |
| Rows muted | 42 | 42 | (as 261031) | – |
| Track 40 muted | no | no | yes | – |
| Saved playhead | 3263232 | 3263232 | 181473 | – |

Off and Mute are separate fields in the file:
- **Off** = the track's `off` flag; for instrument rows the instrument entry's `powerOn: false`
  as well.
- **Mute** = `muted`.

The per-track table (TrackId, name, kind, Off and Mute in original and copy, D's Off, the
fixture's Off) is in `evidence/.../offcheck.md`.

**The rows that are not Off in the file** (all others are saved Off):

| TrackId | Name | Mute |
|---|---|---|
| 3 | Track 3 (Groove Agent SE) | no |
| 25 | Track 3 — kopia (Groove Agent SE) | no |
| 45 | Track 8 — kopia 2 — kopia (HALion Sonic) | no |
| 40 | Track 8 — kopia 3 (HALion Sonic) | no |
| 5 | VB3-II | no |
| 23 | VB3-II — kopia 2 | yes |
| 2 | Stereo Out (master, no Off) | – |

**Loaded model in the C runs**: all four C runs logged
`tracks: … (muted=42 off=45) … primaryLoaded=6 primaryEmpty=8 … insert chains=46 instances=4`.

**Your own interactive open of the original** (installed DAL 1.4.1), from the existing
`project-load-diag.log`:
- Load on 2026-11-08 at 21:18:55, closed at 21:26:08.
- It logged `instrument kept unloaded (track Off)` for tracks 8, 26, 46, 57, 54, 65, 9 and 24.
  Track 7 (MIDI 1) is Off and has no instrument.
- It instantiated Groove Agent SE (3, 25), HALion Sonic (45, 40) and VB3-II (5, 23).
- At close it destroyed exactly those six instruments and four effects (2 × DAL Mono Delay,
  2 × Pro-Q 3; `effects detached count=4`).
- No On/Off toggle was logged in that session (`track-power-diag.log` was last written
  2026-11-06), and no autosave was written (`autosave-diag.log` was last written 2026-11-06).

So **file, loaded model and instantiated plug-ins agree**: 6 instruments On, 45 rows Off, and C
measured that project with only the window position changed.

**Power button display**, from reading the code (not observed on screen):
- Audio rows draw the Power cell from `Track::isTrackOff()` (`TrackLanesView.cpp:1795`).
- Instrument rows draw it from the instrument controller's `powerOn`
  (`InstrumentTimelineRowCoordinator.cpp:2302`), which is loaded from the saved `powerOn`.
- Every Off instrument row in 261106 has both flags set Off, so the two sources agree for this
  file.
- An Off row is still shown as a row; its power glyph is grey (`0xff5a5858`), and an On row's is
  green.
- Whether the screen showed something else in your session cannot be determined from files and
  logs. It would need a look at a fresh temp copy in DAL, which was not done here.

## 6. Evidence

`docs/evidence/cpu-memory-abcd-2026-11-08/`:
- **Per run (16 runs)**: `runs/<run>/run.log` (scenario log), `samples.txt` (250 ms samples of
  every DAL process: pid, CPU seconds, private, working set, OS peak working set) and `meta.txt`
  (exe, hash, arguments, exit, leftover processes, crash files, proxy file count).
- **Tables**: `summary.md` (generated) and `parsed.txt` (per run).
- **Project comparison**: `projdiff.txt`, `instr.txt` and `variants.txt`.
- **Scripts**: `scripts/` (runner, series, parser, summariser, variant builder, base build).
