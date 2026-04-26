# Phase Plan

This document defines the planned implementation phases for MiniDAWLab.

This phase plan is expected to evolve as the project matures.

Near-term phases should be concrete and tightly constrained.  
Later phases may remain provisional until earlier phases have been implemented and validated.

The purpose of this document is to guide controlled evolution, not to pretend that all future design decisions are already known.

The goal of this plan is to keep work narrow, reviewable, and aligned with the steering documents.

Phases must not silently broaden scope.  
Before each phase, planning/gap analysis must be performed as defined in `docs/IMPLEMENTATION_GUIDE.md`.  
After each phase, validation must be performed against `docs/VALIDATION_CHECKLIST.md`.

## Phase Planning Rules

For every phase:

- define a narrow scope
- define explicit out-of-scope
- identify gaps, ambiguities, assumptions, risks, and likely technical debt
- resolve steering issues before implementation begins
- implement only after the phase is stable
- validate the result before moving on

Each phase should produce something with real value, but must remain small enough to review clearly.

## Phase 0 — Project Scaffold and Steering Foundation

### Goal

Create the repository structure and steering foundation so implementation can proceed under explicit control.

### In Scope

- `PROJECT_BRIEF.md`
- architecture principles
- implementation guide
- phase plan
- validation checklist
- Cursor skill / agent steering setup
- basic repository layout
- initial README

### Out of Scope

- JUCE integration
- build system setup
- audio device setup
- file loading
- playback
- waveform rendering
- transport implementation

### Expected Value

The repository becomes governable before code implementation begins.

### Key Questions

- Are the steering documents complete enough to constrain implementation?
- Is the architecture envelope explicit enough for Phase 1?
- Are there ambiguities that would cause the agent to invent architecture?

---

## Phase 1 — Single-Clip Timeline Playback Scaffold

### Goal

Implement one audio file on a timeline with:

- visible waveform
- play / stop / pause
- seek / playhead
- playback via audio device
- clean separation between UI, transport, and audio engine

### Architectural Interpretation

This phase must **not** be treated as “just a WAV player”.

It must be treated as:

**a small timeline/playback engine that currently happens to support one audio clip**

### In Scope

- basic app scaffold for a JUCE-based desktop app
- audio device initialization sufficient for playback
- load/open a single audio file
- a file loading / import component responsible for turning a file path
  into a loaded clip, separate from playback
- represent one clip in a timeline-oriented way
- simple waveform display
- transport controls: play / stop / pause
- playhead display
- seek behavior
- clear separation between:
  - UI
  - transport
  - playback/engine
  - file loading/import
  - waveform rendering
  - clip/session representation

### Out of Scope

- multiple clips
- multiple tracks
- recording
- editing tools beyond basic seek/playhead interaction
- mixer
- routing graph
- sends / buses
- MIDI
- plugin hosting
- background analysis pipelines
- advanced persistence
- undo/redo framework

### Phase 1 Playback Assumptions

For Phase 1 only, the following strategy choices are fixed:

- The loaded audio file is assumed to be short enough to be held fully
  decoded in memory. Streaming from disk is deferred.
- File loading is synchronous on the message thread. An asynchronous
  loading model is deferred.
- The loaded file's sample rate must match the active audio device sample
  rate. If it does not, the file is rejected with a user-visible error and
  the session state remains unchanged. Resampling is explicitly deferred
  and must not be introduced implicitly to make mismatched files play.
- The loaded file's channel count is used as-is; no upmix or downmix logic
  is introduced in Phase 1.

These are strategy choices, not architectural ones. They must be revisited
when a later phase changes the loading or playback path.

### Expected Value

A minimal but architecturally meaningful playback scaffold exists.  
The project can load one audio file, show it, and play it, while preserving a clean growth path.

### Phase 1 Required Planning Questions

Before implementation, explicitly answer:

1. What is the source of truth for transport state?
2. What owns the loaded clip or file model?
3. What is responsible for file loading/import?
4. What is responsible for playback behavior?
5. What is responsible for waveform generation/display?
6. What assumptions are being made because only one clip is supported?
7. Which design decisions are being deferred, and why is that safe?

### Phase 1 Required Outputs

After implementation, provide:

- a short flow map
- a responsibility map for new classes/files
- a brief “why this split” explanation
- a note on how the design supports the next likely phase

---

## Phase 2 — Multiple Clips on One Timeline

### Goal

Extend the design from one clip to multiple clips on a single session timeline without collapsing the architectural separation established in Phase 1.

### Agreed Phase 2 semantics (steering)

- **Placed clips:** more than one decoded clip may exist in session state; each has an explicit
  **start sample** on the **session timeline** (device samples, same rate rules as Phase 1).
- **Add placement:** the primary UI path adds a clip at the **current playhead** (read once
  from `Transport` on the non-realtime path at add time), not an arbitrary free placement UI.
- **Overlap:** clips may overlap in time. **Front-to-back** order is **owned by `Session`**
  (index 0 = front-most, “on top”); the UI is a read-only consumer of that order, not the
  source of truth.
- **Newest on top (Phase 2 default):** the clip added most recently is **front-most** for
  overlap resolution unless a later phase introduces reordering.
- **Playback (not mixing):** at each timeline instant, at most one clip is audible: the
  **front-most clip that covers** that instant (stacked-events style). Underlying clips are
  heard only where not covered. This is **not** summing overlapping audio.
- **Playhead meaning:** the transport playhead is **timeline-absolute** (sample index along
  the session timeline, from 0 to the end of the placed material), not an offset inside “the
  one clip” as in Phase 1.
- **Timeline length and end behavior:** `timelineLengthSamples` is **derived** (e.g. from the
  maximum of `startSample + numSamples` over placed clips). When playback reaches the end,
  behavior matches the Phase 1 **clamp-at-end** intent: **silence**, playhead does not advance
  past the end, consistent guidance for re-starting playback.
- **Threading:** session changes continue to be published to the audio path via an
  **immutable atomic snapshot** (generalizing the Phase 1 `shared_ptr` handoff), with **no
  mutex, allocation, or blocking** in the callback when reading that snapshot.

### In Scope

- more than one clip in session state with per-clip **placement** and **order** as above
- session-owned ordering and a coverage-based playback rule (front-most covers)
- transport and UI that treat the playhead as **timeline-absolute** and seek/click on that
  same axis
- UI sufficient to add clips, show **multiple** clips on a shared visual timeline, and keep
  transport ownership clear
- validation that the separation of UI, transport, engine, and session is preserved

### Out of Scope

- trim, split, fade, or full editing
- multiple tracks, mixer, routing graph, sends / buses, MIDI, recording, plugin hosting
- mixing overlapping clips as a **sum** (this phase is “top clip wins by region”, not a mixer)
- advanced editing toolset

For a possible late Phase 2 extension, minimal single-lane event interaction may be allowed if explicitly approved:

- **clip selection**
- **moving clips** on the existing single timeline (placement change only, no new editing tools)

**Steering: overlap order when a clip is moved (committed end-state only).**  
`Session` owns the ordered list of placed clips; index 0 = front-most. The rule applies only when a move is **committed** (e.g. pointer released with a new position). It does **not** depend on drag history, prior overlap sets, or intermediate positions.

Let `M` be the single clip that was moved. After the move is committed, let **N** be the set of *other* placed clips that **overlap** `M` in time (the committed end-state only).

- If **N** is **empty** — `M` overlaps **no** other clip — **promote `M` to index 0**: remove `M` from its current list position and re-insert it at the front; clips that were above `M` in the list shift down by one. There is still exactly one index 0.
- If **N** is **non-empty** — `M` still overlaps one or more other clips — **preserve `M`’s current ordinal** (its position in the list is unchanged by this move).

**Interaction constraints (non-negotiable for this rule):**
- **Selection** never changes front/back order.
- **Drag in-flight** never reorders; only the **committed** move end-state may change order.
- Only **`M`** is reordered; no other clip’s list position is changed by a move of `M`.

**Explicit non-goals** for this extension: multi-clip move, keyboard modifiers for bring-to-front, ripple, snapping, trim/split/fades, cross-track drag/drop, or broader DAW-style editing semantics.

**Gating:** implementing selection/move and applying this rule in code is a **separate, explicitly approved** step; this document does not by itself authorize that implementation.

**Minimal timeline ruler (seek affordance, optional late Phase 2 follow-up).**  
A thin **time bar** may sit **above** the single event lane. **Seek** (click and drag) uses only this bar: it maps x to the same session-timeline sample space as the playhead and calls `Transport::requestSeek`. The lane stays for **selection and clip move**; empty background in the lane **does not** seek. Ticks are **unlabeled** marks at **round seconds** of session time, with **adaptive density** so a long session stays readable. **Not in scope** for this strip: time labels, zoom, tempo/bar/beat, markers, loop/cycle, snapping, keyboard seek shortcuts, or a shared “timeline view model” beyond layout alignment.

### Expected Value

The architecture proves that Phase 1 was not a dead-end single-file design.

### Key Risks

- one-clip or clip-local playhead assumptions surviving into code paths that must be timeline-absolute
- **Session**-owned order silently migrating into UI paint order or the waveform as canonical
- summing overlaps by habit (DAW default) instead of the agreed **topmost-covers** rule
- transport truth duplicated or diverging as timeline length and seek clamps evolve
- premature generalization into full DAW subsystems

---

## Phase 3 — Multiple Tracks

### Goal

Introduce multiple tracks while preserving understandable state ownership and avoiding premature mixer/routing complexity.

### In Scope

- track-level session representation (`Track` / `TrackId` in the domain; `SessionSnapshot` as an ordered list of tracks)
- clip-to-track association (each track owns an ordered `PlacedClip` list; same within-track overlap rule as Phase 2)
- UI: one horizontal lane per track (stacked vertically), default track on launch, **Add track** control, new clips go to the **active** track (last track created)
- playback: **sum** across tracks of each lane’s Phase-2-style coverage (no cross-track “who wins” — both are audible; overlapping clips on the **same** track are still not summed)
- validation of scaling from single-track assumptions

### Out of Scope

- full mixer, per-track faders, meters, master bus processing
- sends / buses
- plugin hosting
- MIDI instrument hosting
- recording workflows
- complex editing tools
- trim, fade, clip gain (still out)

### Phase 3 late extension: cross-track clip drag

- **In scope:** drag an existing `PlacedClip` from one lane to another; commit on **mouse up**; session publishes **one** new snapshot. **No** track type / format compatibility — “valid target” is any lane in the `TrackLanesView` stack (geometric hit only).
- **Target ordering:** the dropped clip becomes **front-most (index 0)** on the destination track, matching the “newest = front” add-clip path.
- **Active track** (`Session::activeTrackId_`): **unchanged** by a cross-track drop. The user sets it with **Add track**, **Clear** (reset), or **`Session::setActiveTrack`** (e.g. header click; no snapshot publish).
- **Within-track** committed drag still uses `Session::moveClip` / `SessionSnapshot::withClipMoved` (end-state rule in that track only). **Cross-lane** commit uses `Session::moveClipToTrack` / `SessionSnapshot::withClipMovedToTrack` (separate, named API).
- **UI:** a translucent **ghost** appears **only** on the lane under the pointer while dragging **after** the movement threshold, except when that lane is the **source** (the source lane already shows the live drag preview; no duplicate ghost). **Outside every lane,** all ghosts are cleared; **invalid-drop** feedback uses a **non-default** cursor (implementation picks the closest JUCE `MouseCursor` to “not here” on the current platform) on the **source** component; the cursor is restored on re-entering any lane and unconditionally on `mouseUp`. Releasing the pointer **outside all lanes** is a **no-op** (no `Session` publish). **Not in scope:** undo/redo, multi-clip move, snap-to-grid, per-track “can accept clip” type checks.

### Phase 3 late extension: minimal track headers (left column)

- **In scope:** a **fixed-width header** to the **left** of each stacked lane, showing the track’s **name** (stored on **`Track`** in the domain; default `"Track <id>"` from **`Session`** when the track is created). **Active** track is **highlighted** in the header. **Click** on a header calls **`Session::setActiveTrack(TrackId)`** — this **may not** `atomic_store` a new `SessionSnapshot`; `activeTrackId_` remains **message-thread-only** state on **`Session`**. **Add clip** still targets the **active** track. **Ruler** is inset left by the same width as the header column so the ruler’s x ↔ sample map matches the **lane** strip only (not the header column).
- **Out of scope (headers step):** rename UI, faders, pan, mute, solo, meters, track delete, drop-on-header, any change to **playback** or to **cross-track / within-track** move semantics, putting **`activeTrackId_`** in **`SessionSnapshot`**.

### Phase 3 late extension: track reorder (header drag)

- **In scope:** **Drag** from a **track header** (not the event lane) to **reorder** the session’s `Track` list; **`Session::moveTrack`** + **`SessionSnapshot::withTrackReordered`** publish a new snapshot. **No** per-track `PlacedClip` change (each track’s clip list and name are **unchanged**). A single **insert line** is drawn in **`TrackLanesView::paintOverChildren`** only (green = real reorder, red = no-op, nothing in invalid area). **Invalid** = pointer **outside** `TrackLanesView` or **x ≥ header width** (lane / event area); **forbidden** cursor (shared with invalid cross-lane **clip** drop) on the **source** header. **`activeTrackId_` is not written** on reorder; the highlight follows that **id** to its new row. **No** ghost track, no mixer, no keyboard reorder.
- **Out of scope (reorder step):** rename, delete, resize, collapse, mute, solo, faders, clip drag changes, `Transport` or playback changes, persistence.

### Phase 3 late extension: non-destructive right-edge trim

- **Model:** `PlacedClip` carries a **visible / effective length** (placement window on existing PCM). `AudioClip` is never shortened; trim is not split/cut and does **not** reorder clips in a lane (dedicated `SessionSnapshot::withClipRightEdgeTrimmed` + `Session::setClipRightEdgeVisibleLength`).
- **Playback / overlap / timeline length** use **`getEffectiveLengthSamples()`**; material length remains for diagnostics and reveal.
- **UI:** narrow right-edge handle on `ClipWaveformView` when the event is wide enough; separate pointer mode from move / cross-lane drop.
- **Project file:** v2 field `visibleLengthSamples` on each clip (0 = full; omitted in JSON when full on save); readers accept **v1** and **v2**.

### Phase 3 late extension: visible timeline span (UI viewport)

- **In scope:** A **message-thread-only** `TimelineViewportModel` holds a grow-only `visibleEndSamples` used as the x ↔ session-sample **denominator** for `TimelineRulerView` and `ClipWaveformView` (inset lane strip), so trimming or shrinking the **derived** session length does not rescale the world. The left edge is fixed at **0**; the span **auto-expands** when the derived extent grows (add clip, move that extends, load); it **does not** auto-shrink when the logical end shrinks (right-edge trim) — a future explicit zoom/scroll step may allow shrinking. **Seek** still maps the full ruler width, then `Transport::requestSeek` receives the target **clamped to** `Session::getTimelineLengthSamples()` (derived logical end). `Transport` and playback remain unchanged. Not persisted in `.mdlproj`.
- **Out of scope:** left-edge offset, zoom controls, scroll bars, “fit to content”, persisting the viewport, any change to `SessionSnapshot` for this, any audio-thread or engine work. **Superseded in part** by the arrangement extent / pan step below: grow-only `visibleEnd` is replaced by a stored extent on the snapshot, and the viewport is `visibleStart` + `visibleLength` with wheel pan.

### Phase 3 late extension: arrangement extent + pannable viewport (`.mdlproj` v3)

- **Model:** `SessionSnapshot` stores **`arrangementExtentSamples`** (read / propagated by all snapshot factories; effective extent = `max(stored, derived content end)`). **Content end** = `getDerivedTimelineLengthSamples()`. `Session::setArrangementExtentSamples` publishes `withArrangementExtent` (grow-only on **stored** value). `PlaybackEngine` uses `getArrangementExtentSamples()` for the play run-end so gaps and “past last clip but before extent” render **silence** and playhead still advances. **Not** a UI-only line anymore.
- **UI:** `TimelineViewportModel` holds `visibleStartSamples_` + **`samplesPerPixel_`** (zoom scale). The visible “length” in samples is always **derived** as `round(timelineWidthPx * samplesPerPixel)` — it is not stored, so **resizing the window** shows more or less time at the **same** scale (events do not stretch). On first `sync` with no material and no stored extent, `Main` seeds default arrangement to **1 hour** (`3600s` in samples) and `setSamplesPerPixelIfUnset(sampleRate / 10)` (default **10 pixels per second** of session time). The **only** paths that set `samplesPerPixel_` after that are the one-time seed and **`zoomAroundSample`**: `Ctrl+wheel` zoom is **mouse-centered** with `s = visStart + round(xPx * spp)` and `spp` clamped to `[0.1, ext/width]`. **Plain** wheel pans by `round((width/8) * samplesPerPixel)` samples; in-flight move/trim on a lane still blocks wheel on the stack. Ruler and lanes use **identical** `xToSessionSampleClamped` / `sessionSampleToLocalX` in `TimelineRulerView` over the same timeline width (lane subtracts the track header). Ticks use **pixels per second** = `sampleRate / samplesPerPixel` (tick **count** changes with width; **step** only changes with zoom). Playhead/seek **clamp to arrangement extent**; playhead is drawn only when inside the visible window.
- **Project file:** **v3** adds optional `arrangementExtentSamples` (save writes **effective** extent); readers still accept v1 / v2 (extent 0, floor from content only).

### Phase 3 late extension: minimal project save / load (`.mdlproj` v1 / v2 / v3)

- **In scope:** JSON on disk with tracks (id, name), placed clips (id, `startSample` on the session timeline, **absolute** `sourcePath` from `AudioClip::getSourceFilePath`), optional per-clip **`visibleLengthSamples`** in **v2+** (see right-edge trim extension), `nextPlacedClipId` / `nextTrackId` / `activeTrackId`, `playheadSamples` and `deviceSampleRateAtSave` as transport/session metadata, optional top-level **`arrangementExtentSamples`** in **v3** (arrangement extent step). **Save** writes **v3**; **read** accepts **v1**, **v2**, and **v3**. **Save** and **load** are **message-thread** operations in `io/ProjectFile` + `Session::saveProjectToFile` / `loadProjectFromFile`.
- **Load path:** build a full `std::vector<Track>` and publish **one** `SessionSnapshot` via a single `atomic_store` — **not** by chaining `addTrack` / `addClipFromFileAtPlayhead`. `SessionSnapshot::withTracks` is the load-only constructor path.
- **Transport:** `Transport` remains the sole owner of playhead state; after a successful load, the UI calls `transport.requestSeek` with the saved value **clamped** to the loaded session’s derived extent. **No** playhead in the audio snapshot.
- **Partial load:** missing files or decode/rate failures skip that clip, append a line to a user-visible list (path + reason), and still apply the new snapshot if JSON parse and structure were valid. **Invalid JSON or wrong version: no** session change.
- **Id seeds after load:** `max(storedNext, maxIdInFile + 1)` for placed clips and tracks so monotonic ids never collide with restored rows.
- **Out of scope:** a full DAW project format, relative/media paths, async decode, embedded audio, or undo.

### Expected Value

The project begins to resemble a real timeline-based audio application rather than a clip demo.

### Key Risks

- introducing track abstractions too early or too heavily
- mixing transport, track state, and playback execution in unclear ways
- hidden future mixer architecture sneaking in without documentation

---
## Phase 4 — Simple Audio Recording

**Phase order:** This phase is **before** [Phase 5 — Basic Mixer Direction](#phase-5--basic-mixer-direction). Recording and capture are validated on the existing timeline and snapshot model before introducing mixer-facing structure.

### Goal

Add the first minimal **mono** audio **input** recording while preserving the existing timeline, `Session` / `SessionSnapshot` immutability, and **realtime-safe** audio-callback rules.

The goal is not a Cubase-like recording system, but a narrow slice: arm a track, start/stop from **numpad `*`**, write a take to disk, and commit a normal `PlacedClip` on the **armed** track at the playhead, with a **UI-only** growing waveform preview that disappears after commit.

### In Scope

- **Track header UI:** a record-arm control (e.g. **R**), plus a simple **audio** track type indicator (icon); track type may remain “audio” only for this slice. **Add / select active track** behavior stays consistent with the current `Session` model (`getActiveTrackId` / `setActiveTrack` for add-clip target; arming is separate and may be a single **armed** track for recording only).
- **One armed recording track** (initially): arming a second track clears the first.
- **Mono** recording from the **first / default** input channel only (input **0** in device buffer terms for this slice).
- **Numpad `*`** to **toggle** record run: see **Command and Transport coordination** below.
- **Recording start time** = current **playhead** (session sample) at the moment a successful `beginRecording` is accepted, on the **armed** track.
- **Takes** written under **`<projectDir>/takes/`** as finalized **WAV** (or the project’s chosen lossless container), with **absolute** `sourcePath` on the new clip (same persistence story as other clips). **Take files:** mono **24-bit linear PCM** at the project/device sample rate (typical **48 kHz**; no 16-bit fallback in `RecorderService`).
- **On successful stop/finalize:** a **`RecordedTakeResult`**-style outcome (message-thread struct; exact name in code TBD) holds: finalized file, target **TrackId**, `recordingStartSample`, **intended** sample count, sample rate, and **dropped / overrun** sample count. Only **after** the take file is finalized on disk: create the `PlacedClip` (L = 0, visible length = recorded sample count) and **publish** a new `SessionSnapshot` via the existing atomic handoff. **Placed clip** = normal timeline clip; playback uses the existing engine path.
- **Temporary** realtime **min/max (peak) preview** for the in-flight take: **one consumer** (see **Preview peaks**); **not** a row in `SessionSnapshot`; same **x↔session-sample** mapping as [TimelineRulerView](src/ui/TimelineRulerView.h) and committed clips; may **overlap** existing events visually; does **not** change existing clips in the model.
- **Explicit** failure or empty-input handling in product terms where reasonable (e.g. no input device, zero channels) — *implementation detail* may still produce a valid silent take of the intended length if the device opens.

### Out of Scope (this slice; do not smuggle in)

- **Mixer, pan, sends, groups, routing matrix**, or **Basic Mixer** (Phase 5) work.
- **Split / cut, fades, crossfades, comping, take lanes, punch** overwrite, destructive overwrite of existing clips, **mute in Session** for “recording mutes track”, or any **new** overlap policy.
- **Stereo / multichannel** record, **multiple** simultaneous armed tracks, **input routing UI**, full **device/settings** dialog, **software/direct monitoring**, **sample-rate conversion**, **latency compensation** (see **Deferred** for where latency will matter later).
- **Plugin** processing on the record path.
- **Orphan** take files in a global app directory when the project is **unsaved** (see **Project file precondition**).

### Command and Transport coordination (root / main composition)

- **Root or main content component** (composition root) **coordinates** numpad `*`, **Transport**, and the recording service. It is the only place that sequences **read playhead** → **begin** → **set intent** to **Playing** (after successful begin), and **stop intent** → **stop/finalize** → **Session publish**.
- A **`RecorderService`** (or equivalent name) may **own** arm state, SPSC ring(s), background writer, and take file I/O, but **must not** call `Transport` directly and **must not** own **transport semantics** (play/stop/seek are not embedded inside the recorder for this slice).
- Suggested start sequence when **armed** and **not** recording, and the project is saved to a **known path** (see preconditions):
  1. Read `Transport::readPlayheadSamplesForUi()` for `recordingStartSample`.
  2. `beginRecording(...)` on the message thread. If it **fails**, do **not** change transport, do not create a clip, do not write orphan files.
  3. If `beginRecording` **succeeds**, then `Transport::requestPlaybackIntent(Playing)` (or equivalent) so the playhead advances with capture (aligned with the approved “auto-start playback on record” product choice for this project).
- Suggested **stop** sequence:
  1. `Transport::requestPlaybackIntent(Stopped)` (or **Paused** only if the project explicitly standardizes on that — default here is **Stopped** to match a clear “record pass ended”).
  2. `recording_` / equivalent cleared so the **next** audio callback no longer **pushes** new input to the SPSC path.
  3. **Signal** writer, **drain** FIFO and any **silence** segments written for overruns, **flush/close** WAV, **join** writer.
  4. Assemble **RecordedTakeResult** (file on disk, track id, start sample, **intended** length, `sr`, `droppedSamples`).
  5. **Only then** `Session::addRecordedTake` / `addClip...` (exact API TBD) to decode + publish. **If** `droppedSamples > 0`, show a user-visible **warning** (e.g. “Recording overrun: N samples were replaced with silence.”).
- The exact class names and method names are **implementation** details; the **order** and **invariants** above are **steering**.

### Realtime SPSC path (audio callback)

- The **push** from `PlaybackEngine::audioDeviceIOCallback...` (or a thin helper) into the recorder **must** follow a **realtime-safe, single-producer / single-consumer (SPSC)**-style contract for the work done **in the callback**:
  - **No** `Session` or `SessionSnapshot` access
  - **No** **locks**, **allocations**, **blocking**, **waits** (except a named, documented `signal` that is provably non-blocking on supported platforms)
  - **No** direct **UI** mutation
- A **separate** non-realtime thread (writer) may own `juce::AudioFormatWriter`, disk, and `Session` is **not** touched from the audio thread.

### FIFO capacity and overrun (steering, before coding)

- **Initial** mono **sample** ring capacity: **next power of two** **≥** `currentDeviceSampleRate * 5` (≈ **5 s** of audio at the device rate).
- If a callback block **does not** fully fit: **never block** the audio thread. **Write** the prefix that **fits**; the remainder is counted as **dropped/overrun** samples.
- **Intended** timeline duration: the take’s **length in samples** (and any UI counter of “how long you’ve been recording”) **includes** the full block’s logical length; **dropped** samples are **not** a silent **shorten** of the file.
- **Missing** samples are represented in the final WAV (or in the result metadata) as **equivalent silence** (or a single **corrupted/flag** field while **preserving** duration — default steering is **silence** for simplicity). **After stop**, surface a **warning** if any samples were filled as silence due to overrun.
- Maintain **`droppedSampleCount`** (or equivalent) **separate** from the **intended** sample count.

### Stop / finalize and `RecordedTakeResult` (invariants)

On stop, the implementation **must** deliver a complete **RecordedTakeResult** (or equivalent) containing at least:

- Finalized take **file** path / `juce::File`
- **Target** `TrackId` (armed track at record start, unless explicitly revised in a later doc)
- **`recordingStartSample`**
- **Intended** recorded sample count (including silence substituted for overruns)
- **Sample rate** used for the take
- **`droppedSampleCount`** (0 if clean)

**Session** snapshot with the new `PlacedClip` is published **only** after the take file is **closed** and **available** for the same `AudioFileLoader` path used by other clips.

### Preview peaks (single owner)

- **Only one** place may **drain** preview peak / min-max data per tick (e.g. `TrackLanesView` timer or a single “recording overlay” owner). **Only** the **recording** lane (or a dedicated sub-view for that track) should receive drained blocks. **Non-recording** `ClipWaveformView` instances must **not** call `drain…` in parallel (no races).

### Non-destructive recording onto a non-empty track

- **Do not** as part of this slice: move, split, trim, delete, mute, overwrite, or add take lanes. The new clip is **appended** in product terms as a **new** `PlacedClip` on the **armed** track at `recordingStartSample` with a length of **N** intended samples. If it **overlaps** existing clips in time, **existing** front-to-back / topmost-wins overlap semantics (as in Phases 2–3) **apply unchanged**; no recording-specific override.

### Engine behavior during active recording (transient; not Session)

- While **recording is active** (atomic flag in engine-readable view), the **armed / recording** track is **excluded** from **playback** mixing in `PlaybackEngine` (skip that track’s clip contributions). **Other** tracks play **unchanged**. **Existing** clips on the recording **track** remain **visible** in the UI (read from the published snapshot) but **no audio** is mixed from that track for the **duration of recording** only. This is **not** a **mute** written into `Session` or `SessionSnapshot` and must not persist after stop.

### Device / input scope (implementation)

- **Mono**; **one** input channel; **no** input routing UI.
- When the device is opened in code, **1 input, 2 outputs** (e.g. `initialiseWithDefaultDevices(1, 2)`) is the **intended** change — **in implementation**, not a steering rewrite of the whole device model.
- **No** new full **audio settings** window for this slice. **No SRC**; record at the **current** device / project **sample rate** as today’s loader and engine assume.

### Unsaved project

- If there is **no** saved project file (path **unknown**): **refuse** to start recording with a **visible** message (e.g. *“Save the project before recording.”*). **Do not** start transport, **do not** create **orphan** WAVs in a global store, **do not** create an **empty** committed `PlacedClip`.
- If the project is saved, takes live under **`<parentOfProjectFile>/takes/`** (or the agreed `takes/` subfolder next to the `.mdlproj` / project file on disk — exact layout is implementation, **absolute** paths on clips remain the persistence rule).

### Deferred (document for later; not in this slice)

- **Input/output latency** and **round-trip** alignment: the natural hook is the **input push** in the audio callback and the **playhead** position; future compensation would adjust **start position** and/or **written** data using `juce::AudioIODevice` latency getters — **not** implemented here.
- **Software monitoring** / direct monitoring, **meters** as a first-class bus (meter-only UI is optional later).

### Expected Value

The project gains a first **real** record → commit → play loop while keeping **snapshot immutability** and a **verifiable** realtime path. The phase is intentionally narrow enough to validate **file lifetime**, **overrun** reporting, and **transient** engine exclusion **before** Phase 5 (Basic Mixer Direction) mixer work.

---

## Phase 5 — Basic Mixer Direction

### Preconditions

- Basic playback and editing are stable.
- Simple mono recording is implemented without violating snapshot immutability or audio-thread safety.
- Recorded clips behave like normal timeline clips after being committed.

### Goal

Add a minimal mixer-facing structure appropriate to current project needs.

### In Scope

- simple level-control concepts where justified
- minimal, explicit groundwork for future routing
- documented decisions about what mixer concepts exist now versus later

### Out of Scope

- full Cubase-like mixer
- large routing matrix
- advanced automation
- broad plugin support
- generalized audio graph architecture unless explicitly approved

### Expected Value

The project gains a controlled first step toward practical mix behavior without exploding in scope.

---

## Phase 6 — Routing, Sends, and Groups

### Preconditions

- Basic mixer-facing concepts exist.
- Recording remains separated from routing complexity unless explicitly approved.

### Goal

Introduce routing concepts only when earlier phases are stable enough to justify them.

### In Scope

- minimal routing extensions explicitly approved in steering docs
- shared FX/send concepts
- group-bus concepts
- validation that routing does not collapse transport/session clarity

### Out of Scope

- large general routing framework
- arbitrary graph systems unless explicitly documented and approved
- broad plugin hosting ecosystem

### Expected Value

The architecture begins to support the real workflow needs described in the brief.

---

## Phase 7 — MIDI Clips and Piano-Roll Direction

### Goal

Add the first MIDI-facing concepts once the audio playback/timeline foundation is stable enough.

### In Scope

- MIDI clip representation
- initial piano-roll direction
- architecture updates explicitly documented before implementation
- validation that MIDI is integrated without breaking playback structure

### Out of Scope

- full instrument ecosystem
- broad DAW feature parity
- notation/score
- advanced MIDI tooling unless explicitly scoped

### Expected Value

The project begins to support melody prototyping, one of the core workflow goals from the brief.

---

## General Phase Advancement Rule

A phase must not automatically lead to the next phase just because implementation “seems to work”.

Before moving on, ask:

1. Is the current phase actually aligned with the steering documents?
2. What is still weak, risky, or provisional?
3. What architectural pressure did this phase reveal?
4. Do any steering documents need to be updated before continuing?
5. Is the next phase still the right next phase?

If the answer to any of these is unclear, stop and resolve the documents first.

## Explicit Anti-Drift Rule

If implementation reveals a tempting shortcut toward a broader DAW design, do not follow that shortcut automatically.

Instead:

- identify it explicitly
- explain why it is tempting
- explain the architectural consequences
- decide in documents before implementing it

The project should grow by explicit decisions, not by accidental drift.
