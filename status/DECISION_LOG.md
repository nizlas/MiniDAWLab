# Decision Log

This file records small but important implementation decisions that are useful to preserve between phases.

It is not a full chat log and not a replacement for the steering documents.  
It exists to capture concrete decisions, rationale, and limits that may matter later.

---

## 2026-04-24 — Phase 2 move ordering on committed single-clip move (steering)

Decision (see `docs/PHASE_PLAN.md`, late Phase 2 extension):

- When a **single** placed clip `M` is **moved** and the move is **committed**, order is decided **only** from the **end-state** of the session: whether `M` overlaps any **other** placed clip after the new placement. No `N_before`, no per-drag state, no reorder during in-flight drag.
- If, after the commit, `M` overlaps **no** other clip: **promote `M` to index 0** (remove `M` from its current position, insert at front, shift others that were above `M` down by one).
- If, after the commit, `M` overlaps **one or more** other clips: **preserve `M`’s list ordinal** (no reorder from this move).
- **Selection** does not reorder. **Only** a committed move end-state may reorder, and **only** `M` is reordered.

Rationale:
- Aligns with reference DAW behavior (isolated release ≈ “reset to front”; overlapping release ≈ keep stack order) without session bookkeeping of drag path.
- Keeps a single, inspectable decision from one snapshot: overlap emptiness in the committed state.

Notes:
- Implementation (selection, drag, `Session`/`SessionSnapshot` move API) is **not** implied by this log entry; it requires an explicit follow-up approved step.
- “Newest on top” for **new** clips remains the separate default in `PHASE_PLAN.md`; this decision governs **moved** clips only, once that extension exists.

---

## 2026-04-24 — Phase 1 playback assumptions

Decision:
- Audio files are fully decoded into memory in Phase 1.
- File loading is synchronous on the message thread.
- Files whose sample rate does not match the active audio device sample rate are rejected.
- No implicit resampling is introduced.
- No general upmix/downmix logic is introduced in Phase 1.

Rationale:
- Keeps Phase 1 narrow and predictable.
- Avoids hiding resampling or loading complexity inside playback.
- Preserves clear separation between file loading and playback.

Notes:
- This is a Phase 1 strategy choice, not a permanent architectural commitment.
- Later phases may revisit loading and sample-rate handling explicitly.

---

## 2026-04-24 — Transport source of truth

Decision:
- `Transport` is the single source of truth for:
  - playback intent
  - authoritative playhead position
  - pending seek request

Rationale:
- Prevents duplication of transport state between UI, engine, and other layers.
- Preserves the steering rule that UI must not own playback state.

Notes:
- Playback intent is written from non-realtime code.
- The authoritative playhead is written only from the audio callback path.
- Seek is requested from non-realtime code and applied by the audio callback.

---

## 2026-04-24 — Phase 1 cross-thread transport model

Decision:
- UI/message-thread code reads transport state through read-only APIs.
- The audio callback is the only writer of the authoritative playhead.
- Seek requests are written on the message thread and consumed on the audio thread.
- The audio path uses a lock-free, non-blocking synchronization model.

Rationale:
- Keeps the transport contract explicit.
- Avoids mutexes and blocking synchronization on the audio path.
- Preserves a clean separation between UI interaction and realtime state mutation.

Notes:
- Exact low-level primitives are implementation details unless promoted to steering constraints later.

---

## 2026-04-24 — File loading/import is a distinct Phase 1 concept

Decision:
- File loading/import is treated as its own concept in Phase 1.
- `AudioFileLoader` is separate from playback, transport, and UI logic.

Rationale:
- Prevents file opening/decoding from becoming entangled with playback execution.
- Makes it easier to evolve clip/session behavior later without rewriting loading logic.

Notes:
- In Phase 1 this is intentionally a small concept, not a large subsystem.

---

## 2026-04-24 — Session owns the loaded clip

Decision:
- `Session` owns the currently loaded clip.
- Playback reads clip data from `Session`.
- UI and `PlaybackEngine` do not own clip loading state.

Rationale:
- Keeps clip ownership explicit.
- Avoids hidden ownership in the UI or playback layer.
- Preserves a path from one clip now to multiple clips later.

Notes:
- Phase 1 supports zero or one loaded clip.

---

## 2026-04-24 — Playback engine is the only audio callback object

Decision:
- `PlaybackEngine` is the only object registered as the audio callback.
- No parallel callback path or alternate playback object is introduced.

Rationale:
- Keeps audio-thread behavior centralized and reviewable.
- Prevents accidental duplication of playback logic.

Notes:
- Phase 1 introduced silence first, then real clip playback, while keeping the same ownership model.

---

## 2026-04-24 — Mono-to-stereo Phase 1 special case

Decision:
- If the loaded clip has exactly one channel and the output device has at least two output channels, the mono signal is duplicated to the first two outputs.
- Otherwise, the existing explicit Phase 1 channel policy remains unchanged.

Rationale:
- Makes common mono-file playback behave naturally on stereo devices.
- Keeps the change small and local without introducing general channel mixing.

Notes:
- This is an explicit Phase 1 exception, not a general-purpose upmix/downmix system.

---

## 2026-04-24 — Pause vs Stop behavior

Decision:
- Pause keeps the current playhead position.
- Stop sets playback intent to `Stopped` and requests seek to sample 0.

Rationale:
- Creates a clear and expected user-visible distinction between Pause and Stop.
- Reuses the existing transport request path instead of introducing new stop-specific state handling.

Notes:
- Stop does not unload the clip.
- If no clip is loaded, seek-to-zero is effectively a no-op.

---

## 2026-04-24 — Waveform rendering stays outside playback

Decision:
- Waveform rendering is derived from `Session` / `AudioClip` data.
- The waveform view does not read audio data from `PlaybackEngine`.
- The waveform view does not own transport state.

Rationale:
- Preserves separation between display and playback.
- Avoids accidental coupling between UI rendering and audio execution.

Notes:
- Phase 1 waveform rendering is simple and synchronous.
- This may be revisited later if performance or scale requires it.

---

## 2026-04-24 — Playhead overlay and click-to-seek use transport only

Decision:
- UI reads the playhead through a read-only transport API.
- Click-to-seek maps view position to sample position and calls `Transport::requestSeek(...)`.
- UI does not maintain its own authoritative playhead state.

Rationale:
- Preserves transport as the only source of truth.
- Keeps the waveform/playhead UI aligned with the same state the engine uses.

Notes:
- Phase 1 uses a simple full-width clip coordinate model.
- More advanced timeline semantics belong to later phases.
    
## 2026-04-25 — Phase 1 documentation retrofit completed

Decision:
- The existing Phase 1 codebase was retrofitted to the strengthened six-tier in-code documentation rubric.
- This included the clarified body-readability rule and the bounded readability-refactor allowance.
- The retrofit was completed before continuing with Phase 2 code work.

Rationale:
- The earlier documentation level was not sufficiently pedagogical for a reader browsing the codebase.
- File headers, class/method comments, and technical labels alone were not enough.
- Central method bodies also needed plain-language in-body explanatory comments where branch meaning would otherwise be hidden behind low-level mechanics.

Notes:
- The retrofit was intended to preserve behavior.
- The purpose was to improve pedagogical readability, not to change architecture or functionality.
- The strengthened documentation standard now serves as the baseline for subsequent Phase 2 implementation work.

## 2026-04-25 — Phase 2 resumed after documentation retrofit

Decision:
- Phase 2 planning remains the active next step after the completed Phase 1 documentation retrofit.
- The approved Phase 2 scope is still:
  - multiple clips on one timeline
  - explicit clip start positions
  - overlap allowed
  - front-most covering clip wins
  - add clip at current playhead
  - clamp at timeline end

Rationale:
- The documentation retrofit was completed to ensure the existing codebase is readable enough before building further on top of it.
- With that baseline in place, Phase 2 can now proceed under the strengthened documentation rubric.

Notes:
- Phase 2 implementation order remains the previously approved step sequence.
- No Phase 2 code changes were part of the documentation retrofit itself.
- The next executable Phase 2 step is the approved steering/document update for Phase 2 semantics before new code changes begin.

---

## 2026-04-23 — Phase 2 steering: timeline, overlap, and session ordering (docs)

Decision:

- The transport **playhead** is **timeline-absolute** in Phase 2: a single session timeline in
  device samples (same field as Phase 1; semantics extended), not a position inside a single
  loaded clip.
- **`Session` owns** the **ordered** list of placed clips and the **front-to-back** (overlap)
  order. The UI and waveform must not be the source of that order; they reflect `Session` only.
- **Newest added is front-most** in Phase 2: the most recently added clip is index 0 / on top
  for overlap, until a later phase adds explicit reordering.
- **Multiple clips** may **overlap** in time. **Audibility** is **not** a mix sum: at each
  instant, the **front-most clip that covers** that timeline position is the one heard there;
  other clips are audible only in **uncovered** regions (stacked-events behavior).
- **Add clip** uses the current playhead: placement **start sample** = playhead at add time
  (read on the non-realtime path as specified in the phase plan), not a separate placement UI.
- **End of session timeline** uses **derived** length from placements; at end, behavior stays
  aligned with the **clamp at end, silence, no spurious advance** product intent (generalized
  from single-clip end behavior).
- **Callback path** continues to use an **atomic immutable snapshot** of session state, not
  per-block heap allocation or mutexing for session reads, consistent with the Phase 1
  cross-thread model.

Rationale:

- Locks the previously approved Phase 2 plan into steering documents so implementation cannot
  drift (e.g. into implicit mixing, clip-local playhead, or UI-owned ordering) without a
  documented change.

Notes:

- Recorded in `docs/PHASE_PLAN.md` (Phase 2) and `docs/ARCHITECTURE_PRINCIPLES.md` (Phase 2
  subsection) as part of Phase 2 Step 3. No code changes in this step.