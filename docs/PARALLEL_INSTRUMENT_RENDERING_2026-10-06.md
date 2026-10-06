# Parallel live-instrument rendering — first bounded version (1.1.18, 2026-10-06)

Follow-up to [`docs/PERF_PROFILE_100TRACKS_2026-10-06.md`](PERF_PROFILE_100TRACKS_2026-10-06.md):
at 48 kHz / 512 samples the 100-track project's 48 live instruments took 93 % of a callback that
exceeded its 10.67 ms budget in every block. This slice distributes exactly that work — the
instruments' generation stage — over a fixed worker pool and leaves everything else as it was.

## Before / after at the actual buffer size (Release 1.1.18, ASIO Fireface USB, 48 kHz, 512 samples, mixer closed)

Same build, same temp copy of `TSE_pt2_100tracks.dalproj`, same passage (start 0, cycle on),
45 s windows after a 5 s warm-up, `--stability-perf-profile`. The driver offers only 512 (the RME
panel governs; a 1024 request is still answered with 512 — see the profile report §2.1).

| Run | Generation | Callback ms min / mean / max | Budget % mean / max | Blocks ≥ 100 % / 70–100 % | Real time | Instrument CPU summed / block | Generation section wall / block | Callback idle in join mean / max |
|---|---|---|---|---|---|---|---|---|
| 1 | **serial** (`--instrument-workers 0`, no worker threads) | 7.46 / **9.91** / 16.50 | 92.9 / 154.7 | **836** / 3 351 of 4 188 | 4 188 blocks in 45 s (≈ 99 %) | 9.11 ms | 9.38 ms | — |
| 3A | serial forced in-process (7 idle workers) | 7.61 / 9.94 / 15.65 | 93.2 / 146.7 | 953 / 3 244 of 4 197 | ≈ 99 % | 9.14 ms | 9.41 ms | — |
| 3B | **parallel, 7 workers + callback** (same process as 3A) | 1.53 / **2.06** / 3.31 | 19.3 / 31.0 | **0 / 0** of 4 222 | 100 % (4 222 blocks, start interval 10.67 ms) | 10.95 ms | 1.50 ms (max 2.67) | 0.06 / 0.84 ms |
| 2 | parallel, 7 workers (fresh process) | 1.58 / 2.25 / 4.06 | 21.1 / 38.0 | 0 / 0 of 4 225 | 100 % | 11.94 ms | 1.63 ms (max 3.44) | 0.06 / 1.55 ms |
| 5 | parallel, **3 workers** + callback | 2.35 / 3.25 / 7.81 | 30.5 / 73.2 | 0 / 1 of 4 223 | 100 % | 10.26 ms | 2.69 ms (max 7.17) | 0.03 / 0.37 ms |
| 4A/4B | original TSE project (3 live instruments), serial → parallel | 0.88 → 0.49 mean, 2.19 → 1.37 max | 8.3 → 4.6 | 0 / 0 both | 100 % both | 0.79 → 0.81 ms | 0.81 → 0.42 ms | — / 0.28 ms |

Reading the table:

* **Headline:** callback mean 9.9 ms → 2.1 ms, worst block 16.5 ms → 3.3 ms, overruns 836 (20 %
  of blocks, with another 80 % between 70 and 100 %) → **0**, and the device gets its blocks on
  time (start interval = 10.67 ms, playhead advances in real time). Effective parallelism 7.3 on
  8 threads; the callback thread spends on average 0.06 ms waiting for the last job.
* The instruments' **summed CPU** grows from 9.1 to 10.9–11.9 ms per block when they run on eight
  threads at once (lower clocks with more active cores, E-core placement, shared caches). That
  is why the section wall is 1.5 ms rather than 9.1 / 8.
* **3 workers** (4 threads) already removes every overrun on this project; the worst block is
  7.8 ms (73 %), so the margin is thin — 7 workers is the default on this machine.
* **Small project:** no regression; the generation section still halves (0.81 → 0.42 ms) and the
  callback mean goes 0.88 → 0.49 ms. The pool's small-workload rule keeps blocks with less than
  0.3 ms of summed instrument work serial without waking anyone.
* **Session-to-session variance:** yesterday's serial baseline on this project was 14.8 ms mean /
  19.8 ms max with 100 % overruns; today's serial runs measure ≈ 9.9 ms (every plug-in's per-call
  cost ≈ 35 % lower: VB3-II 0.29 ms vs 0.46 ms per instance). The engine's serial path is
  unchanged work, so this is the machine (clock / power state), not the slice — the before / after
  above is therefore taken inside one build and, for 3A/3B, inside one process.
* **Measured audio health vs listening:** all of the above is engine / profiler evidence (budget,
  overruns, start intervals, output peak 0.37 / no overs / no non-finite samples, serial-vs-parallel
  bit-identity in the focused test). Nobody listened to the output during these runs; a listening
  check on the 100-track project at 512 is the user's.

## What is parallel, what is still serial

Parallel (one job per host, inside the callback):

* the live instrument's **generation stage**: UI-MIDI + transport-MIDI merge, Secondary channel
  mapping, capture sink, `AudioPluginInstance::processBlock` into the host's **own** scratch,
  peak / diagnostics counters; transport host and (when the transport is stopped) the audition
  host are separate jobs on separate objects.

Serial on the callback thread, unchanged code and order:

* everything before dispatch: snapshots, transport, record-run handshake, host `beginAudioBlock`,
  live-MIDI dispatch, audio rows' clip rendering + their inserts, transport-MIDI scheduling for
  instrument rows and `Midi` source rows (so every source of a destination is merged before its
  single render), proxy segment notes;
* everything after the join: per instrument row the **add** step (gain / pan from the rendered
  scratch), Pre / Post **inserts**, fader / mute / pan, meters, routing fan-out and sends, Group /
  Master bus strips, device output — in the existing row order (bit-identical sum);
* **proxy-backed hosts** (their mix is a ring copy; the dispatcher leaves them to the row loop and
  their Primary is never touched), the sessionless fallback mix, the **offline mixdown** and
  **proxy rendering** (unchanged exclusion from realtime processing), the live-input monitoring
  pass.

## Design note: work split, buffer ownership, synchronisation

* **Split point.** `ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs` is now
  *render-if-needed + add*: `audioThread_renderGenerationStageForBlock(n)` produces the stage in
  `scratch_` (idempotent per block; `audioThread_beginAudioBlock` resets the flag) and the add step
  applies gain / pan. The pool calls the render step; the strip code is untouched and calls the
  same function, so the serial comparison path **is** the production path with the dispatch
  skipped — one musical logic, no second engine.
* **Buffer ownership.** Each host renders into its own preallocated `scratch_` (sized in
  `prepareForDevice` / load); the MIDI merge uses host-owned `rtMergedMidi_` / `rtRemappedMidi_`
  (capacity reserved, `clear()` keeps it — the per-block `juce::MidiBuffer` heap allocation of the
  old code is gone). The engine's job array (`std::array<RenderJob, 256>`) is an engine member
  written only by the callback thread before publication. Nothing shared is written by two jobs:
  the shared insert scratch, the post-strip stage scratch and the bus buffers are touched only in
  the serial row loop after the join.
* **Scheduling.** The callback writes the jobs (rows the loop will process: Instrument kind, not
  Off, entry with host; muted rows included because they keep processing at gain 0; audition hosts
  only when the strip would process them), sorts them **longest-first by the host's previous
  render duration**, publishes per-job claim flags, bumps a generation counter and wakes the
  workers (`std::atomic::notify_all` → `WakeByAddressAll`). Workers and the callback claim jobs
  with `exchange(1)` on the flag (a stale view of the job count is inert: unpublished indices stay
  claimed), run `audioThread_renderGenerationStageForBlock`, decrement `remaining`; the thread
  that reaches zero notifies. The callback spins at most 150 µs on `remaining`, then blocks in
  `remaining.wait()` (one kernel wait per block at most; in the 45 s runs 1 225–1 410 of ≈ 4 200
  blocks needed it). Idle workers block in `generation.wait()`; nothing spins between blocks.
* **Threads.** `std::thread` workers created once in `PlaybackEngine`'s constructor (message
  thread, device not running) and joined in its destructor (after `removeAudioCallback`, the
  existing tear order). Each worker joins MMCSS **"Pro Audio"** (`AvSetMmThreadCharacteristicsW`,
  loaded dynamically; thread-level only — the process priority class is never raised; JUCE's
  `startRealtimeThread` was avoided for exactly that reason) and falls back to
  `THREAD_PRIORITY_TIME_CRITICAL`. Default count `physical cores / 2 − 1`, capped at 7, 0 on
  ≤ 3-core machines; `--instrument-workers N` overrides (0 = serial), up to 15.
* **Deadline semantics.** A late job makes the block late exactly as a slow plug-in did before;
  the callback never continues past the join, never re-runs a started job, never sums a buffer that
  is still being written, and a host can never enter the next block while its previous render is
  running (the next callback cannot start before this one returns).
* **Lifetime.** Because jobs are confined to the callback, the existing publish-before-destroy
  drains (`waitForAudioCallbackExit`) cover plug-in removal, project switch, device stop / start,
  block-size / sample-rate changes (hosts are re-prepared from `audioDeviceAboutToStart`, no
  callback running) and shutdown; the offline-render gate still makes the callback return before
  the instrument section, so an export can never overlap a job on the same instance.
* **Profiler.** `AudioThreadProfiler` folds with integer-nanosecond `fetch_add` / CAS-max atomics
  (workers fold concurrently); the instrument category is **summed CPU across threads**, the new
  "generation section" fields carry the callback's wall time for that work and its idle wait, and
  the remainder subtracts only the callback thread's own plug-in time plus the section wall.

## Verification

| Check | How | Result |
|---|---|---|
| Serial and parallel produce the same result | `InstrumentParallelFocusedTests` (new, device-free): PRODUCTION engine callback + REAL `ExperimentalInstrumentHost` objects with a deterministic test instrument (host test seam `installInstrumentInstanceForTests`), 8 instrument rows incl. muted / Off / instrument-less / Group-routed / with a send, 48 blocks | **bit-identical** device output (max |diff| = 0, peak 0.67), 47 of 48 blocks parallel (the first has no duration estimate yet), 7 jobs per block |
| Exactly one `processBlock` per instance per block, never concurrent, Off never processed, muted processed | same test (instance-level guards: re-entrancy flag, per-block id, destructor / prepare flags) | 0 overlaps, 0 double-process, Off = 0 blocks, muted = 48 blocks |
| Small workload / 0-worker override stay serial | same test | 16 of 16 blocks serial below the threshold; `--instrument-workers 0` → 0 threads, every block serial |
| Offline mixdown unaffected and equal | same test: `renderOfflineMixdownBlock` under the gate vs the realtime parallel output | max |diff| = 0, pool never used offline |
| Publish-before-destroy, device stop / start, shutdown | same test: a device thread cycling callbacks every ~0.3 ms while the main thread swaps 52 hosts (publish without row → drain → destroy → new host → publish), forced-serial toggles, one device stop / start | 0 drain timeouts, 0 instances destroyed or re-prepared while processing, 0 overlaps |
| Several MIDI sources into one instrument, offline parity | `--stability-midi-routing` (Release, parallel default) on the TSE copy | PASS (11 note-ons / 11 offs on 3 channels delivered once; offline parity) |
| Monitoring + a short MIDI recording, device close / restart during a take, offline export with a live note | `--stability-live-midi` on the pre-gain fixture (policy fixture) | PASS. (On the TSE copy the scenario stops at a header-cell click precondition **before any audio step — identically with `--instrument-workers 0`**, i.e. unrelated to this slice.) |
| Project reload ×3, delete / undo / redo with playback and the MIDI editor, open / save / close | `--stability-smoke` on a fresh TSE copy | PASS |
| Offline export gate against realtime jobs | `--stability-mixdown` on a fresh TSE copy; `ExportLevelFocusedTests` (47 checks, engine realtime vs offline) | PASS / PASS |
| Cycle wraps, mute / Off, Group + send | cycle: the 100-track runs play the 41 s loop with wraps (4 222 real-time blocks, no overrun); mute / Off / Group / send: the focused test's fixture | covered as stated |
| Shutdown with 100 loaded plug-ins | every 100-track run (serial **and** parallel) ends in the known AmpliTube 4 teardown crash `AmpliTube 4.vpa+0x7675E` after "app shutdown begin" (dumps 22:54 / 22:56 / 23:02), or in the scenario watchdog's forced exit | pre-existing, independent of the pool (reproduced with 0 workers) |

Not verified: real listening; a machine with 2–4 physical cores (the default degrades to 0–1
workers there; the 0-worker path is the serial path tested above).

## Operating notes

* Serial override for comparison or fault isolation: start DAL with `--instrument-workers 0`;
  any other `N` (1–15) sets the pool size. No settings UI.
* `--stability-perf-profile … --generation ab` measures serial and parallel back to back in one
  process; `--generation serial` forces serial for the whole window.
* The remaining per-block cost on the 100-track project is now the summed plug-in work divided by
  the threads (≈ 1.5 ms wall) + 0.47 ms of serial inserts + 0.2 ms DAL work; the next bottleneck
  on bigger projects would be the serial insert chains and the heaviest single instrument (a
  block can never be shorter than its longest job: VB3-II ≈ 0.3–0.5 ms).
