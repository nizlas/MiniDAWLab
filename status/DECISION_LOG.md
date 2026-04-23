# Decision Log

This file records small but important implementation decisions that are useful to preserve between phases.

It is not a full chat log and not a replacement for the steering documents.  
It exists to capture concrete decisions, rationale, and limits that may matter later.

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
