# Validation Checklist

This document defines how completed phases must be validated before MiniDAWLab moves forward.

A phase is not considered complete just because it appears to work.  
It must also be checked against the project brief, the architecture envelope, and the implementation constraints.

Validation must be performed after every phase before moving on.

## Validation Rule

After each phase, validate against:

- `PROJECT_BRIEF.md`
- `docs/ARCHITECTURE_PRINCIPLES.md`
- `docs/IMPLEMENTATION_GUIDE.md`
- `docs/PHASE_PLAN.md`

If the implementation works functionally but violates the steering documents, it is not considered an acceptable result.

## Core Validation Questions

For every completed phase, explicitly ask:

1. Does this implementation stay within the current documented scope?
2. Did the implementation introduce hidden architecture not explicitly approved?
3. Is the responsibility split still clear?
4. Is the result understandable without reading every line of code?
5. Does the implementation support the next likely phase without forcing premature generalization?
6. What is still weak, provisional, or risky?
7. What steering-document updates, if any, are now needed before proceeding?

## Required Validation Output

Every phase review should produce the following:

### 1. Alignment check
A short statement of whether the phase is aligned with the steering documents.

### 2. Flow map
A short explanation of how control and data move through the implementation.

### 3. Responsibility map
A short list of the key files/classes and what each one owns or is responsible for.

### 4. Weakness/risk note
A short list of what is still weak, provisional, fragile, or intentionally deferred.

### 5. Next-step note
A short statement of what must be true before the next phase begins.

## DAW-Specific Risk Checklist

The following risks must be checked explicitly after each relevant phase.

### UI / engine coupling

- Has UI code become the hidden owner of playback or engine behavior?
- Is UI merely invoking actions and showing state, rather than owning engine logic?
- Would engine logic remain understandable if the UI were changed?

### Audio-thread safety

- Has any logic been pushed into the audio callback without explicit justification?
- Is there any allocation-heavy, blocking, decoding-heavy, or synchronization-heavy behavior in the realtime path?
- Is the thread-safety model clear enough to explain simply?

### Ownership and lifetimes

- Is it clear what owns the major objects introduced in this phase?
- Is destruction order obvious where it matters?
- Are non-owning references distinguishable from owners?
- Are lifetime assumptions implicit anywhere?

### Transport source of truth

- Is transport state owned in one clearly explainable place?
- Has play/pause/stop/seek state been duplicated across multiple objects?
- Could two parts of the system disagree about transport state?

### File import / decoding separation

- Is file loading/import clearly separate from playback execution?
- Is decoding-related logic entangled with transport control?
- Has clip/file loading accidentally become playback ownership?

### Waveform / playback separation

- Is waveform generation/rendering clearly separate from playback logic?
- Has waveform code become a hidden state owner?
- Could waveform rendering be changed later without rewriting playback logic?

### Scaling from one clip to multiple clips/tracks

- Does the design only work because there is currently one clip?
- Are there one-clip assumptions hidden in naming, ownership, or control flow?
- Would the next phase require a full rewrite rather than a controlled extension?

### Premature scope expansion

- Were any new subsystems introduced that are outside the documented phase scope?
- Was any new abstraction introduced without clear need?
- Was future-proofing pushed too far relative to the current phase?

## Plausible Wrong Implementations Check

For every phase, explicitly ask:

### What implementations might appear to work but are actually wrong?

Examples include:

- storing transport truth in a UI widget because it is convenient
- mixing waveform state and playback state in one component
- allowing file loading code to directly define playback ownership
- embedding single-clip assumptions everywhere because Phase 1 only has one clip
- placing convenience logic in the audio callback because it is “close to playback”
- introducing a hidden manager/singleton to avoid explicit ownership

The goal is to identify deceptive success: implementations that demo well but create future architectural damage.

## Deferred Decisions Check

For every phase, explicitly ask:

1. Which important decisions were deferred?
2. Was the deferral intentional?
3. Why is the deferral safe for now?
4. What future phase is expected to resolve it?

Deferred decisions are acceptable only if they are visible and intentionally contained.

## Phase 1 Validation Checklist

For Phase 1 specifically, validate all of the following:

- one audio file can be loaded
- waveform is visible
- play / stop / pause work
- seek / playhead work
- playback through the audio device works
- UI, transport, engine, file loading, and waveform responsibilities are meaningfully separable
- the implementation is not merely a “WAV player” disguised as architecture
- the result can plausibly grow toward multiple clips without fundamental redesign

## Failure Rule

If validation reveals that the implementation:

- violates the steering documents
- hides architectural decisions in code
- introduces unclear ownership
- couples UI and engine in a way that will be hard to unwind
- or only works by relying on single-phase shortcuts

then the correct response is not to “push ahead anyway”.

Instead:

1. document the problem explicitly
2. assess whether the implementation should be revised
3. assess whether the steering documents are missing something
4. resolve that before starting the next phase

## Success Rule

A phase is considered successful only when:

- it works functionally
- it aligns with the steering documents
- its responsibility split is explainable
- its known weaknesses are explicitly stated
- it leaves a credible path to the next phase without hidden architectural debt
