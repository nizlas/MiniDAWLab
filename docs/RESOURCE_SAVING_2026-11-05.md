# Resource saving: proxy opt-in, Off unloading and automatic DSP rest (2026-11-05)

Background: `docs/CPU_MEMORY_TSE_PT2_2026-11-04.md` (a tester's 4-core / 8 GB laptop sat near 50 %
CPU while stopped and the project took about 5 GB). This document is the design contract and the
result report for branch `claude/resource-saving`. Evidence extracts:
`docs/evidence/resource-saving-2026-11-05/`.

**Short answer.**

* **Off now frees memory and CPU.** With every source row Off, the same project runs at
  0.08–0.10 core and 0.39 GB private (fresh process opening the saved Off copy) instead of
  0.7 core and 5.0 GB. No plug-in instance of an Off row is alive.
* **On rows rest automatically where it is provably safe.** Measured on the same copy and device:
  the stopped callback went from 3.2 ms to 0.75 ms before the first Play and to 1.5 ms after
  Play → Stop; process CPU from 0.69 to 0.40–0.49 core. With one render worker (what a 4-core
  machine gets) the stopped callback went from 3.5 ms to 1.0–1.8 ms and playback from 1.65 ms to
  0.93 ms.
* **Not solved:** stages whose plug-ins report an infinite or no tail (Pro-R reports infinite,
  AmpliTube 4 and the instruments report 0) keep running after they have had input, by design.
  The message thread's idle cost is explained but not reduced (section 6).
* **Blocker for release: AmpliTube 4 can crash DAL when one of its instances is destroyed**
  (section 3.4). This is the long-known `AmpliTube 4.vpa+0x7675E` teardown crash; Off now hits
  it during normal use, not only at shutdown. A decision is needed.

## 1. Proxy rendering is opt-in

* A missing or unknown `proxyUpdateMode` reads as **Manual**. New instrument tracks start
  Manual. The writer stores every mode explicitly, Auto included.
* Explicit `auto`, `onSave`, `manual` and `off` keep their meaning. An older file that omitted
  an Auto choice now reads as Manual (approved migration; the two cannot be told apart).
* Manual: an existing current proxy still plays by the unchanged source priority; Render now
  works when the Primary is loaded; nothing renders at load, on a plug-in notification, on
  Stale or on a missing proxy.
* A track switched **Off** never queues or starts a render in any mode; a running job is
  cancelled when the track goes Off; Render now and Retry are disabled with the tooltip
  "Unavailable: this track is Off and its instrument is not loaded." Back On, a stale Auto
  destination waits a full five-minute window again.
* Unchanged: Primary/proxy priority, fingerprint and Stale rules, DC/EOF policy. Prepare Portable
  Project renders through the scheduler directly and needs a loaded Primary; an Off row is
  therefore reported there as not renderable.
* Steering: `PORTABLE_INSTRUMENTS_AND_PROXIES.md` §18.1 (revision 6).

## 2. Three runtime states

| State | Meaning | Plug-in instances | Work per block |
|---|---|---|---|
| **Off** | The user switched the row off with Power (`Track::trackOff_`, persisted). | None. Inserts are dormant placeholders, the instrument is unloaded; all state is kept as data. | None for the row. MIDI addressed to it is discarded. |
| **On, resting** | Internal. A stage has provably nothing to produce. | Loaded, prepared. | That stage's `processBlock` is skipped; its silent buffer passes through. A resting instrument gets no render-pool job. |
| **On, working** | Input, MIDI, held notes, a tail still ringing, or a plug-in that cannot be judged. | Loaded. | As before. |

Resting has no control and no UI. Mute keeps its meaning (the row still processes, gain 0) and is
not an alias for Off.

## 3. Off

### 3.1 Ownership

The project model owns everything that describes the row: clips, notes, routing, fader, sends,
slot order, host Active / Bypass and every plug-in state blob. Runtime owns only instances.

* **Inserts** (`PluginInsertHost`): each slot of an Off row is a **dormant placeholder** — the
  existing unavailable-placeholder shape (identity, opaque state, Active/Bypass, slot id, stage),
  flagged `dormant`, listed as "<name> (Off)". `exportChain` re-emits it byte for byte, so Save,
  Save As, autosave, undo and Duplicate keep the state captured at Off. `importChain` builds
  dormant placeholders for an Off row (`setTrackDormancyQuery`) and never instantiates them.
* **Instrument** (`InstrumentTrackController` + `ExperimentalInstrumentHost`): the host stays
  registered (lanes, MIDI editor and proxy status keep working) and holds no plug-in. The
  controller keeps a **retained Primary row** with the plug-in fields of the project block
  (kind, path, descriptor, version, state blob, loaded-on-save, proxy save pairing). Every block
  built while the host is empty carries those fields. This also closes an older gap: Groove Agent
  and HALion had no saved-state fallback when their plug-in was not loaded.
* `TrackPowerCoordinator` (message thread) makes the runtime follow the snapshot's Off flag.

### 3.2 Transitions

**On → Off.** The header, mixer and instrument rows write the session Off flag as before (refused
while playing, recording, counting in or loading). From the next block the engine skips the row.
On its next tick (≤ 250 ms) the coordinator unloads the row, one row per message-loop turn:

1. Inserts: close editors, take the instances out of the chain, publish the map without them,
   then under acknowledged exclusive access (callback drained, read-ahead paused and acknowledged)
   capture each state and release it. If exclusive access is not acknowledged, nothing is
   captured or destroyed: the instances go back into the chain and the unload is retried later.
   A timeout is never treated as proof that the owner has left.
2. Instrument: after the callback has been observed outside its processing section, capture the
   state into the retained row; if the capture is empty the instrument stays loaded (its state
   only lives in the plug-in). Then `unloadInstrumentAfterAcknowledgedDrain`, which refuses while
   the owner is still referenced. A Secondary instance is retired after its blob was refreshed.
3. Released insert instances are destroyed one per timer tick, never in the turn that released
   them. Project close takes any that are still waiting into its own paced close.
4. Proxy: a running job is cancelled (section 1); the proxy coordinator treats an Off destination
   as not playing (no reader, no Secondary), instead of as a missing Primary.

**Off → On.** `requestPowerOn` keeps the row Off, loads the instrument (through the same per-kind
project autoload) and then each dormant insert, each on its own message-loop turn with a progress
toast ("Loading <track>: insert 2 of 5"), and only then switches the row On. The callback never
runs a half-built chain because an Off row is not processed. A plug-in that fails stays preserved
(an ordinary unavailable placeholder, or the retained row), the row comes On, and a message names
what failed — the same outcome as opening a project on a machine without that plug-in. Each job
carries a token; a delete, a project change or a new request drops a stale job. If the transport
starts during loading, the final flip waits until it stops. Transport MIDI resumes through the
existing chase.

**Snapshot restores.** Off is part of the session snapshot, so undo/redo of another edit can flip
it without a Power click. The coordinator reconciles: a row restored On while its plug-ins are
unloaded is put back Off and woken like a Power On; a row restored Off is unloaded. The instrument
controller's Power mirror follows the session (`syncPowerMirrorFromSession`).

**Project load.** Rows saved Off restore their insert chains as dormant placeholders and their
instrument rows without the autoload step (`deferPendingAutoloadWhileOff`). Nothing is
instantiated, not even briefly.

### 3.3 Shared resources

A MIDI row that goes Off only stops its own events (its pending note-offs are still sent) and
never unloads its destination. An Off instrument is not woken by MIDI routed to it; that MIDI is
discarded. Nothing another On row uses is released. No plug-in instance is shared between rows.

### 3.4 AmpliTube 4 destruction crash (blocker)

All-sources-Off runs in the app crashed 4 times in 10 (forward order, 10 AmpliTube 4 instances
destroyed per run), always on the **message thread inside `AmpliTube 4.vpa+0x7675E`** (read of a
freed address) while an AmpliTube instance was being destroyed — the same module and offset as the
pre-existing teardown crash documented since 2026-09-26 at app shutdown and project switch.

| Probe | Result |
|---|---|
| Headless harness (`InsertPersistenceFocusedTests --probe-unload`): the project's 10 AmpliTube instances unloaded and reloaded, back to back, paced, after worker-thread processing, during concurrent processing | 0 crashes in 7 runs (70 destructions) |
| App, forward order | crashes at track 22 (last instance) and at track 19 (not last) |
| App, reverse order | 0 of 4 |
| App, destruction deferred to a later timer tick | 2 of 5 |

So the crash needs the GUI process (Direct2D/GPU context present) and is intermittent; DAL's
ordering (unpublish, drain, capture, release, then destroy on a later turn) does not avoid it, and
nothing in DAL can make a third-party destructor safe. Options for a decision:

1. Ship Off unloading as is (a crash risk of roughly 3–4 % per AmpliTube destruction on this
   machine, unknown on others).
2. Keep instances of plug-ins with a recorded destruction crash loaded-but-released while Off: DAL
   would learn this from its own crash breadcrumb (`track power: Off: destroy insert "<name>"`)
   rather than from a name list. Memory for those plug-ins is then not freed.
3. Keep Off unloading behind a setting until the vendor fixes the plug-in.

### 3.5 Off defects fixed on the way

* MIDI routed or auditioned into an Off instrument (MIDI To, live MIDI / Monitor, MIDI editor
  audition of a routed row) was appended to the host's block buffer every block. An Off host is
  never processed, so the buffer grew on the audio thread and played as one burst when the row
  came back On. It is now discarded each block (`audioThread_discardPendingMidi`).
* Read-ahead could adopt an Off audio row.
* An unloaded Primary looked like a missing Primary to the proxy coordinator, which then opened
  proxy readers and loaded Secondary instruments for Off rows (seen: 2 proxies and 3 Secondaries
  loaded after switching every row Off).

**Why Off "did not help" before.** The engine already skipped Off rows, so Off did remove their
DSP. But every plug-in stayed instantiated (memory unchanged), Off is refused while playing, and
the expensive Pro-R and AmpliTube instances that actually ran were on unmuted rows the user still
wanted to hear.

## 4. Automatic rest for On rows

### 4.1 What the wrapper tells us

JUCE 8.0.4 `VST3PluginInstance`: `getTailLengthSeconds()` returns the plug-in's `getTailSamples`
(re-read each call; `kInfiniteTail` → +inf, `kNoTail` → 0). `getLatencySamples()` is cached at
prepare. Input silence flags are always cleared for active buses and the output silence flags are
never read, so DAL cannot get the plug-in's own silence report. The project's plug-ins report:

| Plug-in | Reported tail | Latency | Treated as |
|---|---|---|---|
| FabFilter Pro-R | infinite | 0 | infinite: keeps running after input |
| AmpliTube 4 | 0 | 7 | unknown: keeps running after input |
| FabFilter Pro-Q 3 | 0.171 s | 0 | finite |
| DAL Mono Delay | 5.25 s | 0 | finite |
| DAL Tuner | 0 | 0 | unknown |
| VB3-II, HALion Sonic, Groove Agent SE | 0 | 0 | unknown |

`acceptsMidi()` is true for Pro-R, Pro-Q and AmpliTube, but DAL sends inserts no MIDI (the MIDI
scratch is cleared per slot), so it is not a reason to keep running.

### 4.2 The rule (`src/plugins/InsertChainRest.h`)

Unit: one insert stage (the Pre or the Post inserts of an audio, instrument, group or Stereo Out
row), and each instrument's generation stage. A unit skips `processBlock` for a block when its
input in that block is **digitally silent** (every sample 0; for an instrument: no MIDI and no
held note or sustain pedal, tracked from the MIDI actually delivered) and:

* it has had no input since its plug-ins were prepared and its output has been silent for
  latency + 2 blocks (the loaded-never-played case), or
* every active plug-in reports a finite tail (0 < tail ≤ 60 s), none has been heard after its
  reported tail ran out, and input and output have both been silent for **2 × tail + latency +
  1 block**. The second tail length is a check: a plug-in heard after its reported tail is
  marked unreliable and its unit never rests again in this session.

An infinite or unreported tail, a plug-in that calls itself an instrument, or an open editor keeps
the unit running. Its uncertainty stays local; other units still rest. No time constant is
involved except the plug-ins' own tails.

**Wake.** Any input wakes the unit in the same block, before processing, so the first sample and
the first note are processed (verified bit-identical to no rest). A chain change, Active/Bypass,
a new instance or a changed reported tail wakes it too (re-read about once a second). Resting does
not call `reset()`: a unit only rests after its output has already been silent, so no old echo can
come back. Plug-in transport info keeps being set every block.

**Why digital silence.** Skipping a block whose input and recent output are exactly zero produces
exactly what processing it would have, so output does not depend on which lane decided (callback
block, render-pool job or read-ahead segment). Serial, pool and read-ahead output stay
bit-identical. A plug-in that keeps emitting a noise floor or DC (VB3-II) never rests.

**Scheduling.** A resting instrument with nothing queued gets no render-pool job. Read-ahead uses the
same chain function, so it follows the same rule on the rows it owns. An offline export suspends
rest (`PlaybackEngine::setPluginRestSuspended`, also used by the offline render gate) so an export
equals an uninterrupted render. Dispatching the remaining stopped tails through the render pool was
not done: after rest, the stopped callback is 1.5–1.8 ms, which does not justify it.

### 4.3 Known limits of rest

* A plug-in that under-reports a finite tail and stays silent for more than twice that tail before
  it sounds again would be cut. None of the project's plug-ins reports a finite tail except Pro-Q 3
  and DAL Mono Delay.
* A plug-in that generates sound on its own but stays silent for the first two blocks after prepare
  (and is not flagged as an instrument) could rest before it starts.
* Time-dependent internal state (an LFO phase) is frozen while resting; the output was digital
  silence at that time.

## 5. Measurements

Machine: Intel Core Ultra 9 275HX, 24 physical = 24 logical cores, 95 GB RAM, Windows 11. ASIO
Fireface USB (RME Babyface Pro), 48 kHz, 512 samples (10.667 ms budget). Project: a fresh temp copy
of `TSE_pt2_261031` (36 audio rows, 25 muted; 14 instrument rows; 58 insert instances). Before =
`main` 8472feb + yesterday's `--idle` diagnostics; after = this branch. `--stability-perf-profile
--seconds 20 --warmup 5 --idle --idle-gap 30`, profiler on, one process per run. Process CPU is
CPU time / wall time in core-equivalents. No run had a deadline overrun.

**A–C: rows On as saved (7 render workers, the default here)**

| Phase | Callback ms mean / max, before → after | Process CPU (cores), before → after |
|---|---|---|
| A. Loaded, before the first Play | 3.24 / 5.13 → **0.75 / 1.87** | 0.69 → **0.40** |
| C. Playing (30 s cycle, includes empty stretches) | 0.77 / 1.44 → **0.54 / 1.15** | 0.84 → **0.48** |
| B. Right after Stop | 3.19 / 5.62 → **1.54 / 2.52** | 0.69 → **0.49** |
| B. 30 s after Stop | 3.25 / 5.49 → **1.52 / 2.70** | 0.67 → **0.48** |

**Same with one render worker** (`--instrument-workers 1`, what `defaultWorkerCount` gives a
4-core machine; still this machine's cores — not a simulation of the laptop):

| Phase | Callback ms mean / max, before → after | Process CPU, before → after |
|---|---|---|
| A. Loaded, before the first Play | 3.46 / 5.86 → **0.98 / 1.55** | 0.67 → **0.41** |
| C. Playing | 1.65 / 2.84 → **0.93 / 1.77** | 0.73 → **0.47** |
| B. Right after Stop | 3.59 / 6.03 → **1.78 / 2.94** | 0.68 → **0.51** |
| B. 30 s after Stop | 3.43 / 5.71 → **1.74 / 2.75** | 0.64 → **0.47** |

Rest counters after the four windows (7 workers): insert stage blocks rested 116 906 / processed
72 591; instrument blocks rested 114 237 / processed 54 673. After Stop, 7 of 40 insert stages rest
(stages that never received input in the played range); the 11 Pro-R and the processing AmpliTube
keep running because of their tail reports.

**D–E: every source row Off** (audio, instrument and MIDI rows; there are no group buses, Stereo Out
has no inserts)

| | Callback ms mean | Process CPU | Private | Working set |
|---|---|---|---|---|
| Before, rows On (A) | 3.24 | 0.69 | 5.01 GB | 4.72 GB |
| D. Switched Off in the running process | 0.026 | 0.10 | 0.54 GB | 0.43 GB |
| E. New process opening the saved Off copy | 0.020 | 0.08–0.10 | 0.39 GB (load peak 0.42 GB) | 0.28 GB |
| Empty project (2026-11-04, installed 1.4.0) | — | 0.10 | 0.20 GB | — |

In D and E: 0 live insert instances, 0 loaded instrument Primaries, 0 Secondaries, 0 proxy readers.
D's 0.54 GB vs E's 0.39 GB is memory the plug-in modules and allocators keep after their instances
are destroyed. The remaining 0.2 GB over an empty project is the project's decoded audio (about 83 MB
as float), waveform caches and the 14 empty hosts and controllers.

**Threads** (after, stopped): ASIO callback 7 % of one core before the first Play, 14 % after Stop
(before: 28–30 %); message thread 19 % (unchanged); render workers about 1–2 % each.

## 6. Message thread

Sampled with a stack sampler (dbghelp) and split by user / kernel time while stopped: **9 % user +
11 % kernel** of one core. 90 % of user-mode samples wait in `NtUserGetMessage`. The active part is
Direct2D frame start / present per vblank (≈5 %), timers (≈2 %: transport, live MIDI, Inspector
channel strip, tooltip, playhead overlay, mixer), the scenario runner's own status label, and a
security product hook (`bdhkm64.dll`) in the message loop. Kernel time is the D2D/DXGI present path
and that hook. The meter and the playhead overlay only repaint on change. No unnecessary DAL repaint
or model refresh was confirmed, so nothing was changed. A finer attribution needs a profiler with
kernel stacks (ETW / WPA).

## 7. Open limits

* **AmpliTube 4 destruction crash** — section 3.4, needs a decision.
* **Decoded audio of Off-only rows is still kept in RAM.** Releasing and re-decoding clip material
  (and keeping waveform overviews) was not implemented; in this project it is under 83 MB.
* **Proxy pairing after Off → On:** reloading the Primary bumps its semantic revision, so a proxy
  that was Current can read Stale afterwards. With the new Manual default nothing renders by itself.
* **Off → On while playing:** Power stays refused while playing (unchanged). A load that finishes
  while the transport runs waits for Stop before the row comes On.
* **Not verified in the running app by hand:** MIDI editor on an Off row, Save / reopen while Off
  through the UI, undo of a Power change, listening across rest / wake. These are covered by the
  focused tests below only as far as stated there.
* **Conny's laptop:** nothing was measured there. Paging remains a hypothesis.

## 8. Tests

| Suite | Result | Covers |
|---|---|---|
| `MiniDAWSelftests` | 3 535 / 0 failures | missing mode → Manual, explicit modes written and kept, Manual/Off/Off-track never render, Render now refused and explained on an Off track; rest rule (never-played rest, wake in the first block, echo after a silent gap heard, 2 × tail, unknown / infinite keep running, unreliable tail, chain change, never-rest, self-sounding plug-in) |
| `InsertPersistenceFocusedTests` | 143 / 0 | real DAL Mono Delay + BassEvening: refused exclusive access leaves both instances and states untouched; Off unload; export equals live chain; Save while Off holds the captured states; reload with the row Off creates no instance; On restores exact state, order and host flags; impulse response identical; 8 Off/On cycles keep 2 instances |
| `InstrumentParallelFocusedTests` | 44 / 0 | serial vs pool bit identity with rest on; audition MIDI to an Off row discarded and not replayed at On; never-MIDI instruments rest after 2 blocks with no pool job; first notes after a rest bit-identical to no rest |
| `ExportLevelFocusedTests` | 76 / 0 | realtime vs offline identity with rest present |
| `InsertAudibleFocusedTests` | 69 / 0 | Active / Bypass with rest present |
| `ArrangementMidiEventFocusedTests` | 87 / 0 | unchanged |
| `AudioStripParallelFocusedTests` | 85 / 6 failures | the same 6 failures as an older `main` build (pre-existing); runs with rest suspended because it counts every block |
| `ReadAheadPrototypeFocusedTests` | 217 / 39 failures | the same 39 as an older `main` build (pre-existing); runs with rest suspended |

Rerun against the exact base `8472feb` (same toolchain and settings, two runs each): the same 6 and 39
failing checks at base and on this branch. See `OFF_UNLOADING_AND_AMPLITUBE_ISOLATION_PLAN_2026-11-05.md` A.4.

The Off commit was also built and tested on its own (without the rest commit):
`InsertPersistenceFocusedTests` 143 / 0, `MiniDAWSelftests` 3 524 / 0, `ExportLevelFocusedTests` 76 / 0.

Not run: the stability matrix, `--stability-load-loop`, `--stability-delete-loop`,
`--stability-mixdown` and the live-MIDI / proxy scenarios. They are recommended before a tester build
because this branch touches plug-in lifetime, the audio callback and project load.
