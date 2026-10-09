# Read-ahead prototype — state/time model and transitions (Stage B prototype, v2)

**Status: experimental, OFF by default.** Enabled only with the CLI flag
`--experimental-readahead` (optional depth: `--experimental-readahead=N`, N = 2..8 queued
segments, default 3). Without the flag the engine contains **no** read-ahead objects and every
code path is byte-for-byte the A1/A2 behavior. This is DAL's own pre-processing of eligible
audio tracks; it is **not** an integration with Steinberg's ASIO-Guard.

This is the v2 model. v1 (the first prototype commit) treated every transport pause as a
discontinuity and gated cycle off entirely; v2 replaces those with a coherent stream model
that supports pause/resume without re-feeding, cycle playback, and a defined save path.
The implementation lives in `src/engine/ReadAheadRenderer.{h,cpp}` plus narrow integration
points in `PlaybackEngine`, `PluginInsertHost`, `ProjectIoCoordinator` and `Main`.

---

## 1. The core problem and the chosen model

A plugin that has rendered future blocks has also advanced its internal DSP state (delay
lines, envelopes, oscillators). Discarding an audio buffer or waiting on a fence does **not**
rewind that state, and generic `getState`/`setState` is **not** a sample-exact checkpoint of
a plugin's DSP. The model is therefore built around one object and one invariant:

**The audible segment stream.** The engine renders playback as a deterministic sequence of
segments — `(timeline start, run length, frame offset in the device block)` — fully
determined by block-stable inputs: block size, transport position, cycle locators,
arrangement end and the playback latency offset. A linear block is one full segment; a
cycle-wrap block is two (up to the right locator, then from the left locator). The renderer
and its worker derive **the same sequence from the same inputs**, so the worker can render
segments ahead and the callback can consume them by exact key.

> **Invariant: exactly-once, contiguous segment stream per plugin instance** while a row is
> owned. Every insert instance on a read-ahead row processes the audible segment sequence in
> order, each segment exactly once, regardless of which thread rendered it or whether the
> rendered output was ultimately played. Transitions either preserve this (gapless adoption,
> gapless drain, pause/resume continuation) or are deliberate discontinuities with a bounded,
> counted, documented state residue (seek, locator/cycle edits, monitor/record handover,
> miss abandonment).

Four distinct positions exist and are never conflated:

| Position | Owner | Meaning |
|---|---|---|
| Transport playhead `t0` | callback | where the audible block starts |
| Instance stream position | worker (while owned) | end of the contiguous segment stream fed to the row's chain; ahead of `t0` by ≤ depth queued segments |
| Queue contents | SPSC ring | the rendered segments between the audible position and the instance position, each keyed `(seq, start, run, destFrame, generation)` |
| Expected consume key | callback | the next segment the audible stream needs from the queue |

Ownership may change hands only at block begin on the callback thread, and only when the
worker provably holds no chain (claim/stop Dekker pair, or its acknowledged pause).

All queue keys and rendered content use the **audible timeline domain** (transport position
plus the configured playback latency offset); run-length limits replicate the engine's own
transport-domain arithmetic exactly. v1 mixed the two domains, which broke adoption whenever
a non-zero playback offset was configured — fixed in v2. The worker start gate is a
**monotone callback block serial** (not a timeline position), so it is immune to cycle wraps
moving positions backwards.

## 2. Scope (deliberately narrow)

* **Eligible rows:** audio tracks that Stage A1 would give a strip job, rendered **through
  the production strip core** (`renderAudioTrackPostStripToStereoScratchWithChainAccess`:
  clips → pre-gain → Pre inserts → fader/mute/solo → Post inserts → pan — the real chain
  order, fader kept inside the strip). At most 16 rows adopted; the rest stay on live A1.
* **Excluded from adoption:** monitored rows, the record-armed row, the recording row, rows
  within (depth+2) blocks of the arrangement end, rows at a negative audible position.
* **Everything else untouched:** instruments (A2), proxies, buses, sends-fan, monitoring,
  offline export, count-in, recording capture (raw pre-strip — read-ahead can never touch
  recorded samples), PDC, Mute/Off policy, automation.

## 3. Transport transitions

**Pause / resume (Space):** *not* a discontinuity. The playhead keeps its position, rows
stay owned, the queue stays intact (the worker may keep filling it to the bounded depth).
Resume at the same position continues consuming from the queue: the instance stream is
contiguous and exactly-once across the pause — **no re-feed, no discarded state** (v1
discarded at every stop and re-fed ≤ depth blocks on replay; that artifact is gone).

**Stop button (= stop + seek 0), seek, locator/cycle-geometry edits, playback-offset
changes:** deliberate discontinuities. Queued outputs are discarded (counted), rows return
to live rendering at the new position. The instance's state lead (≤ depth segments past the
jump point) rides the jump. The direct path also makes a state jump at a seek (tails from
pre-seek material ring into post-seek material); read-ahead's difference is only that the
pre-jump history ends ≤ depth segments later than the audible stop point. Replaying a region
after a jump feeds duplicate timeline positions in **both** paths — that is normal DAW
transport behavior, not a read-ahead defect. No reset is used on contiguous transitions, and
no "rewind" of plugin state is ever claimed or attempted.

**Stop while the worker is mid-segment:** nothing special — ownership transitions always
wait for the claim to clear (`busy == 0`) or the worker's acknowledgment; a row whose worker
is still inside a render is `Abandoning` (silent for that row, counted) for at most one
block before it goes live.

**Frozen playhead (arrangement end, count-in):** the expected-position check and the
consume-absence check (a row that is owned but was never offered a consume this block left
the routing plan or the rendered path) release ownership within one block.

**Edits while paused** (fader, clip, plugin parameters on an owned row): the queue already
holds ≤ depth rendered segments with pre-edit values, so after resume the edit is audible at
most depth segments late — the same explicit late-apply window as during playback (§6). This
is a stated experimental decision, not a hidden behavior.

## 4. Cycle

Cycle is supported, on the engine's own segmentation rules — no separate DSP path:

* The worker replicates the engine's wrap arithmetic exactly (first run cut at the right
  locator; second segment from the left locator filling the rest of the block; short blocks
  when the loop span or arrangement end cuts the second run). Each ring slot is one
  **segment** with its own `destFrame`, rendered into the slot at that offset, so the
  consume-side fan uses the production fan helper unchanged.
* **Loop iterations are distinguished by a per-adoption monotone sequence number** carried
  in each slot and tracked by the consumer. Two passes over the same loop positions can
  never be confused, and stale results (produced for a segment the audible stream already
  passed, e.g. after a miss) are detected by `seq`, not by position comparison — position
  ordering is meaningless across a wrap.
* The worker's per-segment transport context carries the segment's true timeline start and
  the active loop range (`isLooping`, loop start/end), same values the serial path stamps.
* Loop spans shorter than the read-ahead horizon just mean the queue holds more, shorter
  segments; the depth bound counts segments, so the state lead is ≤ depth segments ≈ ≤ depth
  blocks of samples.
* Enabling/disabling cycle or moving a locator **while rows are owned** is a geometry change:
  queued segments were predicted under the old geometry, so the rows take a discard reset
  (deliberate-transport-edit class, §3) and immediately re-adopt under the new geometry.
* If the engine's actual segmentation ever diverges from the worker's prediction (defensive
  case), the exact-key consume simply misses and the miss policy (§7) self-heals; wrong-time
  audio can never be played because keys must match exactly.

## 5. Monitor, arm and record

* **Record-armed rows are never adopted**, and arming a row that is owned starts a gapless
  draining release (pre-emptive drain: by the time recording actually starts, the row has
  long been back on the live path, so record start, count-in and the record-run boundaries
  behave exactly as A1/A2). Recording capture is raw pre-strip input and is never routed
  through read-ahead at all — input can not be lost by this feature.
* **Monitor enabled on an owned row = immediate handover**, not a drain: the direct path
  switches instantly (clip playback suppressed, live input through the chain), so the row
  takes a discard release and the monitor pass processes it the **same block** when the
  worker is idle, at most one block later (`Abandoning`, counted) when the worker was
  mid-segment. v1 drained instead, which over-played ≤ depth clip blocks the direct path
  would have suppressed and delayed input onset by the same amount — v2 removes both.
  The residue is the standard bounded state lead (≤ depth segments of clip material in the
  chain's tails at monitor onset), documented, counted.
* **If recording starts on a row that is somehow still owned** (armed during the same block,
  degenerate), the row takes the same immediate discard release, so the recording track's
  clip-playback omission applies from the first recording block.
* Monitor/record commands and runtime status agree: monitored rows are excluded from
  adoption by the same monitor view the engine's own gates use, in the same block order
  (owned-check before monitor-check everywhere).

## 6. Control changes while ahead — the explicit late-apply window

* **Applied at consume time (immediate, unchanged):** routing destination, sends levels,
  meters — the ring holds post-strip/pre-fan data, fanned with the current routing plan.
* **Applied late, by up to depth segments (~depth × blockSize samples; ~32 ms at default
  depth 3, 512 @ 48 kHz):** fader, pre-gain, pan, mute, solo, plugin parameter changes, and
  clip edits on adopted rows. **This is a real user-facing decision** (delayed control
  response on adopted rows while the experimental flag is on), stated here rather than snuck
  in, and it is *not* a final approval of such response for a production version. The
  alternative — discarding the queue on every control change — re-processes samples through
  stateful plugins; worse.
* **Insert chain edits** (add/remove/swap on an adopted row) apply late by the same bound.
  Lifetime is handled by publish-before-destroy plus a worker pause window (§8); ownership
  itself survives the edit (the queued old-chain segments play out, the worker continues
  with the newly published map — remaining instances keep contiguous streams).

## 7. Queue miss, recovery, and leaving the mode

A miss (the audible stream needs a segment the ring cannot supply) contributes **silence for
that row for that segment** — never a wait, never audio from a wrong position (exact-key
consume), never concurrent processing. Every miss is counted. What happens next:

* **Single miss:** ownership is retained; the consumer's expected sequence number advances,
  so the late result is discarded as stale when it appears and the stream stays aligned. A
  transient hiccup costs exactly the missed segments, then ring hits resume.
* **Two consecutive missed segments on a row = the mode does not hold:** the row leaves
  read-ahead (discard release, counted separately as a miss abandonment) and renders live
  again from the next block — correct position, single owner, no hidden run of silent
  blocks. The instance's fed stream has a bounded gap (≤ the missed segments) at the point
  the audible stream already went silent; no additional audible artifact is introduced by
  leaving.
* **Re-adoption cooldown** (~64 blocks) prevents a struggling worker from flapping between
  adopt and abandon.
* Real overload is not hidden and not promised away: a genuine underrun is counted and the
  recovery above is the defined behavior. Transition-created gaps (none are expected: all
  supported transitions are gapless or discard-based) would show up in the same counters and
  in the focused tests, which assert zero misses across every supported transition.

Diagnostics stay internal (renderer counters, test/log access only) — no counters in the
user interface.

## 8. Lifetime and exclusive access

* **Publish-before-destroy + worker pause:** `PluginInsertHost` publishes the new map, then
  runs the drain hook (production `Main.cpp` and the test harness): wait out the in-flight
  callback, pause the read-ahead worker until acknowledged (the worker holds no chain, map
  or snapshot while paused), resume. Retired instances are destroyed only after the hook, and
  the worker re-acquires the newly published map fresh per segment, so it can never touch a
  destroyed instance. Ownership is **not** reset by chain edits (v1 full-reset here; v2
  keeps the queue — see §6).
* **Offline export:** raising the offline gate pauses the worker and requests a full reset;
  the gate's early return keeps every callback away from the chains until it drops.
* **Device stop/start:** stop pauses the worker and hard-resets all ownership before the
  host releases resources; start re-prepares buffers and resumes.
* **Structure changes:** the worker verifies per segment that the adopted track index still
  resolves to the adopted TrackId and self-stops otherwise; a row whose consume disappears
  (removed from the routing plan) is released within one block by the consume-absence check.
* **Global transport-context writes** exclude owned rows' chain playheads
  (`audioThread_setProcessTransportContextExcept`); each owned playhead has exactly one
  writer (the worker).

## 9. Save / Save As / autosave — the state-capture window

All three paths (`ProjectIoCoordinator` → `Session::saveProjectToFile` → `exportChain` →
`getStateInformation`) are bracketed by the engine's **plugin-state capture window**:

* **During playback:** the window requests a gapless draining release of every owned row and
  waits (bounded, message thread) until none is owned, then pauses the worker. Capture then
  reads `getStateInformation` from instances that are processed by the audio callback only,
  **exactly at the audible position** — the same concurrency class and the same state
  meaning as an A1/A2 save during playback today. The drain is inaudible (that is its
  defining property) and adoption resumes after the window closes.
* **While paused/stopped:** nothing consumes, so no drain is attempted; the worker is paused
  (acknowledged) and capture reads instances that nobody is processing. The captured state
  corresponds to the end of the instance's contiguous fed stream, which may be ≤ depth
  segments past the paused playhead — a defined, bounded, documented capture point (generic
  plugin state is parameters/preset data; DSP tails are not restorable sample-exactly by any
  host path, including A1/A2's).
* Captured **parameters are always current** — rendered-but-unplayed segments hold old
  parameter *audio*, never the saved state; nothing stale is serialized from prepared jobs.
* Unavailable-plugin placeholders re-emit their saved blobs byte-for-byte, unchanged.
* An explicit Save reports completion only after the file is written (unchanged); autosave
  uses the same bounded window — no audible stops, no unbounded callback waits (the window
  never blocks the audio thread; it only gates adoption and waits on its own message
  thread, capped).

## 10. What the counters mean (internal only)

`adopted`, `producedSegments`, `consumedSegments`, `missedSegments`, `staleDiscarded`,
`drainReleases`, `discardResets`, `missAbandons`. Accounting identity on owned segments:
every audible segment of an owned row is exactly one hit or one miss. **That identity alone
does not prove correct audio** — the focused tests separately verify (1) which samples each
instance processed (position-recording state-dependent probes), (2) which rendered segments
were actually consumed and bit-compare the output against the direct path, and (3) what was
discarded or missed. "Exactly once" is claimed only for stable playback and the transitions
§3–§5 list as gapless; deliberate discontinuities are claimed only as *bounded and counted*.

## 11. Verification scope honesty

The focused tests (`ReadAheadPrototypeFocusedTests`) drive the production callback with
deterministic, state-dependent test inserts (feedback state + per-call position/size
recording + state serialization that embeds the probe's processed-sample count). Linux
results verify **logic, audio data and lifetime** only: bit-identity with the direct path
(linear and cycle), pause/resume continuation, exactly-once streams, the save capture
window, miss/abandon recovery, monitor/record handover and chain-edit lifetime. They say
**nothing** about Windows/ASIO performance, real third-party plugins, or audible quality.
