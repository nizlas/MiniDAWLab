# Architecture Principles

This document defines the architecture envelope for MiniDAWLab.

The agent is not allowed to invent architecture outside this envelope.  
It may only implement within the structure, constraints, and scope explicitly documented here and in the other steering documents.

## Core Principle

The agent is a constrained implementer, not architect-in-chief.

Architecture must be made explicit in documents before it is implemented in code.

If architectural gaps, ambiguities, or risks are discovered, the correct action is to update the steering documents first, then reassess implementation.

## Phase 1 Architectural Framing

Phase 1 must **not** be treated as “just a WAV player”.

It must be treated as:

**a small timeline/playback engine that currently happens to support one audio clip**

This means the design must support a logical growth path toward:

- multiple clips
- multiple tracks
- clearer transport ownership
- separation of playback, file loading, waveform generation, and UI
- future routing and mixer-related expansion

However, that future growth path must be supported **without introducing premature subsystems** outside the current phase scope.

## Core Separation Requirements

### UI and engine separation

UI must not own audio engine logic.

UI may display state and invoke high-level actions, but it must not become the hidden owner of playback behavior, transport truth, decoding flow, or engine decisions.

### Clear transport source of truth

Transport state must have a clear source of truth.

Playback position, play/pause/stop intent, and seek state must not be duplicated across multiple unrelated objects without explicit justification.

The transport source of truth for Phase 1 is a single object named `Transport`,
owned at the application composition level. The exact top-level class
arrangement that instantiates and owns it is an implementation choice.

`Transport` holds:

- the playback intent (playing / paused / stopped),
- the authoritative playhead position in samples,
- any pending seek request.

Writers are constrained:

- playback intent is written only by non-realtime code in response to user
  actions,
- the authoritative playhead position is written only by `PlaybackEngine`
  from inside the audio callback,
- a seek is expressed as a pending seek request written by non-realtime code;
  `PlaybackEngine` consumes it at the start of its next callback and applies
  it to the playhead.

No other component may mutate these fields. UI components read transport
state but do not own or mutate it.

**Project load:** Restoring a saved playhead position is expressed only as
`Transport::requestSeek` after a new session snapshot is published. The
project file may record `playheadSamples` and `deviceSampleRateAtSave`, but
`Transport` is not populated from a snapshot; it remains the sole owner of
the live playhead and seek pending state.

### Phase 2 playhead and session (one timeline, multiple clips)

Phase 2 reuses the same `Transport` fields. The **meaning** of `playheadSamples` changes
from “sample index in the one loaded clip” to a **timeline-absolute** index on the
**session timeline**: sample `0` is the start of the timeline, and the playhead runs in the
same address space that placement and the waveform use (until a later phase introduces
more complex timebases).

**`Session` owns** the set of **placed** clips, each with a **start sample** on that timeline
and a deterministic **front-to-back** order for overlap. The UI and waveform are **read-only
consumers**; they do not own clip order. For Phase 2, **newest added is front-most** (index 0)
unless steering documents are updated.

**Where the user seeks:** With a **minimal timeline ruler** above the event lane, **seek** is requested from that strip only (same session sample axis as the playhead). The **event lane** handles clip selection and move; it does not seek on empty background. `Transport` remains the only seek/playhead owner.

**Coverage playback (Phase 2, per track):** on a **single** track, only the **front-most** placed clip that
**covers** that timeline position is audible in that lane (stacked “events” mental model;
**not** summing overlapping clips **on the same track**).

**Phase 3 minimal multi-track:** `SessionSnapshot` holds an ordered list of **tracks**; each track has its own front-to-back clip list and the same overlap rule as Phase 2 **within that lane**. For the same timeline instant, output is the **sum** of what each track would produce on its own (not a mixer UI, no per-track gain).

**Phase 3 late extension — cross-track clip move:** A clip may be moved to another track with **`Session::moveClipToTrack`** and **`SessionSnapshot::withClipMovedToTrack`**, which reassign the existing `PlacedClipId` to the target track at a new timeline start, inserting the row as **front-most (index 0)** on that track. This is a distinct, named command from **within-track** **`Session::moveClip`** (committed end-state rule only in the clip’s current lane). **No** per-track “compatibility / type” predicate in the current project — “valid target lane” is purely geometric. **`Session::moveClipToTrack` does not change** which track is **active** for **Add clip** — only **`Session::addTrack`** and **`Session::clearClip`** (reset) do.

**Phase 3 late extension — track headers (UI):** each **`Track`** carries a **display name** in the **domain** (`Track::getName()`), set when the snapshot is built (e.g. default `"Track 1"`, `"Track 2"`, … from **`Session`**, not from UI list indices). The **add-clip target** is **`Session::activeTrackId_`**, a **message-thread** field; it is **not** in **`SessionSnapshot`** and **does not** require a snapshot republish when the user **clicks a header** to call **`Session::setActiveTrack`**. The timeline ruler and lane area share the **same** horizontal x ↔ session-sample range: the main layout insets the ruler by the **same** fixed width as the left header column so alignment matches pre-header behaviour in the lane strip.

**Phase 3 late extension — track reorder (header drag):** **Row order** of **`Track`s** in **`SessionSnapshot`** is changed only by a **named** command **`Session::moveTrack` / `SessionSnapshot::withTrackReordered`**, not by in-flight UI. Each **`Track`’s** internal **`PlacedClip`** list is **unchanged**; engine **sum** across tracks is the same. **`activeTrackId_`** is **not** reassigned on reorder; the same **id** appears on a different row. The gesture is **header-only**; in-flight feedback (insert line) is **UI-local**; invalid cursor uses the same **forbidden** glyph as an **invalid** cross-lane **clip** drop, via one shared **UI** helper, not a second cursor implementation.

**Phase 3 late extension — non-destructive right-edge trim:** **`AudioClip`** PCM is never shortened. **`PlacedClip`** holds the **placement window** (effective length) on that material. **`SessionSnapshot::withClipRightEdgeTrimmed`** replaces one row by id without changing lane order (not split, not “cut”). Playback offset in the material remains from sample **0**; the audible region is bounded by the effective length. **Timeline** extent, **overlap** resolution, the **engine** coverage test, and **waveform** paint span use **effective** length, not `getAudioClip().getNumSamples()` alone for placement.

**Phase 4 — minimal mono input recording (steering reference):** The **application root** (e.g. `MainComponent`) **coordinates** numpad `*`, `Transport` intents, playhead for record placement, and begin/stop of capture; a **`RecorderService` (or equivalent) must not** call **`Transport`** or own transport policy. The audio callback is limited to **realtime-safe** handoff to an SPSC-style buffer; take files, `AudioFormatWriter`, and new **`PlacedClip` / `SessionSnapshot`** work happen **off** the callback, in the order documented in `docs/PHASE_PLAN.md` (Phase 4) and `status/DECISION_LOG.md`. While recording, the **engine** may **skip mixing** the recording track (transient only; not a session mute). See Phase 4 for device scope, FIFO overrun rules, unsaved-project refusal, and deferred latency compensation.

**Snapshot handoff** generalizes Phase 1: the audio thread loads an **immutable** snapshot of
session placement (e.g. `std::shared_ptr` to a const snapshot value) with **lock-free, non-allocating**
reads on the hot path; the exact snapshot type is an implementation choice consistent with
`docs/PHASE_PLAN.md` and `status/DECISION_LOG.md`.

**Session-owned overlap order — how it may change:**  
Front-to-back order of placed clips is **session state**, not UI state. Any change to that order must be expressed as an **explicit, named** `Session` (or `SessionSnapshot`) operation — for example, adding a clip, **`Session::moveClip`** (within a track, end-state rule in `docs/PHASE_PLAN.md`), or **`Session::moveClipToTrack`** (to another track, **front-most insertion** on the destination; **no** ad-hoc per-track “type” gate in the current codebase). **Selection, hover, and in-flight drags** must not silently reorder clips. The principle is: **no order mutation as a side effect of general UI state.**

### File loading is separate from playback

File import, file opening, and audio decoding concerns must be separated from playback control and playback execution.

Opening a file must not implicitly blur boundaries between:
- file loading
- clip/session state
- transport behavior
- audio output behavior

### Waveform rendering is separate from playback

Waveform generation/rendering must be separated from playback logic.

A waveform view may depend on audio file or clip information, but waveform-related code must not become entangled with transport execution or audio callback behavior.

### Avoid hidden singletons

No hidden singletons unless explicitly justified in documents.

Global or effectively global state should be avoided unless there is a documented architectural reason.

### Clear ownership and lifetimes

Ownership and lifetimes must be explicit.

It should always be reasonably clear:
- what owns what
- which objects are long-lived
- which objects are phase-local or view-local
- which references are non-owning
- where destruction order matters

## Audio-Thread Safety Principles

The audio callback is a constrained environment.

Do not move logic into the audio callback unless explicitly justified.

In particular, avoid placing the following into the realtime path unless documented and justified:

- file loading
- decoding setup
- UI work
- waveform generation
- allocation-heavy behavior
- hidden synchronization that may block
- state mutation whose thread-safety model is unclear

If threading or background work is introduced, the synchronization model must be explicitly explained first.

### Phase 1 cross-thread model

Phase 1 uses the message thread for UI and synchronous file loading, and uses the audio-callback thread for playback (`PlaybackEngine`).
Transport state is read and written across these two threads.

The cross-thread model is constrained as follows:

- communication of transport state between the message thread and the audio
  callback must be lock-free and non-blocking,
- the audio callback is the only writer of the authoritative playhead
  position,
- the message thread may request a seek, but the audio callback applies it;
  the message thread never writes the authoritative playhead directly,
- the audio-callback path must not take mutexes, wait on condition variables,
  allocate, or use any other blocking synchronization.

The exact lock-free primitives used to realize this model (for example
atomic variables, single-producer flags, or equivalent mechanisms) are an
implementation choice, not a steering-level constraint, as long as the
properties above hold.

If later phases introduce richer transport state (for example loop regions
or tempo), this section must be revisited before that state is added.

## Phase 1 Intended Conceptual Split

Phase 1 should aim toward a clear conceptual split such as:

- **App / composition layer**  
  Top-level assembly and wiring.

- **Transport / playback control layer**  
  High-level playback intent and position control.

- **Engine layer**  
  Audio-device-facing and playback-facing behavior.

- **Domain/session layer**  
  Concepts such as loaded clip, timeline placement, and session state.

- **File loading / import concept**  
  Responsibility for turning a file path into a loaded clip, with no
  knowledge of transport, engine, or UI. This is a concept, not a
  subsystem: in Phase 1 it may be a single small class. It must not own
  clips, playback state, or transport.

- **UI layer**  
  Waveform display, transport controls, playhead display, and user interaction.

This is a conceptual split, not a forced early over-abstraction.  
The implementation may remain small, but responsibilities must remain visible.

## Scaling Constraint

The architecture must work for one clip now **without poisoning the path** toward multiple clips and tracks later.

That means Phase 1 must avoid designs that only work because there is exactly one clip, if those designs would create painful refactoring later.

Examples of risky patterns:

- UI object secretly owning transport truth
- playback state embedded only in a waveform component
- clip identity and transport identity being treated as the same thing
- file import directly constructing playback behavior with no separable model
- one-off logic that assumes only one future track or one future clip forever

## Anti-Scope-Creep Rule

Do not add architectural concepts that are not explicitly needed for the current phase.

In particular, do not introduce major subsystems for:
- plugin hosting **beyond** the minimal per-track single VST3 insert slice documented as **Phase 8** in `docs/PHASE_PLAN.md`
- mixer/routing frameworks
- background asset pipelines
- generalized track graphs
- advanced persistence systems
- undo frameworks
- multi-threaded job systems

unless those are explicitly added to the steering documents first.

## Plugin host — ownership and threading (Phase 8)

- **Live plugin instances are not part of `SessionSnapshot`.** They are mutable, may own GUI editors, and must be constructed and torn down on the **message thread** only.
- A **`PluginInsertHost`** (name in code is an implementation detail) **owns** `TrackId → std::unique_ptr<juce::AudioPluginInstance>` on the message thread. It publishes a separate **atomic `shared_ptr` to an immutable “active processor view”** for the audio callback (same handoff *idea* as `Session::sessionSnapshot_`, but a different pointer and payload).
- **`prepareToPlay` / `releaseResources` / loading state / opening editors** run on the message thread. The audio callback must only **read** the active view and call **`processBlock`** on already-prepared processors into **pre-sized scratch buffers** — no locks, no heap allocation on that path, no `Session` access.
- **Persistence:** project file v8 stores plugin **file path** (absolute `.vst3` on disk), a stable **identifier string** for mismatch detection, and an opaque **`getStateInformation`** blob (e.g. Base64). This is **not** clip PCM and does not use the `Audio/` project-relative audio policy.
- **Realtime caveat:** third-party `processBlock` code is **not** guaranteed to be allocation-free or lock-free; this is accepted for Phase 8 and must not be mistaken for a certification of realtime safety.

## Modern C++ Principle

Modern C++ should be used when it improves correctness, clarity, maintainability, or architectural fit.

Do not avoid newer language features merely because they are newer.

However:
- do not use advanced features for their own sake
- do not hide simple ownership or flow behind unnecessary cleverness
- prefer readable and learnable code

**Pedagogical visibility:** Responsibility, ownership, lifetime, and thread or realtime constraints must be **visible in the code** for central types and files—not only in external prose. A reader who knows C++ but not this codebase or JUCE should be able to orient themselves from class and file-level documentation, from the thread/realtime markers on the audio path, and from **readable method bodies** (top-down intent in non-trivial methods and callback reachability), without reverse-engineering intent from implementation details. The **exact** rules (six tiers: file header, class doc, method doc, audio-thread markers, JUCE-usage notes, **body readability**; plus bounded **readability refactors** during documentation passes where listed in `docs/IMPLEMENTATION_GUIDE.md`), anti-patterns, and the hard validation gate are defined in `docs/IMPLEMENTATION_GUIDE.md` under **In-Code Documentation Requirements**. That section is part of the architecture envelope for how implementation is expressed; it is not optional commentary.

## Required Architectural Questions Before Implementation

Before implementation of a phase, the following questions must be answered:

1. What is the source of truth for transport state?
2. What owns the loaded audio file or clip model?
3. What code is responsible for playback versus file import versus waveform display?
4. What assumptions are being made because Phase 1 only supports one clip?
5. Which decisions are intentionally deferred, and why is that safe?

If these questions cannot be answered clearly, the architecture docs must be updated before implementation proceeds.
