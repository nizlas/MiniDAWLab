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

Extend the design from one clip to multiple clips on a timeline without collapsing the architectural separation established in Phase 1.

### In Scope

- support for more than one clip in session/timeline state
- clip placement logic appropriate to the documented scope
- playback behavior that remains understandable with multiple clips
- UI changes necessary to show multiple clips
- validation that transport/state ownership remains clear

### Out of Scope

- multiple tracks
- mixer
- routing graph
- sends / buses
- MIDI
- recording
- plugin hosting
- advanced editing toolset

### Expected Value

The architecture proves that Phase 1 was not a dead-end single-file design.

### Key Risks

- one-clip assumptions hidden in state ownership
- waveform/playback coupling becoming more tangled
- transport truth becoming duplicated
- premature generalization into full DAW subsystems

---

## Phase 3 — Multiple Tracks

### Goal

Introduce multiple tracks while preserving understandable state ownership and avoiding premature mixer/routing complexity.

### In Scope

- track-level session representation
- clip-to-track association
- UI representation sufficient to make tracks visible and understandable
- playback behavior extended to track-aware structure
- validation of scaling from single-track assumptions

### Out of Scope

- full mixer
- sends / buses
- plugin hosting
- MIDI instrument hosting
- recording workflows
- complex editing tools

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
