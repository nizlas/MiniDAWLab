# Parallel audio processing and read-ahead pre-processing — architecture and implementation plan (2026-10-09)

> **Status: plan only.** Nothing in this document is implemented. It is the steering
> negotiation required by `docs/ARCHITECTURE_PRINCIPLES.md` ("Architecture is negotiated in
> documents before it is asserted in code") for the two backlog items in
> [`docs/PHASE_PLAN.md`](PHASE_PLAN.md):
> *Parallel processing of independent audio channels and whole insert chains* (Stage A) and
> *ASIO-Guard-like pre-processing for paths that need no live response* (Stage B).
>
> **Reviewed revision:** `f11c0fb` ("1.2.1: proxy stays Current after save/reopen", main).
> All file/line references below are against that revision. No production code, no new
> diagnostics and no broad test runs were part of producing this plan.

---

## Summary (plain language)

Today one audio-callback thread does almost all of the per-block work. Since 1.1.18 the
*live-instrument generation stage* (the instrument plug-ins' `processBlock`) runs in parallel
on a fixed worker pool inside the callback; everything else — audio-clip rows and their
Pre/Post insert chains, the instrument rows' insert chains, fader/pan, routing, Group and
Stereo Out buses — still runs serially. The latest measurement on the user's machine
(Release, ASIO, 48 kHz, 512 samples, 48 live instruments, 18 AmpliTube instances on unmuted
audio rows) shows instrument generation at ≈ 1.60 ms wall (already parallel) while the
**serial audio-row insert chains cost ≈ 4.04 ms mean / 9.44 ms max** per 10.67 ms block —
they are the next capacity limit.

- **Stage A (parallel strips)** extends the proven 1.1.18 pool model so that each *source
  row's whole channel strip* (clip read → pre-gain → Pre inserts → fader → Post inserts →
  pan) becomes one job, dispatched in the same single dispatch/join the instrument jobs
  already use. Summing stays serial and in row order, so output stays deterministic.
  Groups and Stereo Out stay serial in the first stage.
- **Stage B (read-ahead)** renders the strips of rows that need no live response *ahead of
  the device deadline* on a low-priority background thread, so the callback mostly consumes
  finished buffers and processes only the live paths (the user normally monitors exactly one
  track while recording — everything else is playback). Read-ahead must never add latency to
  the live/monitored path, must never process a plug-in instance twice or concurrently, and
  needs a strict ownership model for who processes each instance when.

The recommendation (§5) is to implement Stage A first, in two or three small slices, and to
treat two of its by-products — per-track insert scratch/playhead decomposition and an
explicit Mute/Off processing rule — as prerequisites for Stage B. Stage B then starts with a
deliberately minimal version (audio rows only, global invalidation, small fixed read-ahead).

---

## 1. Concepts

### 1.1 Parallelism *within* the deadline (Stage A)

The device gives the callback one block (512 samples ≈ 10.67 ms at 48 kHz) and a deadline.
Stage A keeps every sample being computed *in the same callback that outputs it*, but spreads
independent work across cores: the callback dispatches jobs, participates, joins, and then
sums. Latency is unchanged — the win is wall time per block. The hard limit is the *longest
single job*: a block can never finish faster than its heaviest insert chain or instrument
(this is already documented for the instrument pool in
[`docs/PARALLEL_INSTRUMENT_RENDERING_2026-10-06.md`](PARALLEL_INSTRUMENT_RENDERING_2026-10-06.md)).

### 1.2 Read-ahead pre-processing (Stage B)

Read-ahead computes blocks *before* the device asks for them, on worker threads with a
relaxed deadline, and the callback consumes finished buffers. This is the principle behind
Steinberg's **ASIO-Guard**: per Steinberg's published documentation, ASIO-Guard splits
processing into a real-time path and a pre-processing path with a larger effective buffer,
and channels that need live response (record-enabled/monitored channels) are excluded
from pre-processing so their latency is unaffected. We use ASIO-Guard **only as a reference
concept** — nothing in this plan claims knowledge of Cubase internals, and every mechanism
proposed below is our own design against DAL's actual code.

What read-ahead improves: work that merely has to *average* under the budget can be smoothed
over several blocks, so a single slow block (plug-in housekeeping spikes, OS jitter) no
longer causes an audible dropout, and total throughput can exceed "everything inside one
device period". What it costs: memory for the prepared buffers, a strict
invalidation/ownership protocol (every edit that affects a prepared block must invalidate
it), and **control latency** on pre-processed rows (a parameter change can take up to the
read-ahead depth to become audible unless the row is dropped back to live processing).

What it does **not** fix: a single too-heavy *live* chain. A monitored track with an
AmpliTube chain must still complete inside every single device block, because its input only
exists at the deadline. Read-ahead removes the *other* rows' load from the callback so the
live chain has the whole budget to itself — it cannot make the live chain itself cheaper.

### 1.3 Instrument proxies are a different mechanism

DAL's instrument proxies (steering: `docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md`) replace a
destination's *generation stage* with a **pre-rendered WAV asset on disk**, streamed by
`ProxyPlaybackReader` through a bounded ring filled by one shared I/O thread. That is
*persistent offline* material with its own identity/currency model (fingerprints, semantic
revisions). Stage B's read-ahead is *transient runtime* material: the same live plug-in
instances, the same project state, just computed milliseconds early and discarded on any
change. The two must not be conflated; in particular this plan does **not** change the
locked proxy source priority (PI-021, Primary-when-available). A user-selectable proxy
despite an available Primary is a separate future feature, out of scope here.

That said, the proxy playback machinery is valuable **prior art** for Stage B: the ring +
generation + "silence and count an underrun, never block" contract of
[`src/instruments/ProxyPlaybackReader.h`](../src/instruments/ProxyPlaybackReader.h) and the
prepared-loop-wrap model are exactly the realtime disciplines a read-ahead consumer needs.

---

## 2. The engine today (mapped against `f11c0fb`)

### 2.1 Callback skeleton

`PlaybackEngine::audioDeviceIOCallbackWithContext`
([`src/engine/PlaybackEngine.cpp`](../src/engine/PlaybackEngine.cpp) line 809, body to
≈ 2339) runs these phases in order (the `AudioCallbackPhase` enum in
[`src/engine/PlaybackEngine.h`](../src/engine/PlaybackEngine.h) is the authoritative list):

1. **Begin** — publish "callback in flight" (`audioCallbackInProcessingSection_`,
   Dekker-paired with the offline-render gate), advance the monotone device clock
   (`monoSampleClock_`), map packed active input channels.
2. **TransportBeginBlock / RecorderPush** — `Transport::audioThread_beginBlock` (consume
   pending seek), record-run handshake (`audioThread_updateRecordRun` stamps one boundary:
   mono sample, timeline sample, wrap serial), push the armed track's **raw pre-strip**
   input to the recorder's SPSC path.
3. **OfflineGateSilence** — if an offline export holds the gate, output silence and return.
4. **LoadSnapshot** — ONE acquire-load each of: `SessionSnapshot`,
   `ExperimentalInstrumentPlaybackSnapshot`, `LiveInputMonitorSnapshot`, `SoloMuteView`,
   `RoutingPlan`. The whole block sees one consistent picture of each.
5. **InstrumentBeginBlock** — `audioThread_beginAudioBlock(numSamples)` on every host
   (resets per-block render flags); then live-MIDI delivery
   (`LiveMidiInputBus::audioThread_dispatch` → `audioThread_addMidiEventForCurrentBlock`
   with sample offsets) and stop-edge note-off flushes.
6. **MixPrep / CountIn** — clear device outputs and all routing bus scratch slots; count-in
   click mixes into the master bus.
7. **ClipRender + TransportMidiSchedule** — the `renderRun` lambda (line ≈ 1943) runs once
   for linear playback, **twice on a cycle wrap** (segment `[t0, R)` then `[L, …)`),
   each time: refresh the shared insert transport context
   (`setInsertProcessContext(timelineStartAudible)`), render each `RoutingPlan::SourceStep`
   **Audio** row's full strip into the shared post-strip stage scratch and fan it to its dry
   bus + sends, then schedule transport MIDI into each Instrument row's host
   (`InstrumentTrackController::audioThread_scheduleTransportMidiForSegment`) and each
   `TrackKind::Midi` source's destination host, plus proxy segment/loop notes.
8. **InstrumentMix** (`mixKeyedInstrumentLanesIntoOutputsIfAny`, line ≈ 1462) — the 1.1.18
   parallel section: one `InstrumentRenderPool` job per live (non-proxy) instrument host
   renders the generation stage into the *host-owned* scratch
   (`ExperimentalInstrumentHost::audioThread_renderGenerationStageForBlock`); the callback
   participates, joins, then walks instrument rows **serially in session row order**
   applying inserts/fader/pan/meters and fanning to dry bus + sends.
9. **Monitor pass** (`renderLiveInputMonitoringPass`, line ≈ 1737, runs inside the finalize
   helper every callback, stopped or playing) — each monitored Audio track runs its full
   strip on the **live device input** (clip playback for that row is suppressed in step 7),
   then routes to the same dry bus + sends as its clip playback would.
10. **FinalizeRouting / FinalizeStagedBusLoop** — walk `RoutingPlan::busSteps` in
    topological order (child Groups before parents, Master last;
    [`src/engine/RoutingPlanBuilder.cpp`](../src/engine/RoutingPlanBuilder.cpp)): each
    Group/Master row applies its own strip to its summed bus scratch and forwards to its
    destination bus or the device outputs.

Transport state ([`src/transport/Transport.h`](../src/transport/Transport.h)) is atomics
only; the callback is the sole playhead writer and seek consumer. Cycle wrap stores the
wrapped playhead, signals a wrap serial, and notes the wrap on the live-MIDI bus.

### 2.2 Audio material and buffer ownership

- **Clips are fully decoded PCM in RAM**
  ([`src/domain/AudioClip.h`](../src/domain/AudioClip.h): "decoded PCM + metadata; immutable
  after construction"). Clip "reading" on the audio thread is pointer arithmetic + SIMD
  copy from the immutable snapshot — no disk I/O, no locks. (Only *proxy* playback streams
  from disk, through its own reader.)
- **Routing bus scratch** is a grow-only pool of shared immutable slots
  (`ensureRoutingBusScratchPool`, Stability C4B: slots are replaced, never resized in
  place; plans co-own them via `RoutingPlan::busScratchOwners`).
- **Post-strip stage scratch** (`postStripStagePtrs_`) is ONE shared stereo buffer in the
  engine, reused serially by every strip in steps 7–10.
- **Insert scratch** is ONE shared stereo buffer in `PluginInsertHost`
  ([`src/plugins/PluginInsertHost.h`](../src/plugins/PluginInsertHost.h): `scratch_`,
  `audioThread_getScratchWritePointers`), reused serially by every chain, plus ONE shared
  `InsertProcessPlayHead` installed in every instance and ONE shared `midiScratch_`.
- **Instrument generation scratch** is per host (`ExperimentalInstrumentHost::scratch_`) —
  this per-owner split is what made the 1.1.18 parallel stage possible.

### 2.3 The channel strip, exactly

All strip variants live in
[`src/engine/PlaybackMixHelpers.cpp`](../src/engine/PlaybackMixHelpers.cpp) and share one
stage order (audio rows line ≈ 731, bus rows ≈ 541/1025, live input ≈ 857, instrument rows
≈ 957):

```
source (clip sum | live input | instrument scratch | bus sum)
  → pre-gain (dB, ramped click-free via PreGainRampState; BEFORE Pre inserts)
  → Pre insert chain      (PluginInsertHost::audioThread_processChainForTrack, Pre)
  → fader × effective mute (gain scale — NOTE: BETWEEN Pre and Post chains)
  → Post insert chain     (…, Post)
  → pan (equal-power law)
  → accumulate into stage → fan to dry bus + sends (fanPostStripStageToDryAndSends)
```

**The fader is inside the chain** (between Pre and Post), and pre-gain is before Pre. Any
design that imagines applying fader/pre-gain "late" on a finished strip output is wrong for
every track with Post inserts; only **pan** and the final sum happen after the last insert.

### 2.4 The 1.1.18 instrument render pool (the approved realtime helper)

[`src/engine/InstrumentRenderPool.h`](../src/engine/InstrumentRenderPool.h): fixed pool
(`defaultWorkerCount()` = physical cores / 2 − 1, capped at 7, 0 on ≤ 3 cores;
`--instrument-workers N` override, `kMaxWorkers` 15, `kMaxJobs` 256), per-job claim flags
(`exchange`), a generation counter with futex-style wake, callback participation with a
≤ 150 µs spin before blocking, longest-job-first ordering by last render duration, and a
small-workload serial rule (`kMinParallelWorkMicros` = 300). **Jobs never outlive the
dispatching callback**, so every drain/gate written for "the audio thread"
(`isAudioCallbackInProcessingSection`, `waitForAudioCallbackExit`, the offline-render gate)
covers the workers. `ARCHITECTURE_PRINCIPLES.md` names this pool the one approved realtime
helper; extending its job model is a steering change — which this document proposes.

### 2.5 Lifecycle, snapshots, state

- Plugin chains: message-thread `PluginInsertHost` publishes an immutable
  `PluginAudioThreadMap` (atomic shared_ptr). Removal retires slots **publish-before-
  destroy**: republish the map, run the drain hook (`waitForAudioCallbackExit`), then
  destroy instances (`retireChainPublishDrainAndDestroy`).
- Instruments: same discipline per host (`activeOwner_` atomic shared_ptr);
  `instrumentProcessingSuspended_` gates the whole instrument section; project teardown
  retires runtimes on the message thread (staged-teardown backlog item notes it is one big
  unit today).
- Offline mixdown (`renderOfflineMixdownBlock`, line 2468) runs **under the offline gate**
  (callback outputs silence meanwhile), builds its own `RoutingPlan`, uses the *same* strip
  helpers and the same live instances, with `isPlaying=true` context and no monitor
  snapshot. Proxy *rendering* is a separate path on isolated instances
  (`ProxyRenderExecutor`) and never touches the live hosts.

### 2.6 Plugin latency: recorded, never compensated

There is **no plugin-delay compensation anywhere in the playback graph**. The only latency
reads are diagnostics (`PluginInsertHost.cpp` layout log, `MainAppWindow` state probe) and
the proxy pipeline, which *records* `getLatencySamples` in the generation metadata without
pre-trimming (PI-014, [`src/io/ProjectFile.h`](../src/io/ProjectFile.h) ≈ line 233).
`docs/CURRENT_ARCHITECTURE.md` (Main output routing) lists PDC as explicitly deferred,
alongside sidechain and multi-output instruments. Consequences recorded for this plan:

- Latent inserts already smear their row relative to other rows today; Stage A/B must not
  *change* that (same instances, same per-segment processing order ⇒ unchanged), and must
  not be sold as fixing it.
- **Design condition for Stage B:** correct time alignment between live and pre-processed
  material relies on both paths processing the *same timeline segments through the same
  uncompensated chains*. If PDC is ever added, the read-ahead time model must be revisited.
- No sidechain and no feedback routing exist (`SessionRouting` validates acyclicity; sends
  target Groups only), so today the only cross-row dependencies are the dry-bus/send sums.

### 2.7 What processes under Mute vs Off (checked, with inconsistencies)

Verified in `PlaybackMixHelpers.cpp` at `f11c0fb` (every strip computes
`effectiveGain = effectiveMuted ? 0 : fader` and gates the insert chain on
`useInsert && effectiveGain > 0`):

| Row state | Instrument host (`processBlock`) | Insert chains (Pre+Post) | Clip read loop |
|---|---|---|---|
| Audio row, Mute (or solo-silenced) | — | **skipped** | runs (adds gain 0) |
| Audio row, fader −∞, not muted | — | **skipped** (row `continue`s entirely) | skipped |
| Audio row, Off | — | skipped | skipped |
| Instrument row, Mute / fader −∞ | **processes** (gain-0 fold; 1.1.9 rule — MIDI consumed, state follows transport) | **skipped** | n/a |
| Instrument row, Off | skipped | skipped | n/a |
| Group/Master, Mute | n/a | **skipped** | n/a |
| Monitored audio row (Monitor on) | — | **always processes**, even on silent/unresolved input ("insert tails keep ringing") | suppressed |

Documented inconsistencies (NOT changed by this plan — a separate backlog row owns the
decision):

- `docs/CURRENT_ARCHITECTURE.md` "Power (Off) vs Mute" says Mute means "still running the
  lane's processing path"; that is true for the instrument **host** but **not** for insert
  chains — a muted row's inserts stop processing (tails freeze mid-state and resume on
  unmute). The `PHASE_PLAN.md` Mute/Off backlog row says "a muted row still processes its
  inserts (gain 0 after the chain)", which does not match the code at `f11c0fb`.
- Mute behaves differently per source kind (instrument host keeps running, audio row's
  chain does not), and a monitored row's chain runs even with no input while an unmonitored
  muted row's chain does not.

Stage A **preserves these semantics bit-for-bit** (the job body is the same strip code).
Stage B's classification must treat Mute/fader changes as chain-behaviour changes (§4.3)
precisely *because* of the gain-0-skips-chain rule.

### 2.8 Dependency summary — what limits parallelism today

Per block, in hard dependency order:

1. Seek/record-run/clock bookkeeping (cheap, order-critical) →
2. live-MIDI delivery + transport-MIDI scheduling **into hosts** (must precede generation) →
3. per-source-row work: clip read + strip (audio), generation + strip (instrument) — rows
   are **mutually independent** here *except* that they all share the single insert scratch,
   the single insert playhead, the single post-strip stage buffer, and they accumulate into
   shared bus scratch →
4. bus steps in topological order (each Group needs all its sources summed first; Master
   needs all Groups) →
5. device output, meters.

The serialization in step 3 is **buffer sharing, not data dependency**. That is the entire
opportunity for Stage A.

---

## 3. Stage A — parallel source-row strips (smallest robust stage)

**Target:** the measured ≈ 4.04 ms mean / 9.44 ms max of serial audio-row insert processing
(18 AmpliTube instances ≈ 3.66 ms of it), while instrument generation (≈ 1.60 ms wall) is
already parallel. No quantitative speed-up is promised here; the claim is only that the
work units are independent and the join model already exists.

### 3.1 Job partitioning

- **One job per source row** (Audio row with an active strip; later also the instrument
  rows' *strip* stage): the job renders the row's complete block — clip segments (both
  cycle-wrap segments, carried as up to 2 segment descriptors in the job), pre-gain, Pre
  chain, fader, Post chain, pan — into a **per-job stage buffer**, and stops there. No job
  touches a shared bus.
- **Fan-out and summing stay serial on the callback**, after the join, and must reproduce
  **today's verified accumulation order** — not an assumed "common row order". Verified at
  `f11c0fb`: a bus scratch sample receives, in order, (1) the count-in click (master bus
  only, MixPrep), (2) audio-row stage fans in `SourceStep` order — today executed
  *segment-major* (`renderRun` fans every step for segment 1, then every step for
  segment 2 on a cycle wrap), (3) monitored rows' stage fans, (4) instrument rows' stage
  fans in session row order, (5) bus-step forwarding in topological order. Because the two
  wrap segments occupy **disjoint frame ranges** of every bus buffer (`destFrame` offsets),
  fanning *step-major after the join* (per step: its segment-1 stage then its segment-2
  stage) produces, at every individual bus frame, the same contributions added in the same
  order as today's segment-major loop ⇒ bit-identical sums. Per-segment stage results are
  therefore kept separate in the job output until the callback fans them; they are never
  pre-summed across segments. Contributions (3)–(5) keep their existing positions after the
  audio fans. Any deviation from this order is a semantic change and is out of scope for A1.
- **Never-concurrent, exactly-covered:** chains are per `TrackId` and each track gets at
  most one job (same duplicate-guard pattern as today's `addJob`); the monitored row is
  excluded from the clip path already (its chain runs only in the monitor pass), the
  recording row is excluded via `omitClipPlaybackForTrack`. The correctness rule is NOT
  "exactly one `processBlock` call per device block" — that was an oversimplification: a
  device block that wraps the cycle splits into two timeline segments, and an audio row's
  insert chain legitimately runs **once per segment** (two `processBlock` calls for that
  device block) today. The rules that actually hold, before and after A1:
  **(i)** no plug-in instance is ever processed concurrently from more than one thread;
  **(ii)** no intended timeline segment / sample interval is processed twice or dropped —
  the segment set per instance per device block is exactly today's set;
  **(iii)** the existing segmentation, the call order of the segments, and the per-call
  transport/playhead context each instance observes are preserved unchanged.

### 3.2 Shared-state decomposition (the real work)

The complete map of **mutable state the parallelized calls touch**, each with its owner,
its lifetime, and its A1 treatment. Invariant: **no two concurrent jobs may share writable
scratch or a mutable transport context.** All replacements are allocated at
`prepareForDevice` / publish time (no audio-thread allocation).

| # | Mutable resource | Owner today | Lifetime today | Why it serializes | A1 treatment |
|---|---|---|---|---|---|
| 1 | Insert audio scratch `PluginInsertHost::scratch_` (+ `scratchPtrs_`) | `PluginInsertHost` | device prepare → next prepare | ONE buffer reused by every chain call | Per-**lane** stereo scratch (one lane per thread that can run a job: workers + callback), passed into a new chain-processing variant that takes caller buffers. A job runs entirely on one lane ⇒ exclusive use. |
| 2 | Insert **MIDI scratch** `PluginInsertHost::midiScratch_` (cleared after every `processBlock`) | `PluginInsertHost` | device prepare → next prepare | ONE `juce::MidiBuffer` handed to every insert `processBlock`; concurrent clear/use races | Per-lane `MidiBuffer`, pre-sized at prepare, cleared after each call exactly as today. |
| 3 | Insert playhead `InsertProcessPlayHead processPlayHead_` (installed via `setPlayHead` on every instance) | `PluginInsertHost` | host lifetime (outlives instances) | ONE mutable `PositionInfo` read synchronously from inside `processBlock` on whichever thread runs it | Per-**track-chain** playhead object (every instance belongs to exactly one chain), installed at publish/prepare time, co-owned so it outlives every published map that references it; the job sets its context per segment with the same values as today's `setInsertProcessContext`. Serial paths (buses, monitor, offline) keep working: the global context setter updates every chain's playhead. |
| 4 | Post-strip stage buffer `postStripStagePtrs_` | `PlaybackEngine` | device prepare → next prepare | ONE stereo buffer reused by every strip in steps 7–10 | Per-job stage buffers (the job's output), engine-owned, sized rows × block at prepare; consumed by the serial fan-out. Serial strips (buses, monitor, instrument rows in A1, offline) keep the shared buffer. |
| 5 | Pre-gain ramp `PreGainRampState` (per-track-index array, engine-owned, reset at device start) | `PlaybackEngine` | device prepare → next prepare | Indexed by track index — each job reads/writes **only its own row's entry**, and each row has at most one job | Kept as-is; safe by disjoint indices. The offline path already passes `nullptr` (no ramp). |
| 6 | C2B insert diagnostics atomics (`audioThreadInsertTrackId_` / `SlotIndex_` / `Stage_`) | `PluginInsertHost` | host lifetime | Global "where is the audio thread" markers — concurrent jobs would interleave garbage | Per-lane atomic marker slots; the diagnostics formatter reports every lane. Diagnostics only, but cross-job mixing would be *misleading*, so it is decomposed, not waved through. |
| 7 | Insert level tap accumulators (`insertLevelTapPeak…`, fold reads the **member** `scratch_`) | `PluginInsertHost` | host lifetime | Fold helper hardwired to the shared scratch | Fold parameterized by the caller's scratch pointers; accumulators are already relaxed atomics (CAS-max / fetch_add), safe to fold from any thread. |
| 8 | Track meters (`audioThread_foldTrackMeterIfMetered`, meter bank) | `PlaybackEngine` | engine lifetime | Single-writer accumulators | Folds stay on the **callback thread**, moved to the serial fan-out phase (the job's stage output is read there); the bank keeps its single-writer model, no cross-job mixing. |
| 9 | `AudioThreadProfiler` per-block scratch | profiler singleton | process | — | Already multi-thread-ready since 1.1.18 (atomic per-block category scratch, `setInsideGenerationJob` TLS); insert folds from jobs use the same discipline. |
| 10 | Transport context for inserts (`setInsertProcessContext` values) | callback (per segment) | one segment | Mutable, segment-scoped | Carried **by value in the job's segment descriptors**; the job writes it into its own chain playheads (#3) only. No job reads another job's context. |

Resources NOT touched by the parallelized calls (verified): routing bus scratch (jobs never
fan — see §3.1), device output buffers, recorder SPSC, transport atomics (jobs receive
precomputed segment descriptors; only the callback advances the playhead), session/plugin
snapshots (immutable, acquire-loaded once by the callback and passed as resolved pointers).

### 3.3 Pool reuse — one dispatch, one join, no second barrier

Reuse `InstrumentRenderPool` (same workers, same claim/generation protocol, same MMCSS
thread class) with a **generalized job payload** (today's job is hardcoded to
`host->audioThread_renderGenerationStageForBlock`; it becomes a small tagged union or
function-pointer + context). Callback restructuring:

- Move transport-MIDI scheduling (and proxy segment notes) for **all** segments of the
  block *before* the dispatch (it is cheap — measured 0.045 ms — and must precede
  instrument generation).
- Dispatch **one batch**: audio-row strip jobs + instrument generation jobs (slice A2 folds
  the instrument rows' strips into their generation jobs so one job = one row end-to-end).
  One barrier per block, exactly as today — no new wake/join points, no second pool, no
  oversubscription (worker count unchanged; the pool's small-workload serial rule and the
  serial fallback path apply to the new job kinds identically).
- Join, then serial: fan-out in row order → monitor pass → bus steps → master → meters.

### 3.4 What stays serial in Stage A, and why

- **Group and Master bus strips** — dependent on all sources (topological order); typically
  few rows and measured cheap (bus finalize ≈ 0.002 ms on the 100-track project). A later
  slice could parallelize independent Group subtrees; not worth a barrier now.
- **The live-input monitoring pass** — stays on the callback thread, unchanged, so
  monitoring latency is untouched (no handoff, no extra block). Workers never touch the
  monitored row's chain (it has no clip job).
- **Proxy-backed instrument rows** — existing rule kept (their mix is a ring copy; the
  Primary must not run).
- Recorder push, live-MIDI delivery, count-in, offline mixdown, the sessionless fallback —
  unchanged.

### 3.5 Lifetime and drains

Unchanged by construction: jobs never outlive the callback, so `waitForAudioCallbackExit`
(used by the insert map's publish-before-destroy drain and by every host retirement) and
the offline gate continue to cover worker access to plug-in instances. The insert map's
acquire-load happens once on the callback thread; jobs receive the already-resolved chain
pointers for their row (no additional map loads on workers).

### 3.6 Proposed first scope

- **A1:** per-track insert scratch/playhead decomposition + audio-row strip jobs in the
  existing dispatch. Instrument row strips stay serial (as today).
- **A2:** fold instrument generation + strip into one job per instrument row.
- Acceptance: bit-identical output serial vs parallel with deterministic content (§6);
  `--instrument-workers 0` remains the identical-code serial path; no new callback phases
  beyond renamed existing ones.

---

## 4. Stage B — read-ahead pre-processing (DESIGN DRAFT — unresolved transitions)

> **Status: design draft, not an implementable specification.** The ownership transitions
> in §4.4 (`Live → Ahead` and `Ahead → Live`) are **not resolved**: a safe ownership
> handover transfers *who may call the instance*, but it cannot reset the instance's
> internal time position. At `Ahead → Live` the instance may already have processed
> several blocks past the audible position — the callback cannot "resume live" at the
> audible playhead without re-feeding samples the instance has already consumed (which is
> forbidden), so demotion with an invalidated prefix leaves a gap that the current draft
> does not close. At `Live → Ahead` the ring starts empty and must fill while the row
> keeps playing — the draft must not assume a seamless, continuity-preserving handover.
> Consequently the earlier "control latency = 0 (always demote)" / "parameter changes are
> as immediate as today" promise is **not substantiated by this design** and is withdrawn
> until the transitions are solved. These problems are to be resolved before any Stage B
> implementation; nothing in Stage A depends on them.

The unit of pre-computation is deliberately the **same unit Stage A creates: one source
row's post-strip stage output for one device block**. Buses, master, summing and all live
paths stay in the callback. This is what makes Stage B a consumer of Stage A rather than a
second engine.

### 4.1 Classification — which rows can be pre-processed

A row is **live** (never pre-processed) while any of these hold; each maps to an existing
published signal the engine already reads per block:

| Live condition | Existing source of truth |
|---|---|
| Audio row monitored | `LiveInputMonitorSnapshot` |
| Row armed / actively recording | record-run state + `RecorderService`; armed MIDI rows via `LiveMidiInputCoordinator` routing snapshot |
| Instrument row receiving live MIDI (monitor or capture) | `LiveMidiInputBus` routing snapshot |
| Instrument row with Secondary audition active (editor open, transport stopped) | playback snapshot `auditionHost` gating |
| Instrument row under proxy/Secondary-live override | `ProxyPlaybackView` / coordinator dependencies |
| Count-in running | count-in state |

Everything else — audio rows playing clips, instrument rows driven purely by baked
transport MIDI (`InstrumentTrackRenderSnapshot`) — is **pre-computable**. The user's
stated workflow (one monitored track while recording, everything else playback) means the
pre-computable set is normally "all rows but one".

**Mixed buses:** Groups/Master receive both live and pre-computed *stage outputs* and are
themselves always processed live in the callback (cheap, measured). So "a group with one
live member" needs no special case: its live member contributes live, the rest contribute
prepared stages. No downstream node is ever forced live *by* an upstream live member,
because bus processing is live for everyone. Sidechain/feedback dependencies: none exist
today (§2.6); if sidechain ever lands, any sidechain *listener* whose source is live must
itself be classified live — recorded here as a forward constraint.

Classification is re-derived on the message thread whenever its inputs change (monitor
toggle, arm, record start/stop, routing snapshot, proxy view) and published like every
other view (atomic shared_ptr, block-boundary latch).

### 4.2 Time model

- **Domain:** timeline samples, exactly as the callback's `renderRun` computes them. Given
  transport intent, playhead, cycle locators and arrangement extent — all visible at block
  start — the segment sequence of future blocks is **deterministic** (linear advance; at
  the right locator, wrap to L with the same two-segment split the callback does; stop at
  the extent). The pre-processor renders *the same segments the callback will later
  compute*, identified by `(timelineStart, length, wrapSerial)`; cycle wraps are therefore
  pre-renderable (prior art: the proxy reader's prepared-loop model).
- **Depth:** small and fixed initially — e.g. 2–4 device blocks (≈ 21–43 ms at 512/48 kHz),
  a build-time constant with a `--readahead-blocks N` developer override; **not** a user
  setting in the first version. Depth bounds both memory (per row: depth × stereo block)
  and worst-case control latency (§4.3), so small is a feature.
- **Delivery:** per pre-computable row a preallocated SPSC ring of stage blocks stamped
  `(generation, timelineStart, wrapSerial)`. The callback, per row: if the head matches the
  block it is about to sum → consume (memcpy-level cost); else → miss path (§4.5).
- **No added live latency, structurally:** the device deadline, block size and the live
  paths' processing are untouched; read-ahead only changes *when* playback-only rows'
  strips are computed. Live input never enters a ring. Time alignment of live vs
  pre-computed material: both are rendered for the same timeline segment through the same
  (uncompensated, §2.6) chains, so their relative alignment is exactly today's.
- **Plugin transport context:** workers set the per-chain playhead (Stage A's
  decomposition) to the *future* segment's position — hosted plug-ins see the same
  `PositionInfo` sequence they would see live, just earlier in wall time. Pre-processing
  uses the normal realtime processing path; **`setNonRealtime` / the offline flag is never
  touched** (plug-ins may legally render differently offline — we make no assumption that
  they don't).

### 4.3 Changes and transport — what invalidates, what the user hears

The strip order (§2.3) dictates honesty here: pre-gain and fader sit *inside* the chain,
and Mute at gain 0 *skips* the chain (§2.7). So fader/Mute/pre-gain changes alter chain
input or chain execution — they can NOT be late-applied to a finished stage buffer without
changing today's semantics. The design therefore has exactly two change classes:

- **Late-applicable (consume-time):** nothing in the first version. (Pan is the only
  theoretical candidate — it is after the last insert — but splitting it out complicates
  the buffer format for negligible gain; deferred.)
- **Invalidating:** everything else — fader, pre-gain, Mute, Solo view change, pan, insert
  parameter change (including automation/CC-driven), insert add/remove/reorder/swap,
  clip edits (MIDI and audio), tempo change (rebakes render snapshots), routing or send
  change (changes fan-out, and the plan rebuild already republishes), Monitor/arm toggle,
  recording start, memory-solo switch, project switch, seek, stop, cycle toggle or locator
  move, playback-offset change.

**Invalidation protocol:** bump the row's (or, in the first version, a single global)
generation counter; stale ring entries are ignored by the consumer (generation mismatch —
same pattern as the render pool's generation and the C2B stale-plan guards). The row then
runs **live in the callback** (today's Stage A path — same code) until the pre-processor
has re-primed at the new state. Consequences, stated plainly:

- **What the user hears:** the *intent* is that an edited row drops to live processing at
  the next block boundary. **Caveat (see the §4 draft status):** because the demoted
  instance may already be several blocks ahead of the audible position, "drops to live"
  is not immediate in the current draft — the transition leaves either stale audio (if the
  prefix is kept) or a gap (if it is invalidated). The immediacy claim is withdrawn until
  the `Ahead → Live` transition is solved.
- **Where immediate response structurally requires live processing:** monitored input,
  live MIDI to instruments, audition, count-in — exactly the §4.1 live set.
- **Transport:** Play from stop can be pre-primed while stopped (playhead known; prime
  `[playhead, playhead + depth)`), so Play consumes prepared blocks immediately. Seek =
  global invalidation + live until re-primed. Cycle wrap is *not* an invalidation (§4.2).
  Stop discards nothing eagerly; rings just stop being consumed, and the next event
  (seek/edit/Play) decides.

### 4.4 Plugin state and ownership

**One owner per instance per block, with a fence-based handover.** Per row a small state
machine, advanced only at block boundaries:

- `Live` — callback processes the instance (today's path).
- `Ahead` — the read-ahead worker processes the instance, strictly sequentially along the
  predicted timeline (block n+1, n+2, …). The callback only consumes ring output and NEVER
  touches the instance.
- `Live → Ahead` (promotion): at a block fence where the callback has processed through
  block n, ownership transfers; the worker continues from n+1. The instance sees one
  continuous sample/MIDI stream — no gap, no overlap, no double notes. **Unresolved:** the
  row's ring is empty at the fence and fills only as fast as the worker renders; the
  design must not assume continuity across the handover — until the ring has at least one
  valid block the row would miss (silence) unless promotion is deferred until primed.
  Which of those (defer vs. accept a priming window) applies is an open decision.
- `Ahead → Live` (demotion, on invalidation or classification change): the worker finishes
  (or abandons *before starting*) its current block and releases ownership. The instance's
  internal state is exactly "has processed through block m" at all times — the handover
  transfers *the right to call the instance*, it does **not** rewind the instance's
  internal time position, and m may be several blocks past the audible playhead.
  Re-processing discarded blocks through the same instance is forbidden (stateful
  plug-ins would double-count the input). **Unresolved consequence:** when the prefix
  (audible position .. m] is still valid it can play out from the ring and the callback
  resumes at m+1; but when the edit *invalidated* that prefix, the samples in
  (audible .. m] can be neither replayed from the ring (stale) nor re-processed (double
  feed) — the current draft leaves a gap (silence) there, which contradicts the
  "immediate fallback" ambition. This transition must be redesigned (candidates: bound
  the gap by depth and accept it, demote lazily at m+1 only, or keep stale-but-harmless
  prefixes per §4.3) before Stage B is implementable.
- **Separate instances or state checkpoints were considered and rejected** for v1:
  checkpoint restore via `getStateInformation`/`setStateInformation` is not RT-safe, and
  the proxy work proved state blobs are volatile (Groove Agent SE's grow per call) and
  asynchronous to audibility (kit streaming) — a restore mid-playback risks silence or
  wrong sound. Costs and risks are documented so the decision is explicit.

**Project save while Ahead — OPEN semantic decision (not an accepted deviation):**
`exportChain`/instrument state capture run on the message thread against instances that may
have processed up to `depth` blocks (≤ ~43 ms) past the audible position. Today the same
capture during playback is already mid-flight by an in-block amount, so the *degree*
changes rather than the *kind* — but whether a saved project may legitimately capture
plug-in state from a position the user has not yet heard is a semantic question steering
has not decided. Options on the table: accept the bounded skew, or demote-and-fence all
rows before capture (costing a save-time hiccup). This decision must be made explicitly
before Stage B lands; this plan does not pre-accept either answer.

### 4.5 Realtime and failure paths

- **All allocation up front:** rings, stage buffers, segment descriptors sized at
  `prepareForDevice` (rows × depth × block). Generation stamps + atomic indices; no locks
  on the consumer side (the proxy reader's fetch contract is the template, PI-031).
- **Not ready at the deadline:** the callback must never wait on a background job and must
  never process the worker-owned instance itself. Miss ⇒ that row contributes **silence
  for this block**, an underrun counter increments (relaxed), and the row is scheduled for
  demotion at the fence (§4.4). This mirrors proxy-playback underrun semantics exactly —
  audible as a dropout of one row, never as a stall of the device. There is deliberately
  **no** "process it quickly now" fallback: that is the double-processing trap.
- **Stale results:** consumer checks `(generation, timelineStart, wrapSerial)`; mismatch =
  miss. Workers check the generation before starting a block and abandon stale work.
- **Threads and priority:** read-ahead runs on its own thread(s) — **one** worker initially
  — at BELOW the callback/pool priority (normal or just-above-normal; explicitly not the
  MMCSS Pro Audio class). It must never compete with the Stage A pool inside the deadline:
  Stage A workers are deadline-critical callback participants; the read-ahead thread is
  throughput-only. This is the same separation the proxy I/O service already practices
  (one shared low-priority fill thread). CPU oversubscription is avoided by count (pool ≤ 7
  + 1 read-ahead + callback on the user's 24-core machine) and by priority, not by
  cleverness.
- **Device stop/start, sample-rate or block-size change:** `prepareForDevice` /
  `releaseResources` invalidate everything, resize off the audio thread, and re-prime;
  the ownership state machine resets to `Live` for all rows. The existing drains
  (`waitForAudioCallbackExit`, publish-before-destroy, `instrumentProcessingSuspended_`,
  offline gate) must additionally fence the read-ahead worker (a new "read-ahead quiescent"
  gate joined on the message thread) before any instance it might own is retired — this is
  the one genuinely NEW lifetime rule Stage B introduces, because read-ahead work is the
  first plug-in processing that outlives a callback.

---

## 5. Recommended staging

1. **A1 — decompose shared insert state + parallel audio-row strips** (per-track insert
   scratch and playhead, per-job stage buffers, strip jobs in the existing dispatch, serial
   row-order fan-out). Attacks the measured 4.04 ms directly. Smallest slice that forces
   the shared-state decomposition every later stage needs.
2. **A2 — instrument generation + strip as one job per row.** Removes the remaining serial
   instrument-insert work (Pro-Q 3 / Mono Delay chains) with no new barrier.
3. **Mute/Off processing rule** (existing backlog row, do before B): Stage B's invalidation
   model leans on mute semantics; deciding the ONE rule first (and fixing the
   `PHASE_PLAN.md`/`CURRENT_ARCHITECTURE.md` wording drift documented in §2.7) avoids
   designing read-ahead around behaviour that is about to change.
4. **B1 — minimal read-ahead:** audio rows only (instrument rows stay live — their
   generation is already parallel and their MIDI wiring is the most entangled part);
   single global generation (any invalidating event demotes ALL pre-processed rows to live
   = today's path, then re-prime); depth 2 blocks; one worker; prime-while-stopped;
   explicit limitations: no per-row invalidation granularity, no instrument read-ahead, no
   late-applied parameters. **Precondition:** the §4 draft-status problems (the
   `Ahead → Live` gap, promotion priming, save-while-ahead semantics) are resolved first —
   the previously stated "control latency = 0 (always demote)" property is not
   substantiated by the current draft.
5. **B2+ (deferred deliberately):** instrument-row read-ahead (requires fencing transport-
   MIDI scheduling into the worker timeline), per-row generations, pan as a late-applied
   parameter, bus-subtree parallelism, any PDC work, staged project teardown.

Explicitly out of scope and unchanged: proxy source priority (PI-021) and every proxy
identity/currency rule; a user-selectable proxy despite an available Primary remains a
separate future feature.

If A1/A2 land and the next Windows measurement shows the callback comfortably under budget
for the user's real projects, **B can be re-scoped or postponed** — the plan's own position
is that A is justified by measurement today, while B's complexity (ownership, invalidation)
is justified only if post-A measurements still show deadline pressure.

## Key design decisions, risks, open questions

**Decisions (proposed):** one pool, one barrier (A); job = whole source-row strip; summing
serial in row order (determinism by construction); Stage B consumes Stage A's unit; miss =
silence + demotion, never wait, never double-process; same-instance sequential ownership
with fences, no checkpoints; realtime processing path for read-ahead (no offline flag);
read-ahead on a separate low-priority thread, never the deadline pool.

**Risks:** third-party `processBlock` thread-affinity assumptions (AmpliTube ran only on
the callback thread until now — instruments already proved multi-thread-tolerant under the
pool, but each new plug-in class needs the §6 soak); diagnostics singletons (insert tap,
C2B markers) need per-thread folding; Stage B's new "worker outlives the callback" lifetime
rule is the first break of the 1.1.18 invariant and carries the retirement-fence burden;
save-while-ahead state skew (§4.4 — an OPEN semantic decision, see the §4 draft status);
the unresolved `Ahead → Live` / `Live → Ahead` transitions (§4 draft status) block any
Stage B implementation until redesigned.

**Open questions for steering:** Is a transient one-row silence (counted, logged) an
acceptable miss behaviour for B1, or must B1 demote so eagerly that misses are effectively
impossible? Should prime-while-stopped run always or only after an explicit Play-armed
state (battery/CPU courtesy)? Where should the `--readahead-blocks` developer override live
(command line vs config), given diagnostics-gating policy?

---

## 6. Future verification plan (plan only — nothing here was run)

- **Bit-identity where it is honest:** serial (`--instrument-workers 0`) vs parallel runs
  must produce byte-identical output for DAL-deterministic content — clips, deterministic
  test instruments (the `installInstrumentInstanceForTests` seam), DAL Mono Delay chains —
  extending the existing `InstrumentParallelFocusedTests` pattern to strip jobs and to
  read-ahead consume paths. **Third-party plug-ins are explicitly NOT held to bit-identity**
  (AmpliTube/Pro-Q may be internally nondeterministic); for them the checks are structural
  (below) plus listening.
- **Never-concurrent, exactly-covered (NOT "once per device block"):** counter assertions
  through the existing relaxed activity counters (`processOkBlocks`, insert tap block
  counts) in focused tests, asserting the corrected §3.1 rule: no instance processed
  concurrently from two threads; the set of timeline segments processed per instance per
  device block equals today's set (one call per segment — a wrap block legitimately
  produces two calls), in the same order, with the same per-call playhead context; no
  intended segment processed twice or dropped. Cycle-wrap blocks are a mandatory fixture,
  precisely because they falsify the naive "one call per block" assertion.
- **Routing/summing determinism:** fixed fixture (sources → Groups → sends → Master),
  repeated runs compare full-mix checksums serial vs parallel vs read-ahead-primed.
- **Transition matrix:** monitor on/off, arm, record start/stop (incl. count-in and cycle
  takes), seek, Play/Stop, locator edits, insert add/remove during playback, project
  switch, device stop/start and block-size change — each asserting: no stuck notes, no
  double processing, correct demotion/re-prime, meters/undo unaffected.
- **Lifetime:** plug-in removal/reload and host retirement under active jobs and under
  read-ahead ownership (publish-before-destroy + the new read-ahead fence), ASAN/soak.
- **Deadline distribution on the real target:** `--stability-perf-profile` runs on the
  user's Windows/ASIO machine (100-track project and the user's real projects), comparing
  callback mean/max, overrun counts and the generation/strip phase split before vs after
  each slice; plus a listening pass on a real project by the user. **Explicitly: this
  cloud/Linux environment can verify logic and determinism harnesses only — no claim about
  Windows/ASIO performance or audible behaviour is made or implied until the user's
  Windows measurements and listening confirm it.**

---

## References

- Code (at `f11c0fb`): `src/engine/PlaybackEngine.{h,cpp}`, `src/engine/PlaybackMixHelpers.{h,cpp}`,
  `src/engine/InstrumentRenderPool.h`, `src/engine/RoutingPlan.h`, `src/engine/RoutingPlanBuilder.cpp`,
  `src/plugins/PluginInsertHost.{h,cpp}`, `src/plugins/ExperimentalInstrumentHost.h`,
  `src/transport/Transport.h`, `src/engine/LiveMidiInputBus.h`, `src/instruments/ProxyPlaybackReader.h`,
  `src/domain/AudioClip.h`, `src/io/ProjectFile.h`.
- Docs: `PARALLEL_INSTRUMENT_RENDERING_2026-10-06.md`, `PERF_PROFILE_100TRACKS_2026-10-06.md`,
  `CURRENT_ARCHITECTURE.md` (threads, routing, Power vs Mute, live MIDI),
  `ARCHITECTURE_PRINCIPLES.md` (realtime rules, approved pool), `PHASE_PLAN.md` (backlog rows).
- External reference concept: Steinberg's published ASIO-Guard documentation (help.steinberg.net,
  "ASIO-Guard"): documented behaviour — two processing paths, pre-processing excluded for
  record-enabled/monitored channels — is separated throughout this plan from our own design
  proposals, which are everything in §3–§5.
