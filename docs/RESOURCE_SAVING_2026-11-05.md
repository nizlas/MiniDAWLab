# Resource saving: proxy opt-in, Off unloading and automatic DSP rest (2026-11-05)

Background: `docs/CPU_MEMORY_TSE_PT2_2026-11-04.md` (a tester's 4-core / 8 GB laptop sat near 50 %
CPU while stopped and the project took about 5 GB). This document is the design contract and the
result report for branch `claude/resource-saving`. Evidence extracts:
`docs/evidence/resource-saving-2026-11-05/`.

**Short answer.**

* **Off now frees memory and CPU.** With every source row Off, the same project runs at
  0.08–0.10 core and 0.39 GB private (fresh process opening the saved Off copy) instead of
  0.7 core and 5.0 GB. No plug-in instance of an Off row is alive.
* **On rows rest automatically, but only plug-ins that declare themselves audio-processing
  effects** (revision 2 of the rule, section 4.2). Silence so far never proves that a plug-in
  cannot sound later, so instruments, generators, plain "Fx" and tool plug-ins keep running. The
  numbers below were measured with the first, more permissive rule, under which instruments that
  had not received MIDI also rested: the stopped callback went from 3.2 ms to 0.75 ms before the
  first Play and to 1.5 ms after Play → Stop; process CPU from 0.69 to 0.40–0.49 core. With
  revision 2, re-measured after stage 0 against the exact base (section 10): stopped callback
  3.2 → 1.0 ms before the first Play and 3.3 → 1.8 ms after Stop; process CPU 0.71 → 0.51 core
  before Play, 0.76 → 0.63 right after Stop, 0.66 → 0.56 thirty seconds later. Category-based
  rest is a compatibility heuristic, not proof that a plug-in can never sound by itself.
* **Not solved:** stages whose plug-ins report an infinite or no tail (Pro-R reports infinite,
  AmpliTube 4 reports 0) keep running after they have had input, by design. The message thread's
  idle cost (19 % of a core) is **observed but not explained** (section 6).
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
| **On, resting** | Internal. A stage of declared audio-processing effects has silent input and nothing left to produce by its own report. | Loaded, prepared. | That stage's `processBlock` is skipped; its silent buffer passes through. A resting instrument gets no render-pool job. |
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

### 4.2 The rule (`src/plugins/InsertChainRest.h`, revision 2)

**Who may rest at all.** Neither condition below proves that a plug-in cannot sound later: a
transport-driven or free-running generator can stay silent for any number of blocks and then
start without input, MIDI or a parameter change. So a unit may rest only when every active
plug-in in it declares itself an audio-processing effect (`categoryAllowsRest`): its VST3
sub-categories contain "Fx" and one of EQ, Dynamics, Reverb, Delay, Distortion, Filter,
Modulation, Pitch Shift, Restoration, Mastering, Spatial or Analyzer, and none of Instrument,
Synth, Sampler, Drum, Piano, Generator, External, Tools, Network or OnlyARA. That is the
plug-in's own declaration, not something DAL verifies. In this project that covers Pro-Q 3
(EQ), Pro-R (Reverb), AmpliTube 4 (Distortion) and Pro-C 2 (Dynamics). It excludes every
instrument (HALion Sonic, Groove Agent SE, VB3-II, BBC SO), DAL Mono Delay, DAL Tuner and
BassEvening (plain "Fx") and VB3-II Remote Input ("Fx|Tools").

Revision 1 (commit 2804928) let any unit rest after two silent blocks if it had never had input,
instruments included. That was wrong as a safety claim and is replaced.

Unit: one insert stage (the Pre or the Post inserts of an audio, instrument, group or Stereo Out
row). An instrument's generation stage never rests under revision 2. A unit that may rest skips
`processBlock` for a block when its input in that block is **digitally silent** (every sample 0)
and:

* it has had no input since its plug-ins were prepared and its output has been silent for
  latency + 2 blocks (the loaded-never-played case), or
* every active plug-in reports a finite tail (0 < tail ≤ 60 s), none has been heard after its
  reported tail ran out, and input and output have both been silent for **2 × tail + latency +
  1 block**. The second tail length is a check: a plug-in heard after its reported tail is
  marked unreliable and its unit never rests again in this session.

An infinite or unreported tail, a category that does not allow rest, or an open editor keeps
the unit running. Its uncertainty stays local; other units still rest. No time constant is
involved except the plug-ins' own tails.

**Wake.** Any input wakes the unit in the same block, before processing, so the first sample is
processed (verified bit-identical to no rest). These also wake it and make it re-qualify from the
start:

* a parameter change from any source and thread, editor or not (`AudioProcessorListener` on every
  live insert instance);
* a state or preset load into the same instance (in-place undo, preset apply, slot restore);
* the transport starting or stopping, and a jump in play position (locate, cycle wrap);
* a chain change, Active/Bypass, a new instance or a changed reported tail (re-read about once a
  second).

A parameter or state wake keeps a tail already found unreliable marked unreliable. Resting does
not call `reset()`: a unit only rests after its output has already been silent, so no old echo
can come back. Plug-in transport info keeps being set every block. A plug-in that reports
parameter changes continuously (for example a meter exposed as a parameter) therefore never
rests; that costs CPU, not correctness.

**Why digital silence.** Skipping a block whose input and recent output are exactly zero produces
exactly what processing it would have, so output does not depend on which lane decided (callback
block, render-pool job or read-ahead segment). Serial, pool and read-ahead output stay
bit-identical. A plug-in that keeps emitting a noise floor or DC (VB3-II) never rests.

**Scheduling.** A resting instrument with nothing queued would get no render-pool job; under
revision 2 instruments do not rest, so every instrument keeps its job. Read-ahead uses the
same chain function, so it follows the same rule on the rows it owns. An offline export suspends
rest (`PlaybackEngine::setPluginRestSuspended`, also used by the offline render gate) so an export
equals an uninterrupted render. Dispatching the remaining stopped tails through the render pool was
not done: after rest, the stopped callback is 1.5–1.8 ms, which does not justify it.

### 4.3 Known limits of rest

* The category is the plug-in's own declaration. A plug-in declared as, say, "Fx|EQ" that makes
  sound with silent input, later, and without any parameter, state or transport change, would
  not be heard until something wakes it. DAL cannot detect that.
* A plug-in that under-reports a finite tail and stays silent for more than twice that tail before
  it sounds again would be cut. In this project only Pro-Q 3 reports a finite tail among the
  plug-ins that may rest.
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

**These A–C numbers are for rule revision 1.** Under revision 2 the 114 237 rested instrument
blocks and the rested stages made only of DAL Mono Delay, DAL Tuner, BassEvening or VB3-II Remote
Input are processed again. The phase A and B gain is therefore smaller, and the one-worker case
loses most. It was not re-measured in this step, because that needs the ASIO device and a second
audio instance was not started.

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

**Observed** (user-mode stack sampler with dbghelp, and `GetThreadTimes`, while stopped): the
message thread used **9 % user + 11 % kernel** of one core. In the user-mode samples:

* 88.7 % are inside `NtUserGetMessage`, the thread waiting for a message. Frames of a security
  product (`bdhkm64.dll`) sit above `GetMessageW` on 90 % of samples.
* About 5 % are vblank-driven Direct2D painting (`onVBlank` → `handleDirect2DPaint` →
  `startFrame`), 1–2 % DXGI / D3D11, 2.9 % `Label::valueChanged`, 1.7 % timer callbacks, and
  under 1 % each Groove Agent SE, HALion Sonic, JUCE's VST3 parameter dispatcher and `atcuf64.dll`.

**Not established.** Waiting samples show where the thread sleeps, not what costs CPU, so the
security hook and `GetMessage` on those stacks do not explain the cost. The sampler saw no kernel
stacks, so nothing is known about what the 11 % kernel time is.

**Hypotheses, untested:** the kernel time may come from the Direct2D / DXGI present path, from
the security product, or from timer and message traffic; the 2.9 % `Label::valueChanged` is
probably the perf scenario's own status label. The test is an ETW capture with kernel stacks
(WPR / WPA), with and without the window minimised and without the status label. Nothing was
changed in DAL.

## 7. Open limits

* **AmpliTube 4 destruction crash** — section 3.4, needs a decision.
* **Decoded audio of Off-only rows is still kept in RAM.** Releasing and re-decoding clip material
  (and keeping waveform overviews) was not implemented; in this project it is under 83 MB.
* **Proxy pairing after Off → On:** reloading the Primary bumps its semantic revision, so a proxy
  that was Current can read Stale afterwards. With the new Manual default nothing renders by itself.
* **Power while playing:** Power stays refused while playing. Since stage 0 no plug-in is created
  or destroyed because of Off while the transport runs, and undo / redo that would change Power is
  refused while playing (section 9).
* **Not verified in the running app by hand:** MIDI editor on an Off row, Save / reopen and Power
  undo were checked in test fixtures without an audio device (section 9), not in the running app.
  Listening across rest and wake was not done.
* **Conny's laptop:** nothing was measured there. Paging remains a hypothesis.

## 8. Tests (at commit 2804928, rest revision 1; stage 0 results are in section 9)

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
Stage 0 (Power guards, rest revision 2) and its tests: section 9.

The Off commit was also built and tested on its own (without the rest commit):
`InsertPersistenceFocusedTests` 143 / 0, `MiniDAWSelftests` 3 524 / 0, `ExportLevelFocusedTests` 76 / 0.

Not run: the stability matrix, `--stability-load-loop`, `--stability-delete-loop`,
`--stability-mixdown` and the live-MIDI / proxy scenarios. They are recommended before a tester build
because this branch touches plug-in lifetime, the audio callback and project load.

## 9. Stage 0 (2026-11-05, after review)

Plan: `OFF_UNLOADING_AND_AMPLITUBE_ISOLATION_PLAN_2026-11-05.md` (A.2, A.3).

**Power only while stopped.**

* Undo / redo first looks at the snapshot it would restore. While playing, a step that would switch
  any row On or Off is refused: the history pointer and the model stay unchanged and a toast says
  "Undo would switch a track On or Off. Stop the transport first." Steps that leave Power alone
  work during playback as before.
* `TrackPowerCoordinator` creates and destroys nothing while the transport is busy (playing,
  recording, counting in, project load): reconciling, every wake step and the deferred destruction
  of released instances wait for Stop. A stepwise On load that is between two plug-ins when the
  transport starts pauses there; the row stays Off until all its plug-ins are loaded after Stop.
  One load already in progress when the transport starts finishes.

**Rest revision 2** (section 4.2): only declared audio-processing effects may rest; instruments
never rest; parameter changes (any thread, no editor needed), state and preset loads, transport
start / stop and play-position jumps wake a resting stage.

Which rest cases are safe, and on what basis:

| Case | Basis | Safe? |
|---|---|---|
| Declared processing effect, never had input, silent output for latency + 2 blocks | the plug-in's category says it turns input into output | as safe as that declaration; anything it would do on a parameter, state or transport change wakes it |
| Declared processing effect after input, finite reported tail, 2 × tail of silence | the plug-in's tail report, checked once more | as safe as that report; a late sound marks it unreliable for the session |
| Zero-reported or infinite tail after input (AmpliTube 4, Pro-R) | none | does not rest |
| Instruments, generators, plain "Fx", "Fx\|Tools" | silence proves nothing about future output | do not rest |

**Tests added** (all pass):

| Suite | Checks |
|---|---|
| `MiniDAWSelftests` (3 543 / 0) | category policy table; a generator silent for 40 blocks keeps running and is heard from its first sounding block; a parameter change and a state load each wake a resting stage, the new sound is heard in the next block; transport start, stop and a position jump wake, contiguous playback lets it rest; a long tail with three echoes and silent gaps keeps every echo and rests only after 2 × tail |
| `InsertAudibleFocusedTests` (86 / 0) | through the real insert host: late-starting generators declared "Fx\|Generator", "Instrument\|Synth", "Fx" and "Fx\|Reverb\|Generator" keep running and are heard at block 20; an "Fx\|EQ" effect rests after 2 blocks and processes the first input block; a parameter change without an editor wakes it; an in-place state load that turns sound on wakes it; Play wakes it in the first playing block |
| `InstrumentParallelFocusedTests` (45 / 0) | instruments without MIDI keep processing every block and keep their pool jobs; an instrument that starts by itself after 10 silent blocks is heard, output identical to rest suspended |
| `SoloFocusedTests` (111 / 0) | real `UndoRedoCoordinator`: undo without a Power change works while playing; undo that would switch a row On, and redo that would switch it Off, are refused while playing with history and model unchanged; both apply after Stop |
| `InsertPersistenceFocusedTests` (153 / 0) | real DAL Mono Delay + BassEvening with `TrackPowerCoordinator`: no unload while busy, unload after Stop, no destruction while busy, Power On refused while busy, a stepwise load pauses between two plug-ins while busy with the row Off, completes after Stop with the states captured at Off; Save while Off and reopen (existing) |
| `MidiEditorCohortFocusedTests` (ok) | real MIDI editor window without an audio device on an Off row holding no instrument: the clip opens and its note is shown, a note moves, audition is silent and loads nothing, undo and redo of the note edit work, the row stays Off |

Unchanged and passing: `ExportLevelFocusedTests` 76 / 0, `ArrangementMidiEventFocusedTests` 87 / 0.
Timing-sensitive suites, two runs each on this build, compared with the exact base `8472feb` runs
of A.4: `ReadAheadPrototypeFocusedTests` 217 / 39 failures, the same 39 checks as the base.
`AudioStripParallelFocusedTests` 85 / 4 failures: the 4 sample-coverage failures of the base remain,
and the base's two pool-dispatch failures ("parallel: blocks were dispatched to the pool", "worker
threads ran jobs") passed in both runs. Its own counters show why the outcome moved: the base and
the earlier PR head dispatched 0 of 49 blocks to the pool in all four runs, this build 1 and 4.
That check depends on a measured block cost against the pool's threshold. Why the measured cost
changed is not established, so it is not counted as a fix. No check failed that passed at the
base.

Found while testing: a default DAL Mono Delay with tempo sync on writes its parameters in a
different order and a recomputed delay time (333.300049 vs 333.300018 ms) after a reload, so its
state bytes after Off → On differ from those captured at Off. That is the plug-in re-serialising,
not DAL losing state; the test uses explicit non-synced states, as the existing Off test does.

**Not done in stage 0:** CPU after rest revision 2 was not re-measured in that step; it was
measured afterwards (section 10). The checks above ran in test fixtures, not in the running app.

## 10. CPU re-measurement after stage 0 (2026-11-05, 21:26–21:34)

**Builds.** Stage 0: `claude/resource-saving` build revision `dc8d9db` (branch head `6601175`
only adds documents; `git diff dc8d9db 6601175 -- src CMakeLists.txt` is empty), exe linked
20:21 after that commit. Baseline: the exact base `8472feb`, built for this run in a separate
worktree. Neither build was changed.

**Conditions**, logged by each run: ASIO "ASIO Fireface USB", 48 000 Hz, 512 samples (10.667 ms
budget), in/out latency 535/552 samples; 7 render workers (the default here); Core Ultra 9 275HX,
24 cores. Audio device settings, the original project and the user configuration were left as
they were. No DAL or Cubase was running before each run; the runs were sequential, one process at
a time. Each run opened its own file in a fresh temp copy of `TSE_pt2_261031`
(`%TEMP%\dal-s0-cpu`). Command per run: `--stability-perf-profile <copy> --seconds 20 --warmup 5
--idle --idle-gap 30`, profiler on, the same path and passage as section 5 (start at 0.00 s,
cycle on, 20 s measured after 5 s warm-up). One run per build, so differences of a few hundredths
of a core are within run-to-run noise. Log extracts: `docs/evidence/resource-saving-2026-11-05/s0cpu-*`.

**Rows On as saved** (36 audio rows, 34 muted, 1 saved Off; 14 instruments; 58 insert instances):

| Window | Callback ms mean / max, base → stage 0 | Process CPU, cores (Windows % of 24 cores), base → stage 0 |
|---|---|---|
| 1. Loaded, before the first Play | 3.24 / 6.01 → **1.01 / 1.79** | 0.71 (2.9 %) → **0.51 (2.1 %)** |
| 2. Playing, same passage as section 5 | 0.93 / 1.41 → **0.80 / 1.27** | 0.92 (3.8 %) → **0.61 (2.5 %)** |
| 3a. 0–20 s after Stop | 3.29 / 6.06 → **1.82 / 3.51** | 0.76 (3.2 %) → **0.63 (2.6 %)** |
| 3b. 30–50 s after Stop | 3.17 / 6.47 → **1.71 / 3.38** | 0.66 (2.8 %) → **0.56 (2.3 %)** |

* No overrun and no block above 70 % of the budget in any window of either build.
* Memory, stopped: base 5.01 GB private / 4.72 GB working set; stage 0 4.99 GB / 4.70 GB. Rest
  does not free memory.
* Processed versus resting: base calls 30 of 72 registered instances (insert slots and
  instruments) in every window. Stage 0 calls 19 before the first Play and 23 during and after
  playback; 11 of 40 insert stages rest before the first Play, 7 after Stop. Instrument blocks
  rested: 0 (instruments never rest under revision 2), against 114 237 under revision 1.
* Insert stage blocks after Stop: 13 439 rested / 15 391 processed in window 3a, and 33 040 /
  37 760 from the end of 3a to the end of 3b (gap plus window). About 47 % of stage blocks rest in
  both, so nothing more came to rest during the 30 s gap.
* Proxy: 0 destinations selected, 0 underruns; no proxy render job was logged (proxy rendering is
  opt-in and the project's rows read as Manual).
* Tails are not over at 3a: the master output peaked at 0.06 in the first window after Stop and
  0.000 in the window 30 s later, in both builds. Stage 0 keeps every stage that had input and
  reports a 0 or infinite tail running, so 3b is not lower because tails ended.
* The playing window peaked at 0.612 at the master in both builds (the same passage).

**What costs most after Stop in stage 0** (summed plug-in time per block, window 3a): VB3-II ×3
1.32 ms, Pro-R ×5 1.03 ms, HALion Sonic ×9 0.25 ms, Groove Agent SE ×2 0.19 ms; Pro-Q 3 and DAL
Mono Delay below 0.03 ms each. In the base the same window has Pro-R ×11 at 2.22 ms and AmpliTube
4 at 0.28 ms. So the remaining stopped cost is the instruments (which never rest) and the five
Pro-R that had input (infinite tail). Per thread, 8 s sample after the gap: ASIO callback thread
16 % of a core (base 28 %), message thread 21 % (base 21 %).

**4. Every source row saved Off, new process** (`TSE_pt2-alloff.dalproj`). The fixture was made
without running DAL: a copy of the project file with `"off": true` added to the 36 audio, 14
instrument and 1 MIDI rows and nothing else changed (checked by comparing the parsed JSON with
that key removed). So no AmpliTube instance was created or destroyed to make it. Buses: there are
no group buses; Stereo Out stays On and has no inserts.

| Window | Callback ms mean / max | Process CPU, cores (Windows %) |
|---|---|---|
| 1. Before the first Play | 0.022 / 0.080 | 0.09 (0.4 %) |
| 2. Playing | 0.035 / 0.107 | 0.09 (0.4 %) |
| 3a. Right after Stop | 0.023 / 0.138 | 0.09 (0.4 %) |
| 3b. 30 s after Stop | 0.020 / 0.132 | 0.07 (0.3 %) |

Private 0.39 GB, working set 0.28 GB in every window (load peak: working set 0.31 GB, paged
0.42 GB); 0 insert instances, 0 loaded instrument Primaries (14 empty hosts), 0 proxy readers,
108 threads (301 with rows On). Message thread 4 % of a core here, against 21 % with rows On:
that is an observation about the loaded plug-ins' presence, not a measured cause (section 6
stands). Not run: a base comparison for this case (the base loads every plug-in of an Off row,
so it equals the On baseline above) and switching Off in a running process (section 5 D), which
would destroy the loaded AmpliTube instances again.

**Revision 1 figures** in section 5 are history, not results for the current rule: before the
first Play revision 1 measured 0.75 ms and 0.40 core, after Stop 1.54 ms and 0.49 core; on a
different day's run of the same base.

**Category rest is a compatibility heuristic.** It trusts the plug-in's own VST3 category. A
plug-in that declares itself an EQ, reverb or distortion and still generates sound by itself
without input, a parameter, state or transport change would be cut off while resting. DAL does
not and cannot prove that this never happens.
