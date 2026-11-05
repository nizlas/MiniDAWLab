# CPU and memory on the real TSE_pt2 project — DAL 1.4.0 (2026-11-04)

**Question.** A tester (Conny) opened `TSE_pt2_261031` with the 1.4.0 installer on a laptop
(preliminary: 4 cores, 8 GB RAM; exact CPU, driver and buffer unknown). The process took about
4 GB while loading and then sank slowly towards 1.7 GB, CPU sat near 50 % with the transport
stopped, and playback was not usable. Why?

**Short answer.**

1. **Idle CPU is plug-in DSP that never rests, not a spinning DAL thread.** With the transport
   stopped, every unmuted audio row with an active insert chain is fed silence on the callback
   thread, every block, indefinitely (the stopped-tail pass added in 1.4.0, `5d02639`). Every
   instrument also processes every block, muted or not (only Off skips it, unchanged since
   1.1.9). In this project that is 11 × FabFilter Pro-R on unmuted audio rows, 3 × VB3-II,
   9 × HALion Sonic, 2 × Groove Agent SE and one AmpliTube 4. DAL's own work outside plug-in
   calls is about 2 % of the callback. The stopped state costs about as much CPU as playback.
   On the stopped callback it costs more wall time, because the stopped tails run serially on
   the callback while playback spreads the same chains over the read-ahead worker and the
   render pool.
2. **The memory is the plug-ins' own, mostly AmpliTube 4.** Private bytes reach 5.0 GB
   (installed 1.4.0: 4.75 GB) during the plug-in restore and stay there. There is no transient
   spike or second copy. Ten AmpliTube 4 instances account for about 2.9 GB (about 290 MB each),
   and 9 of them sit on muted rows. **Hypothesis, not confirmed:** on an 8 GB machine this
   probably cannot stay resident, and a working set that "sinks to 1.7 GB" while the plug-ins
   still hold about 5 GB of private memory would then be Windows trimming the process, which
   means paging. If so, that is the most likely reason playback was unusable. Nothing on
   Conny's machine has been measured; the hypothesis stands or falls with his commit size and
   hard-fault rate (section 7).
3. **An unrequested proxy render does happen.** The two Groove Agent rows have no saved update
   mode, so they read as **Auto**. With no edit, Auto re-rendered both proxies about five
   minutes after opening and wrote proxy metadata into the project file. It cost a few seconds
   of background CPU here and is not the steady 50 %, but it does write to the user's project.

Nothing in production code was changed. Code added: a diagnostics-only `--idle` /
`--idle-gap N` option for `--stability-perf-profile`, which measures with the transport stopped
before the first Play, right after Stop, and after a gap.

---

## 1. What was compared

| | |
|---|---|
| Distributed 1.4.0 | `C:\Program Files\Danielssons Audio Lab\MiniDAWLab.exe`, FileVersion 1.4.0, 10 755 584 bytes, 2026-10-25 19:12. Tag `v1.4.0` = `dae9c68` (2026-10-25 19:06). Release build. |
| Current main | `b466b2c` plus uncommitted multi-track-delete UI work and this diagnostics option. Built Release with ASIO in `build\ninja-release`. |
| Differences relevant here | `git diff v1.4.0 HEAD -- src` touches only MIDI editing, snap, the ruler and scissors. The engine, the render pool, read-ahead, the stopped-tail pass and the proxy policy are identical. Idle CPU and memory matched within noise (main 0.66–0.74 core, 5.0 GB; installed 1.4.0 0.78 core, 4.75 GB). |
| Machine | Intel Core Ultra 9 275HX, 24 physical = 24 logical cores, 95 GB RAM (69 GB free), Windows 11. ASIO Fireface USB (RME Babyface Pro), 48 kHz, 512 samples (10.667 ms budget; the driver offers only 512). |
| Project | A temp copy of `TSE_pt2_261031` in `%TEMP%\dal-perf-tse261031` (all 76 audio sources and 5 referenced proxy files present). The original was never opened. |
| Settings | `%APPDATA%\MiniDAWLab\*.xml` were backed up first; `audio-device.xml`, `read-ahead.xml`, `audio-latency.xml`, `ui-layout.xml` and the instrument catalog were byte-identical afterwards. |

**Uncertainty.** All plug-ins in the project are installed here. It is not known whether
Conny's machine has the same set, versions or content (HALion and Groove Agent libraries). Our
runtime is not claimed to be identical to his.

### Project inventory (temp copy)

* 52 rows: 36 audio (25 muted, 11 unmuted), 14 instrument (9 muted), 1 MIDI, Stereo Out.
  All rows route straight to Stereo Out; there are no group buses and no sends.
* Instruments: 9 × HALion Sonic (8 on muted rows), 2 × Groove Agent SE (unmuted), 3 × VB3-II
  (one muted), plus 1 plug-in-less MIDI row. Every instrument is a live Primary; no proxy is
  selected for playback, because the locked source priority plays an available Primary.
* Inserts (46 restored): Pro-R × 31 slots on audio rows (11 active on unmuted rows, the rest Off or on
  muted rows), AmpliTube 4 × 10 (9 on muted rows), DAL Mono Delay × 13, Pro-Q 3 × 3, DAL Tuner × 1.
* Monitor and Record Arm are runtime-only and start off. No Solo is active (one Solo memory is
  stored but not engaged). MIDI inputs "Babyface Midi Port 1" and "CASIO USB-MIDI" are enabled
  in the audio settings; no MIDI was played during measurement.
* Read-ahead is on (saved choice `enabled="1"`, depth 3). It adopts up to 16 audio rows while
  playing and owns none while stopped. The render pool has 7 workers here
  (`defaultWorkerCount` = min(7, physical / 2 − 1)).
* Proxy update modes: HALion rows `off`, VB3-II rows `onSave` / `manual` / `manual`, and the two
  **Groove Agent rows (3, 25) have no saved mode, which the reader repairs to `auto`**.

## 2. Idle and playback measurements

The phases were run in one process with `--stability-perf-profile <copy> --seconds 30
--warmup 5 --idle --idle-gap 30` (profiler on). Thread CPU was sampled from outside with
`GetProcessTimes`-equivalent per-thread counters. "Core-equivalents" is process CPU time
divided by wall time. "Windows %" is that divided by the 24 logical processors here, which is
what Task Manager shows. On a 4-logical-processor laptop the same core-equivalents would show
about six times higher.

| Phase | Callback ms mean / max (budget % mean / max) | Overruns | Process CPU (core-equiv.) | Windows % (24 logical) | Private |
|---|---|---|---|---|---|
| A. Empty project, installed 1.4.0, device running | — | — | 0.10 | 0.4 % | 0.20 GB |
| User's own running 1.4.0 session with TSE_pt2, stopped (read-only) | — | — | 0.51 | 2.1 % | 4.5 GB (peak WS 4.8 GB) |
| B. Loaded, never played (main) | 3.26 / 5.76 (30.6 / 54.0 %) | 0 | 0.74 | 3.1 % | 5.0 GB |
| B. Loaded, never played (installed 1.4.0) | — | — | 0.78 | 3.3 % | 4.75 GB |
| Playing (cycle 0 s →, 30 s) | **0.71** / 1.55 (6.7 / 14.6 %) | 0 | 0.72 | 3.0 % | 5.0 GB |
| C. Right after Stop (0–30 s) | 3.14 / 6.12 (29.5 / 57.3 %) | 0 | 0.66 | 2.8 % | 5.0 GB |
| C. Stopped, after 30 s gap | 3.12 / 5.58 (29.3 / 52.3 %) | 0 | 0.66 | 2.7 % | 5.0 GB |
| C. Stopped, about 6.5 min after Stop (long run) | 3.07 / 5.47 (28.8 / 51.3 %) | 0 | 0.67 | 2.8 % | 5.0 GB |

There is no decay: the stopped cost is the same before the first Play, right after Stop and
minutes later.

### Where the stopped block goes (per 512-sample block, profiler)

| | ms per block | Note |
|---|---|---|
| Insert plug-ins | 2.80 | Pro-R × 11 = 2.44, AmpliTube 4 (row 22) = 0.31, Pro-Q 3 × 2 + DAL Mono Delay × 2 = 0.06. Pro-R and AmpliTube run only because of the stopped-tail pass. |
| Instrument plug-ins (CPU summed over render threads) | 1.43 | VB3-II × 3 = 0.99, HALion Sonic × 9 = 0.27, Groove Agent SE × 2 = 0.17. Generation wall on the callback is 0.39 ms (7 workers). |
| DAL outside plug-in calls | 0.07 | 2 % of the callback |

While playing, the same plug-ins cost about the same CPU (inserts 1.66 ms plus the read-ahead
worker, instruments 1.60 ms summed). The callback only waits 0.71 ms because read-ahead renders
16 audio rows ahead on its worker and the pool runs the rest. Stopped, the tails run serially
inside the callback.

### Threads (stopped, 15 s windows)

| Thread | % of one core | |
|---|---|---|
| ASIO callback (`fireface_asio_amd.dll`, priority 26) | 28–30 | matches 3.1 ms / 10.67 ms |
| Message thread (`MiniDAWLab.exe` main) | 14–20 | 5 % in the empty project; transport stopped, mixer closed. **Unexplained**: no sampling profile was taken, so which UI, timer or plug-in work runs there is unknown. |
| Render workers (7, MMCSS priority 24) | about 2 each | They block on `std::atomic::wait` between blocks and do not spin. The callback spins at most 150 µs at the join. |
| Others (plug-in threads, GPU driver, read-ahead) | under 3 each | Read-ahead owns no rows while stopped and polls with a 300 µs sleep. |

No thread spins. The idle CPU is the callback doing plug-in DSP, the workers doing instrument
DSP, and the message thread, whose 14–20 % of one core remains unexplained.

### What this means for a 4-core laptop (estimate, not a measurement)

* `defaultWorkerCount` gives **1** worker on 4 physical cores and **0** (fully serial) on
  3 or fewer. If "4 cores" means 2 physical / 4 logical, everything runs on the callback.
* Total idle work is about 0.7 core-equivalents of a Core Ultra 9 P-core. A laptop core that is
  2–3 × slower needs about 1.4–2 cores for the same work. That is 35–50 % of four logical
  processors and consistent with what Conny saw.
* The stopped callback (about 3.1 ms here, mostly serial Pro-R tails) would be about 6–9 ms on
  such a core, close to the 10.67 ms budget at 512 samples, before paging is considered.

## 3. Stopped state and the tail changes (code review)

* `PlaybackEngine::renderStoppedAudioInsertTails` (1.4.0, `5d02639`) runs on every stopped or
  paused block. For each audio row that is not Off, not muted, has an active slot, is not owned
  by read-ahead and is not monitored, it feeds zeros through Pre → fader → Post → pan
  (`renderAudioTrackPostStripToStereoScratchWithChainAccess` with `feedSilenceOnly`). There is
  no rest condition. It runs before the first Play too, when no signal has ever entered the
  chain. Muted rows are skipped (gain 0), which matches the measured 11 Pro-R rather than 31.
* Instruments: `mixExperimentalInstrumentAfterTracks` processes every loaded host every block;
  Mute and −∞ only zero the fold, and only Off skips the host (1.1.9 rule). The 8 muted HALion
  rows therefore still cost CPU (cheap here, 0.03 ms each).
* The render pool is never used for the stopped tails. Stopped blocks keep generation-only
  dispatch plus serial strips.
* No row is processed twice: the stopped pass skips read-ahead-owned and monitored rows, and
  the pool claims each job once.
* No plug-in was seen producing signal by itself (output peak 0 while stopped).

**Verdict.** This is not a local defect such as busy-waiting or double processing. It is a
missing rest policy: tails are correct, but the chains never stop.

**Proposed policy: a proposal only, not approved for implementation and not implemented.**
It needs a decision before any code is written.

1. Per audio row, track whether the chain has received non-silent input since its last
   prepare or reset. A chain that has only seen silence since prepare needs no stopped
   processing. That covers the loaded-never-played case without any timeout.
2. After real input, keep feeding silence until the chain output has been below −120 dBFS for
   at least the chain's reported tail (`getTailLengthSeconds` summed over active slots, with
   the latency added). A plug-in that reports an infinite or unknown tail keeps running, as
   today, so a long delay is never cut. A quiet interval alone never decides.
3. Any input wakes the chain: Play, Monitor, live MIDI, a parameter or slot edit, a chain change,
   or Active / Bypass. Monitored rows stay on the live-input pass as today. Mute and Off keep
   their meaning. Each row keeps its single owner, the callback or read-ahead, so no instance
   is processed on two threads.
4. Separately, dispatch the stopped tails through the render pool, as playback already does.
   That lowers callback wall time and deadline risk on few cores without changing total CPU.

Instruments resting while idle (no MIDI in, output silent past the tail) is the same policy
with a MIDI wake. It is worth less here (0.27 ms for 9 HALion).

## 4. Memory

| Measure | Value |
|---|---|
| Private bytes, empty project (installed 1.4.0) | 0.20 GB |
| Private bytes, peak during load = stable level (main) | 5.02 GB peak, 5.01 GB stable for 9 minutes |
| Private bytes, installed 1.4.0, same copy | 4.77 GB peak during load, 4.75 GB stable |
| Same copy **without the 10 AmpliTube 4 inserts** (main) | 2.16 GB peak, 2.14 GB stable |
| Page faults while stopped | 0–3 per second (69 GB free here; no paging) |

* The rise follows the plug-in restore. Instruments took 34.0–38.2 s on the load clock
  (memory about 0.5 → 1.7 GB), Pro-R and the other small inserts were near zero, and the ten
  AmpliTube 4 instances took 38.6–48.0 s (memory 1.7 → 5.0 GB). The AmpliTube-free copy
  confirms about 2.87 GB for the ten instances.
* DAL's own data is small: 76 audio sources decode to about 83 MB as float, plugin state blobs
  are 0.1–0.2 MB each, and the five referenced proxy files total about 220 MB on disk (no proxy
  is selected for playback). There is no load-time double buffer, and private bytes stayed at
  their load peak.
* **No growing leak was observed during the measurement period** (about 9 minutes after load,
  covering stopped, 30 s of playback and stopped again, with no editing). That is a statement
  about this run only. It is not evidence that DAL or the plug-ins have no leak under longer
  sessions, editing, repeated load/unload or other projects.
* **Paging hypothesis (not confirmed).** If Conny's private bytes also stay near 5 GB while the
  working set falls to 1.7 GB, the drop cannot be memory being freed (the plug-ins still hold
  it). It would be Windows trimming the working set under memory pressure on an 8 GB machine,
  so later access to plug-in memory, such as sample data and amp models, becomes hard page
  faults. We have only Conny's description of the working set, not his private bytes, commit
  or fault rate, so this remains a hypothesis.

## 5. Playback

The 30 s cycle passage ran with no overruns (0.71 ms mean, 1.55 ms max, 0 late starts). On this
machine playback is not the problem. For Conny, idle load and playback probably share one cause
(plug-in DSP running all the time). The unusable playback may add a second one: paging,
because 5 GB of plug-in memory may not stay resident in 8 GB. That second cause is a
hypothesis until Conny's measurements confirm or rule it out. Nothing was listened to; no
claim about audible quality is made.

## 6. Proxy rendering

| | 1.4.0 and main (identical code) |
|---|---|
| Default for a new instrument track | `off` (`fe44693`, since 1.2.0) |
| Default for an older row without a saved mode | `auto`: the reader repairs a missing or unknown value to `auto`, and the writer omits `auto`. A missing mode in an existing file therefore **cannot be told apart from a historical explicit Auto choice**: both were written the same way. |
| This copy | Groove Agent rows 3 and 25 `auto`; VB3-II rows `onSave`, `manual`, `manual`; HALion `off` |
| At load | No render. The policy's first observation of a destination is not treated as a change. |
| Idle, no edit, transport stopped | **Both Auto rows re-rendered about 5 min after opening** (new generations `track_3_…695369…`, 23.9 MB, and `track_25_…97bb5e…`, 37.4 MB), and each wrote a metadata-only checkpoint into `TSE_pt2.dalproj` (the copy's mtime changed). There were no further renders in the remaining 4 minutes. |
| Leftovers | Thread count returned to normal and private bytes were unchanged after the jobs. No extra instance was kept. |
| Off / Manual / On Save rows | Nothing rendered. |

The render was triggered by an observed identity change with no edit. The fingerprint includes
the Primary semantic revision, and that revision is bumped by any host-observed
`audioProcessorParameterChanged` outside a MIDI-bearing processBlock and by every
`audioProcessorChanged`. Groove Agent evidently emits one of these after the project has
loaded. Which callback it is was not attributed in this pass (a one-line log in
`ProxyUpdatePolicyService::pollOne` on change would show old/new fingerprint and revision).

Classification:

1. **Live Primary processing** is the idle cost above.
2. **Playback of an existing proxy file** was not in use (0 selected destinations).
3. **Offline proxy rendering** happened once per Auto row: a few seconds of background CPU
   here, about 5 minutes after opening, and a write to the project file.
4. **Read-ahead of audio strips** ran only while playing (16 rows), with no work while stopped.

Proxy rendering is therefore not the steady 50 %, but it is unrequested background work plus
a project-file write on open. Per the brief this is not the "render despite Off/Manual" defect;
it is Auto behaving as coded on a row with no saved mode. Whether someone once chose Auto on
those rows cannot be known from the file, because the writer omitted both an explicit Auto and
an unset mode. Two decisions are open: what a missing mode should mean (the writer would have
to start persisting `auto` before the reader default could change, and even then older files
stay ambiguous, so any new default would also change rows where Auto was once chosen), and whether
plug-in-originated notifications with no host edit should arm Auto. Fingerprint and Stale rules
were not touched.

## 7. Still unknown, and the smallest observation needed from Conny

1. CPU model and the physical / logical counts (Task Manager → Performance → CPU: "Cores",
   "Logical processors").
2. Audio driver and buffer size (DAL → Audio settings).
3. With the project open and stopped for a minute, then while playing: Resource Monitor →
   Memory → `MiniDAWLab.exe` **Commit (KB)**, **Working Set (KB)** and **Hard Faults/sec**,
   plus Task Manager "In use / Committed" for the whole machine.

Point 3 confirms or rules out the paging hypothesis, deciding between "CPU-bound" and
"paging-bound". Separately, the message thread's 14–20 % of a core while idle (5 % in an empty
project) is still unexplained and needs a sampling profile.

## 8. Reproducing

```
MiniDAWLab.exe --stability-perf-profile "<copy>\TSE_pt2.dalproj" --seconds 30 --warmup 5 --idle --idle-gap 30
```

Use a temp copy: an Auto row may re-render and write the project file after about 5 minutes.
Evidence (bounded extracts): `docs/evidence/cpu-memory-tse-pt2-2026-11-04/`.
