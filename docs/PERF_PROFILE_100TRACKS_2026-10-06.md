# Audio-thread cost profile — the 100-track load-test project (2026-10-06)

> Follow-up (1.1.18): recommendation 1 was implemented — see
> [`docs/PARALLEL_INSTRUMENT_RENDERING_2026-10-06.md`](PARALLEL_INSTRUMENT_RENDERING_2026-10-06.md)
> (callback mean 9.9 → 2.1 ms at 512, 0 overruns, same build serial vs parallel).

**Question.** Why does the 100-track project crackle, and where does the callback time go?

**Answer in one paragraph.** The audio callback is a single thread that processes every
instrument plug-in serially, and at 512 samples (10.67 ms budget) the 48 live instrument
`processBlock` calls alone take **13.7 ms per block** (129 % of the budget); with inserts and DAL
bookkeeping the callback takes **14.7–14.8 ms (138 %)** on average and every single block
overruns. VB3-II ×16 costs 7.3 ms (50 % of the callback), Groove Agent SE ×16 5.0 ms (34 %),
HALion Sonic ×16 1.4 ms (9 %); the 32 processed inserts 0.47 ms (3 %); everything DAL does
outside plug-in calls — MIDI scheduling, clip rendering, strips, routing, summing, meters —
0.56 ms (4 %, ≈ 5 % of the budget). No proxy is in use (every destination plays its live
Primary by the locked source priority), nothing clips, nothing pages. Process CPU is ~155 % of
one core = 6.5 % of 24 cores: the "< 10 % CPU" the task manager shows is one saturated audio
thread plus a little UI, not spare realtime capacity.

Nothing was optimised; this is diagnosis + recommendation only. Code added: the opt-in
`AudioThreadProfiler` and the `--stability-perf-profile` scenario (both off / absent in normal
use). Raw logs: `docs/evidence/perf-profile-2026-10-06/`.

---

## 1. Run conditions and the project's actual load

| | |
|---|---|
| Exe | `build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe`, version 1.1.17, `config=Release`, built from `7a9d550` + this slice's profiler code (Ninja / MSVC, ASIO SDK) |
| Machine | Intel Core Ultra 9 275HX (24 physical = 24 logical cores, hybrid P/E), 95.3 GB RAM, ~70 GB free, Windows 11 Home 26200 |
| Audio | ASIO "ASIO Fireface USB" (RME Babyface Pro), 48 000 Hz, **buffer 512 samples** (10.667 ms budget), output latency 552 samples (11.5 ms); the driver offers exactly one buffer size: `offeredBufferSizes=512` |
| Project | temp copy of `C:\Users\nicla\Music\TSE_pt2_100tracks\TSE_pt2_100tracks.dalproj` (the user's file and the original were not touched; the copy sat in `%TEMP%\dal-perf-100tracks`) |
| Tracks | 36 audio (all 36 muted), 48 instrument, 16 MIDI, 1 Stereo Out — 101 rows incl. master; cycle on over the project's 0–41 s loop |
| Instruments loaded | 48 instrument hosts, 48 with a loaded Primary (16 × VB3-II, 16 × Groove Agent SE, 16 × HALion Sonic); 0 Secondary instances loaded (the 16 configured HALion Secondaries are instantiated lazily and were never needed) |
| Instruments **processed** | 48 — every host ran live `processBlock` every block (`hosts with live processBlock blocks=48`, proxy-mixed 0) |
| Proxies | 0 selected. All 48 destinations report `Primary`: the locked source priority (`decideProxyPlaybackSource`, PI-021) plays the live Primary whenever it is available; the rendered generations of track 3 / 5 and their copies are therefore unused on this machine. Proxy underruns: 0 (not applicable) |
| Inserts loaded | 50 chains / 50 instances (18 × AmpliTube 4 on the audio rows, 16 × Pro-Q 3, 16 × DAL Mono Delay on instrument rows) |
| Inserts **processed** | 32 (Pro-Q 3 ×16 + DAL Mono Delay ×16). The 18 AmpliTube instances are loaded but never processed: their audio rows are muted and the strip skips the insert chain at gain 0 |
| Memory | working set 9.35–9.41 GB, private 9.6 GB, stable across runs; page faults 110–4 266 per 45 s (soft+hard combined, ≤ 95/s) with 70 GB free RAM — no evidence of paging |

Procedure per run (`--stability-perf-profile <copy> --seconds 45 --warmup 5 [...]`): load, settle
8 s, log runtime + inventory, mixer closed (or opened), seek to 0, start playback with the
project's cycle on, discard a 5 s warm-up, then measure 45 s: engine load window
(`PlaybackEngine::snapshotAudioCallbackLoadAndReset`), profiler snapshot, output peak hold +
Stereo Out meter, proxy-reader underrun deltas, process CPU / memory (`GetProcessTimes`,
`K32GetProcessMemoryInfo`, `GetSystemTimes`). The window starts after warm-up, so start-up /
first-block costs are excluded; loading (24 s) is separate.

## 2. Runs

All at 512 samples / 48 kHz, mixer closed unless stated. "budget %" = callback duration / 10.667 ms.

| Run | Variant | Blocks in 45 s | Callback ms min / mean / max | Budget % mean / max | Blocks ≥ 100 % | DAL remainder ms mean / max | Process CPU (one core) |
|---|---|---|---|---|---|---|---|
| 1 | profiler ON | 3 036 | 11.56 / **14.80** / 19.85 | 138.8 / 186.1 | **3 036 of 3 036** | 0.567 / 1.412 | 155 % |
| 2 | profiler OFF | 3 362 | 9.23 / 13.35 / 21.44 | 125.1 / 201.0 | 3 237 of 3 362 (+125 at 70–100 %) | — | 151 % |
| 3 | profiler ON, **mixer OPEN** | 3 029 | 11.43 / 14.84 / 19.16 | 139.1 / 179.6 | 3 029 of 3 029 | 0.628 / 1.014 | 169 % |
| 4 | profiler ON (repeat of 1) | 3 054 | 11.39 / 14.72 / 18.88 | 138.0 / 177.0 | 3 054 of 3 054 | 0.562 / 1.109 | 156 % |
| 5 | profiler OFF (repeat of 2) | 3 050 | 11.28 / 14.74 / 19.23 | 138.2 / 180.3 | 3 050 of 3 050 | — | 158 % |
| 6 | `--buffer 1024` request, 10 s | 788 | 9.50 / 12.65 / 16.87 | 118.6 / 158.2 | 696 of 788 | — | — |
| — | **1024 samples** | **not measurable from DAL on this driver** — see §2.1 | | | | | |

Observations that hold in every run:

* **Every block overruns.** 3 036 of 3 036 blocks took longer than 10.667 ms in run 1; the same
  in runs 3–5. Run 2 was the only one with a faster stretch (min 9.2 ms) and still had 96 %
  overruns. This is the crackle: the driver delivers a block late every ~15 ms.
* **Playback runs slow.** In 45.0 s of wall time the transport advanced 1 554 432 samples =
  32.4 s of music (72 % real time); callbacks came 3 036 times instead of the nominal 4 219. The
  callback-start interval averaged 14.8 ms (= the callback duration): the device waits for the
  callback, so "late starts" (2 560 of 3 037 intervals > 1.25 × period) are a consequence of the
  overrun, not a separate driver problem.
* **Not clipping.** Device output peak 0.37, Stereo Out overs 0/0, no non-finite samples
  (Stereo Out at −30 dB as built). The distortion the user hears is dropout, not overload.
* **Profiler overhead is inside run-to-run noise.** Profiler ON 14.72 / 14.80 / 14.84 ms vs OFF
  14.74 ms (run 5); run 2's 13.35 ms is the spread of the machine itself (CPU clock / core
  placement of the ASIO thread — a hypothesis, not measured). The profiler adds ~160 clock reads
  per block (≈ 5 µs).
* **The mixer window does not change the callback.** Open vs closed: 14.84 vs 14.80 ms mean,
  max 19.16 vs 19.85; DAL remainder +0.06 ms (the per-row meter folds). The mixer costs the
  message thread ~14 % of a core (169 % vs 155 %), never the audio thread.

### 2.1 Why 1024 was not measured here

`AudioIODevice::getAvailableBufferSizes()` returns `{512}` for "ASIO Fireface USB": the RME
driver reports min = max = preferred = the size chosen in its own Settings dialog and ignores host
requests. Run 6 asked for 1024 through the same `setAudioDeviceSetup` path the settings UI uses:
`buffer request 1024 -> actual 512 | was 512 ok (driver kept its own size)`. DAL cannot change it;
no control-panel automation was built.

**To get the 1024 data point** (same passage, same command):

1. Close DAL. Open the RME **Fireface USB Settings** dialog → *Buffer Size (Latency)* → **1024**.
2. Recreate the temp copy if `%TEMP%` was cleaned (copy `TSE_pt2_100tracks.dalproj`, `Audio\`,
   `InstrumentProxies\` from `C:\Users\nicla\Music\TSE_pt2_100tracks` to `%TEMP%\dal-perf-100tracks`).
3. Run `build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe --stability-perf-profile
   "%TEMP%\dal-perf-100tracks\TSE_pt2_100tracks.dalproj" --seconds 45 --warmup 5`;
   the result is appended to `%APPDATA%\MiniDAWLab\stability-run.log` (`engine:` and
   `profile:` lines). Set the RME buffer back afterwards.

The interesting number there is whether the per-block plug-in cost doubles with the block
(pure per-sample DSP → 1024 overruns just as badly, ≈ 27 ms per 21.3 ms budget) or grows less
(per-call overhead inside the plug-ins → 1024 fits, which is what "nästan rent, enstaka knäpp"
suggests). Only that run can decide it.

## 3. Where the time goes (run 1; run 4 within 1 %)

Per 512-sample block, mean / worst, share of the callback. Categories are exclusive timings
around the plug-in / proxy calls themselves; the remainder is the callback total minus those
calls. Phases are inclusive wall sections and are not added to categories.

| Cost | Per block mean | Worst block | Share of callback | Share of the 10.667 ms budget |
|---|---|---|---|---|
| Instrument `processBlock` (48 calls/block) | **13.77 ms** | 18.60 ms | 93.0 % | **129 %** |
| Insert `processBlock` (32 calls/block) | 0.47 ms | 1.71 ms | 3.2 % | 4.4 % |
| Proxy fetch/mix | 0 (no proxy selected) | — | 0 | 0 |
| DAL remainder (everything else) | 0.57 ms | 1.41 ms | 3.8 % | 5.3 % |
| **Callback total** | **14.80 ms** | 19.85 ms | 100 % | **139 %** |

The single worst block (19.85 ms) was 18.60 ms instrument plug-ins + 0.56 ms inserts + 0.69 ms
remainder — the same proportions, not a spike somewhere else.

Where the remainder lives (inclusive phases, mean per block): instrument-mix section outside the
plug-in calls ≈ 0.45 ms (per-host MIDI buffer merge, scratch clears, fader/pan/strip, meter fold,
fan-out — ≈ 9 µs per instrument row), transport MIDI scheduling 0.045 ms, clip render of the 36
(muted) audio rows 0.047 ms, host begin-block 0.016 ms, live MIDI 0.003 ms, bus finalize 0.002 ms.

**By plug-in (instances × mean cost per instance per block):**

| Plug-in | Instances processed | Per instance | Together | Share of callback |
|---|---|---|---|---|
| VB3-II (instrument) | 16 | 0.46 ms (max call 1.88 ms) | **7.36 ms** | 49.7 % |
| Groove Agent SE (instrument) | 16 | 0.32 ms | **5.05 ms** | 34.1 % |
| HALion Sonic (instrument) | 16 | 0.085 ms | 1.36 ms | 9.2 % |
| Pro-Q 3 (Post insert) | 16 | 0.018 ms | 0.29 ms | 1.9 % |
| DAL Mono Delay (Post insert) | 16 | 0.012 ms | 0.19 ms | 1.2 % |
| AmpliTube 4 (Pre insert) | 0 of 18 (rows muted) | — | 0 | 0 |

The five most expensive single instances are all VB3-II copies (tracks 96, 42, 36, 30, 48:
0.471 / 0.467 / 0.467 / 0.464 / 0.464 ms per block, 3.1–3.2 % of the callback each); the 16
VB3-II instances are within 3 % of each other, as are the GA SE and HALion groups — the load is
flat across copies, not one misbehaving instance.

## 4. Measured vs hypothesis

Measured (five 45 s windows, same passage, Release, 512 samples):

* The callback exceeds its budget in essentially every block; mean 138 % of budget, worst 186–201 %.
* 93 % of the callback is inside instrument `processBlock`; the plug-ins alone need 129 % of the
  budget — no reduction of DAL's own work can make 512 fit on one thread.
* DAL's own work is ≈ 0.6 ms per block (5 % of budget); inserts 0.47 ms.
* No proxy plays; no clipping; no paging; the mixer window does not affect the audio thread.
* The profiler's own cost is not visible above run-to-run spread.

Hypotheses (not measured here):

* The 1024 behaviour ("nästan rent") would mean the plug-ins' per-block cost is partly per-call
  rather than per-sample. Needs the RME 1024 run (§2.1).
* Run-to-run spread (13.3–14.8 ms mean, min 9.2–11.6 ms) comes from CPU clock / hybrid-core
  placement of the ASIO thread. Not instrumented (processor id per block was deliberately not
  added).
* Per-block `juce::MidiBuffer` merges in each host allocate on the audio thread when the block
  carries MIDI; measured only as part of the ≈ 0.45 ms instrument-mix remainder, individually
  not isolated.

## 5. Recommendation (priority order, with the numbers behind it)

1. **Parallel instrument processing** is the only measure that can bring this project under
   budget at 512 while the instruments play live: 13.7 ms of mutually independent plug-in work
   per block across 8 P-cores is ≈ 1.7–2.5 ms plus synchronisation, against a 10.7 ms budget. The
   instrument rows are independent until their post-strip stages are summed (each already
   renders into its own stage buffer in `mixKeyedInstrumentLanesIntoOutputsIfAny`), so the
   summing order — and therefore the output — can stay deterministic. This is an engine /
   threading-model change (ARCHITECTURE_PRINCIPLES realtime rules, a preallocated realtime worker
   pool, hosts' per-block state confined to their own worker, meters and MIDI delivery audited for
   cross-thread assumptions) and needs its own steering decision before implementation.
2. **Use Current proxies while the Primary is available** (policy change to PI-021 "Primary live
   when available") would remove the live cost of every proxy-backed destination — 0.46 ms per
   VB3-II / 0.32 ms per GA SE replaced by a ~µs ring copy — without touching threading. It only
   helps rows whose generation is Current (by the fingerprint rules the VB3-II copies are Stale
   because their MIDI-source track ids differ, while the GA SE copies have the same inputs as the
   original — neither was evaluated at runtime since the Primary won), and it changes a locked
   product rule, so it is a steering question, not a quick fix. Also a per-row "freeze" is the same
   mechanism under a different name.
3. **DAL overhead** (0.6 ms = 5 % of budget) is not the bottleneck. The per-host MIDI merge
   allocation and the strip bookkeeping are worth a small cleanup later, but even removing all of
   it leaves 13.7 ms of plug-in time on one thread.
4. Until 1 or 2 exists: at 512 this project cannot play cleanly on one thread on this machine;
   1024 (per the user's report) is the usable setting and the §2.1 run should confirm by how much
   margin. Fewer live instances (the test project triples each instrument 16×) is the only
   project-side lever.

Stop point: diagnosis and recommendation only. No optimisation, no parallelisation, no other
backlog item was started.

## Appendix — what was added to the code (off by default)

* `src/diagnostics/AudioThreadProfiler.h` (header-only): bounded (512 instances), preallocated,
  relaxed-atomic stats; enabled only through `setEnabled(true)` from the perf scenario. Call
  sites pay one relaxed load when off: `PlaybackEngine` (block begin / end, six phases),
  `ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs` (around
  `inst.processBlock` and the proxy mix), `PluginInsertHost::audioThread_processChainForTrack`
  (around each insert `processBlock`). Instances are named on the message thread when the
  playback snapshot / insert map is published (`InstrumentRuntimeCoordinator`,
  `PluginInsertHost::rebuildAudioThreadMapAndPublish`).
* `--stability-perf-profile` (`StabilityScenarioRunner`, `MainAppWindow` perf hooks): the
  procedure in §1. Nothing is saved; the device-setup request can dirty the session, hence the
  temp copy.
* Checked: Release and Debug `MiniDAWLab` build; `ExportLevelFocusedTests` (47 checks) and
  `PluginInsertTempoFocusedTests` (which compile the engine / insert host with the profiler) pass;
  with the profiler off the engine load window is unchanged (runs 2 / 5 vs 1 / 4); output peak and
  Stereo Out RMS are the same across profiler on / off runs (peak 0.372 in every run).
