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
> rendered output was ultimately played. Transitions either preserve this (adoption that
> commits only once the first ahead segment is queued, gapless drain, pause/resume
> continuation) or are deliberate discontinuities with a bounded, counted, documented state
> residue (seek, locator/cycle edits, monitor/record handover, miss abandonment).

**Priming.** Offering a row does not by itself take it off the direct path. The adoption
block still renders live. The worker may start only after that block's join, and its first
segment is the next block. At the next block begin:

* the row becomes Ahead only when that segment is already queued (or the worker has already
  fed the instance, so live-rendering it would play from the wrong time);
* if the ring is empty and the worker has not entered the chain, the prime is **declined**:
  this block renders live, nothing is counted as a miss, and the row may be offered again
  later. Adoption must not invent a gap just because the worker has not delivered yet;
* if the worker is already inside the plugin, the callback does not render that instance and
  does not wait. A segment that lands before the strip pass is played and the row becomes
  Ahead. A segment that is still absent is a counted miss — the same miss policy as any
  other owned row, not a second owner and not hidden silence.

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
is still inside a render is `Abandoning` (silent for that row, counted, never processed
concurrently or live). Three moments are distinct and only the first is block-bounded: the
handover is *requested* at a block begin; the worker *releases* when its in-flight plugin
call returns and the stop is observed; direct rendering *resumes* at the first block begin
after that observation. The release is typically well under one block but is bounded only by
the plugin's `processBlock` duration — **no fixed block count is guaranteed** for the
`Abandoning` span.

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
  worker is idle. When the worker was mid-segment on that row, the row is `Abandoning`
  (silent, counted) until the in-flight plugin call returns — usually the next block, but
  per §3 that span has **no guaranteed block bound**. v1 drained instead, which over-played
  ≤ depth clip blocks the direct path would have suppressed and delayed input onset by the
  same amount — v2 removes both. The residue is the standard bounded state lead (≤ depth
  segments of clip material in the chain's tails at monitor onset), documented, counted.
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

A miss applies once a row is on the consume path (Ahead, or a prime whose worker has already
entered the chain and whose segment is still absent). A declined prime is not a miss: the
row never left the direct path. A miss (the audible stream needs a segment the ring cannot
supply) contributes **silence for that row for that segment** — never a wait, never audio
from a wrong position (exact-key consume), never concurrent processing. Every miss is
counted. What happens next:

* **Single miss:** ownership is retained; the consumer's expected sequence number advances,
  so the late result is discarded as stale when it appears and the stream stays aligned. A
  transient hiccup costs exactly the missed segments, then ring hits resume.
* **Two consecutive missed segments on a row = the mode does not hold:** the row leaves
  read-ahead (discard release, counted separately as a miss abandonment) and renders live
  again — correct position, single owner, no hidden run of silent blocks. The instance's fed
  stream has a bounded gap (≤ the missed segments) at the point the audible stream already
  went silent; no additional audible artifact is introduced by leaving. **The threshold does
  not bound the total silence by itself:** when the abandoning reset hits a worker that is
  still inside a render of that row, the row stays `Abandoning` (silent) until that plugin
  call returns (§3) — live rendering resumes at the first block begin after the release,
  typically immediately, but with no guaranteed block bound under a stalled plugin.
* **Re-adoption cooldown** (~64 blocks) prevents a struggling worker from flapping between
  adopt and abandon.
* Real overload is not hidden and not promised away: a genuine underrun is counted and the
  recovery above is the defined behavior. Transition-created gaps (none are expected: all
  supported transitions are gapless or discard-based) would show up in the same counters and
  in the focused tests, which assert zero misses across every supported transition.

Diagnostics stay internal (renderer counters, test/log access only) — no counters in the
user interface.

## 8. Lifetime and exclusive access

**The pause contract:** the worker pause is depth-counted with an epoch handshake. A pause
request succeeds **only on a real acknowledgment** — the worker observed the request while
provably outside every chain, map and snapshot, and stays parked for as long as the
requester holds its pause. A timeout confers **no exclusivity**: the request is withdrawn
and the caller must abort, defer, or keep holding resources until a real acknowledgment —
"proceed anyway" does not exist. Nested holders are independent: an aborted or finished
operation can never resume the worker under another operation still holding a pause. A
plugin call that never returns therefore stalls the *operation* (resources retained), never
the audio callback, and is never killed or overlapped.

* **Publish-before-destroy + worker pause:** `PluginInsertHost` publishes the new map, then
  runs the drain hook (production `Main.cpp` and the test harness): wait out the in-flight
  callback (the existing A1/A2 bounded callback drain, unchanged), then wait for an
  **acknowledged** worker pause — the hook does not return, and the retired instances are
  not destroyed, until the worker really let go. Between attempts the worker may start new
  segments; those acquire the newly published map fresh and cannot reach retired instances.
  Ownership is **not** reset by chain edits (v1 full-reset here; v2 keeps the queue — §6).
* **Offline export:** raising the offline gate waits for an acknowledged pause (held for the
  whole gate — the offline render processes the same chains on the message thread) and
  requests a full reset; the gate's early return keeps every callback away from the chains
  until it drops.
* **Device stop:** waits for an acknowledged pause before resetting ownership and before the
  host's `releaseResources` runs — a stuck worker render stalls device stop with resources
  retained (the counterpart of JUCE's own guarantee that no audio callback runs during
  teardown). **Device start:** requires the acknowledgment before resizing the worker's
  buffers; if it never comes, the buffers are left untouched and the renderer stays dormant
  for the device session (`prepared_ == false`: nothing is ever adopted) — it retries at the
  next device start. Shutdown joins the worker thread outright.
* **Structure changes:** the worker verifies per segment that the adopted track index still
  resolves to the adopted TrackId and self-stops otherwise; a row whose consume disappears
  (removed from the routing plan) is released within one block by the consume-absence check.
* **Global transport-context writes** exclude owned rows' chain playheads
  (`audioThread_setProcessTransportContextExcept`); each owned playhead has exactly one
  writer (the worker).

## 9. Save / Save As / autosave — the state-capture window

All three paths (`ProjectIoCoordinator` → `Session::saveProjectToFile` → `exportChain` →
`getStateInformation`) are bracketed by the engine's **plugin-state capture window**
(`ScopedPluginStateCaptureWindow`), whose result **gates the write**. The window separates
three different protections — only the first two are guarantees this feature can make:

1. **Concurrent-access protection (against the worker only):** a successful window holds an
   *acknowledged* worker pause — the new writer this feature introduced is provably outside
   every chain while `getStateInformation` runs. The **audio callback is NOT paused**: during
   a playing save the callback keeps processing the instances while they are serialized.
   That is exactly the pre-existing direct-path save concurrency class (A1/A2 today reads
   `getStateInformation` on the message thread while the callback runs `processBlock`); the
   window **restores** that class, it does not improve on it.
2. **Parameter revision:** captured parameters are always current — rendered-but-unplayed
   segments hold old parameter *audio*, never the saved state; nothing stale is serialized
   from prepared segments.
3. **Time-dependent DSP state:** after a successful drain there is **no read-ahead-induced
   state lead** — at the capture point the instance's processed history ends at the audibly
   consumed stream (and during a playing save it keeps advancing live while serialization
   runs, exactly as on the direct path). This is *not* a sample-exact DSP checkpoint; generic
   plugin state is parameters/preset data, and DSP tails are not restorable sample-exactly by
   any host path, A1/A2's included.

Window mechanics per transport state:

* **During playback:** adoption is gated off and every owned row takes a gapless draining
  release as consumption proceeds (bounded message-thread wait); then the acknowledged worker
  pause. The drain is inaudible — a successful window changes nothing audible.
* **While paused/stopped:** nothing consumes, so no drain is attempted (a Playing → Paused
  transition *during* the wait resolves to this branch instead of stalling); the worker pause
  is still required and acknowledged. The captured state corresponds to the end of the
  instance's contiguous fed stream, ≤ depth segments past the paused playhead — a defined,
  bounded, documented capture point.
* Unavailable-plugin placeholders re-emit their saved blobs byte-for-byte, unchanged.

**Failure contract (the window can fail):** when the drain cannot complete (e.g. the
transport claims Playing but no callbacks arrive — stalled or stopped device — or sustained
overload) or the worker never acknowledges its pause, `beginPluginStateCaptureWindow`
returns **false** after releasing every hold it took (no stuck adoption gate, no stuck
pause). Nothing is captured and nothing is written:

* **Explicit Save / Save As** fails with an error (the normal save-failure dialog), does
  **not** mark the project clean, and leaves the existing project file byte-for-byte
  untouched (the write is never started; the writer itself is atomic temp+move besides).
* **Autosave** is deferred: the failure leaves the dirty flag and any previous autosave
  untouched, and the existing autosave tick cadence retries later — no tight retry loop, no
  dialogs.
* The audio callback is never blocked by any of this; the window only gates adoption and
  waits on the message thread, capped.

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
recording + state serialization that embeds the probe's processed-sample count, plus a
holdable probe that parks *inside* `processBlock` under test control). Linux results verify
**logic, audio data and lifetime** only: bit-identity with the direct path (linear and
cycle), pause/resume continuation, exactly-once streams, the save capture window including
its failure contract (failed drain/pause ⇒ no capture, file untouched, dirty retained,
later retry succeeds), the pause acknowledgment contract under a deliberately blocked worker
(timeout ⇒ explicit failure and no exclusivity; late acknowledgment ⇒ clean recovery; chain
retire and device stop wait for the real release), miss/abandon recovery, monitor/record
handover and chain-edit lifetime. They say **nothing** about Windows/ASIO performance, real
third-party plugins, or audible quality.
