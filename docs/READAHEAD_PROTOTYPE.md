# Read-ahead prototype — ownership and transition model (Stage B prototype)

**Status: experimental, OFF by default.** Enabled only with the CLI flag
`--experimental-readahead` (optional depth: `--experimental-readahead=N`, N = 2..8 queued
blocks, default 3). Without the flag the engine contains **no** read-ahead objects and every
code path is byte-for-byte the A1/A2 behavior. This is DAL's own pre-processing of eligible
audio tracks; it is **not** an integration with Steinberg's ASIO-Guard.

This document describes the chosen ownership/transition model **before** the wiring, as the
model is what makes the prototype safe. The implementation lives in
`src/engine/ReadAheadRenderer.{h,cpp}` plus narrow integration points in `PlaybackEngine`,
`PluginInsertHost` and `Main`.

---

## 1. The core problem being solved

A plugin that has rendered future blocks has also advanced its internal DSP state (delay
lines, envelopes, oscillators). Discarding an audio buffer or waiting on a fence does **not**
rewind that state. Therefore the model is built around one invariant:

> **Exactly-once, contiguous sample stream per plugin instance.** Every insert instance on a
> read-ahead row processes the timeline samples in order, each exactly once, regardless of
> which thread rendered them or whether the rendered output was ultimately played.

Everything else (adoption, release, misses, invalidation) is derived from keeping this
invariant, or from documenting precisely where a transport discontinuity makes it impossible
and what the audible consequence is.

## 2. Scope (deliberately narrow)

* **Eligible rows:** audio tracks that Stage A1 would give a strip job (routing-plan audio
  source steps, not monitored), rendered **through the production strip core**
  (`renderAudioTrackPostStripToStereoScratchWithChainAccess`: clips → pre-gain → Pre inserts
  → fader/mute/solo → Post inserts → pan). At most 16 rows are adopted; further rows simply
  stay on the live A1 path.
* **Everything else stays on the existing paths:** instrument rows (A2 combined jobs),
  proxy-backed playback, group/master buses, sends-fan, monitoring, offline export, count-in,
  recording. The prototype never touches PDC, Mute/Off policy, proxy priority or automation.
* **Activation conditions per row (all required, checked every block):** flag on, transport
  Playing, A1 collect active, cycle OFF, no recording, playback offset unchanged and
  non-negative at the row's position, more than (depth + 2) blocks of arrangement left.
  Where a transition cannot yet be handled correctly, the condition set above *excludes* the
  situation instead (see §6).

## 3. Who owns an instance, and what its state means

Per adopted row there is a single-producer/single-consumer ring of `depth` slots. Each slot
is keyed by `(timelineStartSample, runLength, generation)` — a result is only ever consumed
if its key matches exactly what the callback needs **this** block; anything else is discarded
as stale (counted, never played).

Row ownership states (all transitions happen on the **audio callback thread** at block
begin, so the set is block-stable):

| State | Who may process the row's chain | Plugin state position |
|---|---|---|
| `Live` | audio callback (A1 job/serial path) | = transport position |
| `Scheduled` | audio callback (this block, last live block) | = transport position |
| `Ahead` | read-ahead worker only | **ahead of** transport by ≤ depth blocks |
| `Draining` | nobody produces; callback consumes the queue | ahead by (queued blocks) |
| `Abandoning` | worker is finishing its current block, then stops | ahead, results discarded |

* The **worker** is one dedicated thread, separate from the render pool that must meet the
  current audio deadline, and deliberately NOT elevated to the pool's pro-audio priority. It
  never blocks the callback and the callback never waits on it (a not-ready result is a
  *miss*, §7 — never a wait).
* A per-row busy/claim atomic plus a stop-request flag give the Dekker-style guarantee that
  worker and callback never process the same chain concurrently; the publish-before-destroy
  hook (§8) extends the same guarantee across plugin removal.
* The worker renders each block from a **fresh** session snapshot / solo view / insert map
  (acquire-loaded per block, exactly like the callback does), stamps the chain's own entry
  playhead (`audioThread_setEntryTransportContext`) with the block's true timeline position,
  and uses its own scratch buffers and a dedicated host processing lane (lane 16), so the
  existing 16 pool lanes and the callback lane are untouched.

## 4. Adoption (Live → Ahead) is gapless by construction

1. At block *t₀* the engine offers an eligible row; the row becomes `Scheduled` and **still
   renders live this block** through the normal A1 job.
2. After the block's job join (the callback's last touch of that chain), the engine publishes
   the live stream position *t₀+B*. Only after observing that publish may the worker render
   its first block, which is exactly *t₀+B*.
3. From block *t₀+B* on, the callback consumes from the ring. The instance's input stream is
   `…, t₀ (live), t₀+B (worker), t₀+2B (worker), …` — contiguous, exactly once. The queue
   then grows toward `depth` from real-time headroom.

If the first worker block is not ready in time that is a **miss** (§7), never a wait and
never a second render of the same samples.

## 5. Release transitions

Two flavors, chosen per cause:

**Draining release (gapless; used whenever the timeline continues linearly):** the worker is
told to stop producing; the callback keeps consuming the already-queued blocks; when the ring
is empty and the worker has acknowledged, the row returns to `Live` and the next block renders
on the callback again. The instance's stream stays contiguous and exactly-once — **no state
residue**. Used for: Monitor enabled on the row, recording starting, cycle being switched on,
approaching the arrangement end, track-list changes under the row, and any case where the
worker stops by itself (e.g. its snapshot no longer contains the row).

**Discard reset (used at real transport discontinuities):** seek, stop, cycle wrap reaching
the playhead, playback-offset change, forced invalidation. All queued results are discarded
(counted) and every row returns to `Live` immediately — via the claim atomic if the worker is
idle on that row, else through `Abandoning` (the row is silent for that block, counted, and
is live the next block). **Documented state residue:** the instance has already consumed up
to `depth` blocks beyond the discontinuity point. The residue rides the discontinuity the
transport jump already causes today (tails never match across a seek), with one honest
difference: *stopping and replaying from the same position re-feeds ≤ depth blocks that the
instance already processed* — e.g. a delay's feedback will briefly hear those samples twice.
Plugin state save/restore is **not** assumed to be able to rewind arbitrary plugins
sample-exactly, so no rewind is attempted. This is an accepted, documented artifact of the
experimental mode, not hidden error handling.

## 6. Control changes while ahead — an explicit decision, not a side effect

The worker reads a fresh session snapshot and solo view per rendered block. Consequences:

* **Applied at consume time (immediate, unchanged):** routing destination, sends levels,
  meters — the ring holds post-strip/pre-fan data and the callback fans it with the current
  routing plan.
* **Applied late, by up to `depth` blocks (~`depth`×blockSize samples):** fader, pre-gain,
  pan, mute, solo, and clip edits on adopted rows — queued blocks keep the values that were
  current when they were rendered. **This is a real user-facing decision** (delayed control
  response on adopted rows while the experimental flag is on), stated here rather than
  snuck in. If this is not acceptable, the alternative is discarding the queue on every
  control change, which re-processes samples through stateful plugins (§5 residue) — worse.
* **Insert chain edits** (add/remove/swap on an adopted row) also apply late by ≤ depth
  blocks; lifetime safety is separate and handled in §8. The real chain order is preserved —
  fader and pan stay **inside** the strip exactly as in the serial path; nothing was moved
  out to simplify invalidation.

## 7. A miss is measurable incompleteness, not finished error handling

If the callback needs block `(t, run)` from an adopted row and the ring cannot supply it,
the row contributes **silence for that block**. The miss is counted
(`ReadAheadRenderer` counters, queryable and logged), ownership is retained, and the late
result is discarded as stale when it appears — the worker's input stream stays contiguous, so
the plugin state remains aligned with the timeline and playback recovers cleanly. To be
explicit: **silence-at-miss is not transparent and is not presented as finished error
handling.** It is the honest, observable failure mode of this prototype; a production
version would need a deadline/fallback policy decided as a product question.

## 8. Lifetime and exclusive access

* **Publish-before-destroy:** `PluginInsertHost` publishes the new map, then runs the drain
  hook, then destroys removed instances. The hook (production `Main.cpp` and the test
  harness) already waits out the in-flight callback; with the flag on it additionally pauses
  the worker and waits for its acknowledgment (the worker holds a map reference only within
  one block render), requests a full discard reset, and resumes the worker — which then only
  ever sees the newly published map. The callback is never blocked by any of this.
* **Offline export:** raising the offline render gate pauses the worker the same way (the
  export processes the same live chains on the message thread); lowering it resumes. The
  callback's existing gate early-return is untouched.
* **Device stop/start, project switch:** device stop pauses the worker and hard-resets all
  ownership (no callback is running); device start re-prepares the ring buffers and resumes.
  Everything returns to safe direct processing.
* **Global transport-context writes:** the serial paths publish the block context to *every*
  chain playhead (`audioThread_setProcessTransportContext`), which in A1/A2 is safe purely by
  ordering (no job in flight). The worker runs across those points, so with any row adopted
  the engine uses a new `audioThread_setProcessTransportContextExcept` that skips the adopted
  rows' chains; those playheads are written only by their single owner.

## 9. Save / autosave while ahead (documented limitation)

Saving during playback serializes plugin state as it is *now*; with the flag on, an adopted
row's instances are up to `depth` blocks ahead of the audible position. The project file
does not capture a "different" chain or parameters — only the DSP-internal state (tails,
phases) is ahead, the same class of nondeterminism as saving during playback today, slightly
enlarged. This is documented, **not** silently redefined as correct; a production version
would quiesce read-ahead around state serialization.

## 10. What happens on cycle

Adoption is gated **off** while a valid cycle is active; enabling cycle mid-run starts a
draining release. If a wrap arrives before the drain finishes, the wrap block's segment keys
cannot match full-block queue entries: the row misses (silence, counted) for that block and
the wrap's t₀ discontinuity triggers the discard reset at the next block begin. Cycle
playback therefore always runs on the pure A1/A2 path.

## 11. Verification scope honesty

The focused tests (`ReadAheadPrototypeFocusedTests`) drive the production callback with a
deterministic, **state-dependent** test insert (one-pole feedback state plus per-call
playhead-position recording — misplaced internal time changes the output and the recorded
positions). Linux results verify **logic, audio data and lifetime** only: bit-identity with
the direct path during stable playback, exactly-once/contiguous streams across adoption and
draining release, miss/stale behavior with a deliberately delayed worker, and
publish-before-destroy under chain edits. They say **nothing** about Windows/ASIO
performance, real third-party plugins, or audible quality.
