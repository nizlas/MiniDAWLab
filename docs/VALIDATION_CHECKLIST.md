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

## Code Documentation (in-code rubric)

In addition to the questions above, every completed phase that adds or changes **central** hand-written source (see `docs/IMPLEMENTATION_GUIDE.md`, **In-Code Documentation Requirements**) must pass the following **hard** gate. A phase is not successful if it leaves new or meaningfully changed central files in violation of the six tiers without an explicit, reviewed exception.

Ask explicitly, for this phase’s delta:

1. **File headers** — Does every new or meaningfully changed central `*.h` / `*.cpp` file in scope have a file header that states the file’s role, threading expectations, and how it fits the architecture?

2. **Class documentation** — Does every new or changed **public** class in scope have a class-level doc block covering responsibility, ownership/lifetime, and thread rules (and deliberate non-responsibilities where that avoids confusion)?

3. **Method and function documentation** — Does every **non-trivial** public method and every **thread-sensitive** or audio-path-related function in scope have a contract comment, including which thread may call it and, where applicable, realtime constraints?

4. **Audio thread** — For every function that runs on or is only for the audio device callback, is there an explicit audio-thread marker and a one-line statement of what must not happen on that path (unless a named steering exception applies)?

5. **Body readability** — For every new or meaningfully changed non-trivial public method and every audio-path function in scope, can a C++-fluent reader who does not know JUCE and does not know this codebase follow the main path *top-down* from comments and local names alone, without having to reverse-engineer pointer arithmetic, channel indexing, or buffer-tail handling to understand intent? Is chunking and naming used as a *readability* tool (not a formulaic ritual), and are any refactors in `docs/IMPLEMENTATION_GUIDE.md` under **Readability refactors allowed during documentation passes** listed in the commit and within those bounds?

If the answer to any of these is *no* for a file that the phase created or substantively edited, the phase is not complete until the gap is fixed or a documented exception is approved; silent drift is not acceptable.

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

### Recording (when Phase 4 or later touches input)

- Does the recorder or service **own** `Transport` or call it directly, instead of the app root (`MainComponent` or equivalent) coordinating?
- Is there any **lock / allocate / block / wait / `Session` / `SessionSnapshot` / UI** access on the audio-thread push path?
- Are FIFO overruns **counted** and **surfaced** after stop, with timeline duration preserved (silence fill or explicit corruption flag), not a shorter file with no notice?
- Is **`SessionSnapshot` published only after** the take file is finalized and `RecordedTakeResult` is complete?
- Is preview/peak data **drained in one place** so non-recording waveform views do not race?

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

## Phase 2 late extension: single-lane selection and move

When validating the **minimal single-lane** selection + single-clip drag step (`docs/PHASE_PLAN.md` late extension), check at least the following in addition to the general phase checks:

- **Selection is UI-only:** no `PlacedClipId` / selection in `SessionSnapshot` after a click; only a committed `Session::moveClip` changes the published snapshot.
- **Hit test:** the front-most (lowest snapshot index) event under the pointer wins when clips overlap; empty **lane** background clears selection and does **not** seek (seek is on the timeline ruler only).
- **No reorder in-flight or on select:** only `SessionSnapshot::withClipMoved` applies the committed end-state rule (isolated end → promote to index 0; still overlapping → preserve ordinal). Drag preview does not publish the session.
- **Commit threshold:** a tiny pointer jitter without crossing the movement threshold does not call `moveClip`.
- **Playback and transport** unchanged: `PlaybackEngine` still reads the snapshot handoff; no new mixing rule.

## Phase 2 late extension: minimal timeline ruler

When validating the **minimal timeline ruler** strip (`TimelineRulerView` above the lane, `docs/PHASE_PLAN.md`):

- **Ruler-only seek:** `Transport::requestSeek` is driven from the ruler (mouse down/drag on the strip), not from empty background clicks in `ClipWaveformView`.
- **Empty lane click:** still clears selection; does not move the playhead.
- **Same x↔sample map:** ruler and lane have the same width under the same parent insets; playhead line in the lane and playhead marker in the ruler stay aligned.
- **Ticks:** second-grid marks only, no text labels; step coarsens (1s → 5s → …) when the timeline is long or the window is narrow.
- **No new domain contracts:** `Session` / `SessionSnapshot` / `Transport` / `PlaybackEngine` behavior unchanged—only UI wiring and presentation.
- **Sample rate in ruler:** used only to place ticks in “seconds of session time,” not to redefine transport.
- **Stop:** still seeks playhead to session sample 0; ruler and lane both show 0.

## Phase 3: minimal multi-track (domain + lanes + sum)

When validating the **minimal multi-track** step (`docs/PHASE_PLAN.md` Phase 3, `status/DECISION_LOG.md`):

- **Default track:** a fresh session has exactly one track; new clips go there until the user adds another track.
- **Add track:** appends a new empty track; the new track becomes **active**; **Add clip** places on the active track.
- **Multiple tracks:** can load/place clips on different tracks; all tracks share one timeline and transport playhead.
- **Minimal project file (v1 / v2):** save writes **v2** with the same absolute paths and session metadata; optional per-clip `visibleLengthSamples` when a clip is right-trimmed (0 = full). Load accepts **v1** and **v2**; load decodes into one new `SessionSnapshot` (single publish), skips broken clips with a user-visible list, and seeks via `Transport` only. Corrupt or unsupported project files do not replace the current session.
- **Playback sum:** with clips on more than one track (non-overlapping or staggered in time), you hear the **sum** of both contributions when both are “on” in device time. **Within** a single track, overlapping clips are still **not** summed; front-most still wins there.
- **Within-track move:** `Session::moveClip` and committed drag still apply on the track that owns the moved clip (end-state rule in that lane only).

## Phase 3 late extension: cross-track clip drag

When validating **cross-track drag** (`docs/PHASE_PLAN.md` Phase 3 late extension, `status/DECISION_LOG.md`):

- **Domain:** `SessionSnapshot::withClipMovedToTrack` removes the row from the source track and inserts the **same** `PlacedClipId` at index **0** on the target track with the committed timeline start. **`Session::moveClipToTrack` does not change `activeTrackId_`.**
- **Within-track vs cross-track:** same-lane release still calls **`Session::moveClip`** only; different-lane release calls **`Session::moveClipToTrack`**. No **track-type / compatibility** checks — any lane in the stack is a valid target (geometric hit only).
- **Drop outside all lanes:** **no** `Session` publish on `mouseUp` (cancel); clip unchanged.
- **Ghost:** shown **only** on the lane under the pointer **after** the movement threshold, and **not** duplicated on the source lane (source already shows the live drag preview). **Outside every lane,** no ghost anywhere.
- **Invalid-drop cursor:** when outside all lanes (after threshold), source lane shows a **non-default** cursor until re-entering a lane or **`mouseUp`** (cursor always restored on release).
- **Playback / transport / engine:** unchanged; no mixer / undo / snap / multi-select introduced.
- **Ruler / playhead / stop / seek:** unchanged transport semantics; ruler strip and playhead line stay aligned with stacked lanes (same insets/width as before).
- **No mixer UI:** no per-track faders, meters, sends, or buses; summing is engine-only.
- **Steering:** `docs/PHASE_PLAN.md`, `docs/ARCHITECTURE_PRINCIPLES.md`, and this checklist stay consistent with per-track coverage + across-track sum + the Phase 3 late extension on cross-track drag (if implemented).

## Phase 3 late extension: minimal track headers

When validating **track headers** (`docs/PHASE_PLAN.md` Phase 3 late extension, `status/DECISION_LOG.md`):

- **Names:** `Track::getName()` is the **display** name; the UI does not derive names from list index. Default names come from **Session** when building snapshots (`"Track 1"`, etc.).
- **Active track:** `Session::getActiveTrackId` / `Session::setActiveTrack`; **setActiveTrack** does **not** publish a new `SessionSnapshot` (no `atomic_store` on that path). Clicking a header makes that track the add-clip target; **Add track** and **Clear** still reset/choose active as before.
- **Layout:** the timeline ruler and the **lane** waveform area share the **same** content width: the main layout insets the ruler by the same left column width as the track header strip, so the playhead line still aligns with the ruler at the same x in the **lane** area.
- **Cross-track drag:** a pointer over a **header** is not a valid lane; behaviour matches “outside all lanes” for ghost/invalid-cursor. No drop-on-header.
- **Not in scope:** rename field editing, mixer controls, or playback changes.

## Phase 3 late extension: track reorder (header drag)

When validating **track reorder** (`docs/PHASE_PLAN.md`, `status/DECISION_LOG.md`):

- **Domain:** `Session::moveTrack` / `SessionSnapshot::withTrackReordered` only change the **order** of `Track` rows; **no** change to any `PlacedClip` list inside a track. **`activeTrackId_`** is **not** assigned in `moveTrack`.
- **UI:** drag starts on a **header** only; **invalid** = outside `TrackLanesView` or **x ≥ kTrackHeaderWidth** (lane area) — **forbidden** cursor, **no** insert line; release = no publish. **Green** line = real reorder; **red** = no-op; line only in **`TrackLanesView::paintOverChildren`**. **Forbidden** cursor uses the same helper as invalid **clip** cross-lane drop (`ForbiddenCursor`).
- **Separation:** clip drag / cross-track clip drag is unchanged; header drag does not use lane hit-test for “valid drop”.

## Phase 3 late extension: non-destructive right-edge trim

When validating **right-edge trim** (`docs/PHASE_PLAN.md`, `status/DECISION_LOG.md`):

- **PCM:** decoded `AudioClip` length is unchanged by trim; extending the right edge back toward the material end **restores** audio.
- **Domain:** trim state lives on **`PlacedClip`** (effective length only), not on **`AudioClip`**. **`SessionSnapshot::withClipRightEdgeTrimmed`** does **not** promote or reorder clips in the lane.
- **Engine / timeline / overlap:** coverage, derived session length, and within-lane overlap math use **`getEffectiveLengthSamples()`** (or equivalent); playback still uses `off = t - startSample` and caps the run by the effective length.
- **UI:** right-edge handle is distinct from move and cross-lane drag; trim does not call **`moveClip`** / **`moveClipToTrack`**.
- **Project:** **v1** files still load; save produces **v2**; trim round-trips when visible length is not full.

## Phase 3 late extension: visible timeline span (UI viewport)

When validating the **visible span** / `TimelineViewportModel` step (`docs/PHASE_PLAN.md`, `status/DECISION_LOG.md`):

- **Scale vs span:** the model stores **`samplesPerPixel`**; **visible length in samples** = `getVisibleLengthSamples(widthPx)` = `round(widthPx * samplesPerPixel)` and is not stored. **Resize** with fixed zoom: events keep the same horizontal scale (wider window ⇒ more time visible, not fatter events).
- **Stability after trim (inward):** the ruler + lane do **not** change `samplesPerPixel` on session edits; only the event’s right edge moves. The derived visible span can still extend past the last clip; empty tail is normal.
- **Extend trim outward:** same `samplesPerPixel` before and after the commit. Trim preview that extends the mapping span still uses `sessionSampleToLocalXForSpan` when needed.
- **Only** `setSamplesPerPixelIfUnset` and **`zoomAroundSample`** change the zoom state after the one-time seed. **Pan** only changes `visibleStart`. **Not in session:** `TimelineViewportModel` is not in `.mdlproj` and is not on the audio path.

## Phase 3 late extension: arrangement extent + pannable viewport (v3)

When validating **arrangement extent**, **pan**, **zoom**, and **v3** (`docs/PHASE_PLAN.md`, `status/DECISION_LOG.md`):

- **Separates:** `getContentEndSamples()` = clip-derived end; `getArrangementExtentSamples()` = navigable/ playable; viewport = `visibleStart` + **derived** visible length from `widthPx` and `getSamplesPerPixel()` (UI only, not in file).
- **Trim / move:** `arrangementExtentSamples_` in the snapshot is **unchanged** on trim, move, or track reorder; no extent reset in any factory.
- **Playback:** in a gap or past the last clip (but before extent), the engine outputs **silence**; playhead advances; stops at `getArrangementExtentSamples()`.
- **Seek / ruler** maps with `s = visStart + round(x * samplesPerPixel)` (same as lanes’ timeline column, header subtracted). `requestSeek` is clamped to **arrangement** extent, so empty tail to extent is seekable.
- **Default seed:** with no material and no stored extent, first `sync` after device rate is known extends arrangement to **1 hour (3600s)** in samples and seeds **`samplesPerPixel = sampleRate / 10`**. Opening a v1/v2 project with clips does **not** apply that empty-session path (stored 0, content>0, effective = content end from clips).
- **Save / load v3** round-trips `arrangementExtentSamples`; v1/v2 still load; save yields v3.
- **Plain wheel** pan uses `round((width/8) * samplesPerPixel)` sample steps. **`Ctrl+wheel` zoom** is **mouse-centered** on **pixel** `x` in the timeline column; `spp` in `[0.1, max(1, ext/width)]`. Trim/move drag in progress blocks **all** wheel handling on `TrackLanesView` (no pan or zoom during gesture).
- **Ruler and lanes** stay aligned: `TimelineRulerView` static mappers; one `TimelineViewportModel`; ruler width matches lane **timeline** width in `Main` layout. **Tick count** increases when widening the window; **tick step (seconds)** only changes when zooming.

## Phase 4: simple audio recording (minimal mono)

When validating **simple mono input recording** (`docs/PHASE_PLAN.md` Phase 4, `status/DECISION_LOG.md` 2026-04-26 — Phase 4 minimal mono recording):

- **Coordinator vs recorder:** numpad `*`, `Transport` (`requestPlaybackIntent`, playhead read), and record arm/target track selection are **coordinated** from the app root (e.g. `MainComponent`). A **`RecorderService` (or equivalent) does not** call `Transport` or own play/stop/seek policy.
- **Audio callback (SPSC-style path):** the callback path that **pushes** input/preview to the recorder performs **no** lock, allocation, block, wait, `Session` / `SessionSnapshot` access, or UI state mutation. Disk, WAV writer, and snapshot publish are **off** the callback.
- **FIFO capacity:** initial mono **sample** ring is **at least** `next_pow2(deviceSampleRate * 5)` (≈5 s). On overrun, the audio thread **never** blocks: **accept** what fits; count the rest as **dropped**; **intended** take length in samples (timeline duration) is **not** shortened. Missing samples become **silence in the final WAV** (or an equivalent explicit mark) and are reflected in **`droppedSampleCount`**. `recordedSamples_` / intended duration **include** inserted silence. After stop, **warn** if any samples were dropped. No silent file shortening.
- **Stop / finalize order:** (1) `requestPlaybackIntent(Stopped)`; (2) **stop** pushing from the callback; (3) **signal** writer, **drain** FIFO and pending silence/dropout; **flush/close** WAV; **join** writer; (4) return a **complete** `RecordedTakeResult` (or equivalent) with: finalized file, **target** `TrackId`, **`recordingStartSample`**, **intended** sample count, **sample rate**, **`droppedSampleCount`**; (5) **only then** create the `PlacedClip` and **publish** `SessionSnapshot`.
- **Preview peaks:** **one** consumer per tick (e.g. `TrackLanesView`) **drains** preview data and forwards it **only** to the **recording** lane. Non-recording **`ClipWaveformView`** instances do **not** call `drain…` in parallel.
- **Non-destructive on a busy track:** no move, split, trim, delete, mute, overwrite, take lanes, comping, or punch. A new `PlacedClip` at `recordingStartSample`; if it **overlaps** existing clips, **existing** topmost-wins overlap behavior applies **unchanged**.
- **Engine during record:** the **armed / recording** track is **omitted** from **playback** mixing; **other** tracks play. Existing clips on that track **stay visible**; this is **transient** engine state only — **not** a `Session` / `SessionSnapshot` mute.
- **Device scope (when implemented):** **mono**; **input channel 0** only; device init will move to **`initialiseWithDefaultDevices(1, 2)`**; **no** new audio settings UI, **no** SRC, **no** software/direct monitoring in this slice. Latency compensation is **deferred** (document the future hook: callback input + `juce::AudioIODevice` latency getters).
- **Unsaved project:** with **no** saved on-disk project path, recording **fails** with a **visible** message (e.g. *“Save the project before recording.”*). **No** transport start, **no** orphan take file, **no** empty committed clip.
- **Narrow slice:** no mixer, pan, sends, routing matrix, split/cut, fades, plugin processing, or full Cubase-like audio settings in this phase.

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
- **in-code documentation:** it passes the **Code Documentation (in-code rubric)** gate (all six tiers, including body readability) for every central file the phase added or meaningfully changed
