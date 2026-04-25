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
- **Active track** (`Session::activeTrackId_`): **unchanged** by a cross-track drop; only **Add track** and **Clear** reset it.
- **Within-track** committed drag still uses `Session::moveClip` / `SessionSnapshot::withClipMoved` (end-state rule in that track only). **Cross-lane** commit uses `Session::moveClipToTrack` / `SessionSnapshot::withClipMovedToTrack` (separate, named API).
- **UI:** a translucent **ghost** appears **only** on the lane under the pointer while dragging **after** the movement threshold, except when that lane is the **source** (the source lane already shows the live drag preview; no duplicate ghost). **Outside every lane,** all ghosts are cleared; **invalid-drop** feedback uses a **non-default** cursor (implementation picks the closest JUCE `MouseCursor` to “not here” on the current platform) on the **source** component; the cursor is restored on re-entering any lane and unconditionally on `mouseUp`. Releasing the pointer **outside all lanes** is a **no-op** (no `Session` publish). **Not in scope:** undo/redo, multi-clip move, snap-to-grid, per-track “can accept clip” type checks.

### Expected Value

The project begins to resemble a real timeline-based audio application rather than a clip demo.

### Key Risks

- introducing track abstractions too early or too heavily
- mixing transport, track state, and playback execution in unclear ways
- hidden future mixer architecture sneaking in without documentation

---

## Phase 4 — Basic Mixer Direction

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

## Phase 5 — Routing, Sends, and Groups

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

## Phase 6 — MIDI Clips and Piano-Roll Direction

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
