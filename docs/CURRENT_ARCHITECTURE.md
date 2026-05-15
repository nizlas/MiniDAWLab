# Current architecture (baseline)

This document states **what the MiniDAWLab codebase is today** — not phase history or roadmaps.

Use it alongside [ARCHITECTURE_PRINCIPLES.md](ARCHITECTURE_PRINCIPLES.md), [PROJECT_BRIEF.md](../PROJECT_BRIEF.md), and [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md).

---

## Composition and threads

- **Message thread**: UI, `Session` mutations that publish snapshots, plugin/host lifecycle, project I/O, device setup.
- **Audio thread**: `PlaybackEngine` callback only. It must not touch `Session`, allocate, block, or take locks. It reads **published** immutable views via atomics (see below).

---

## Session and snapshot

- **`SessionSnapshot`** is **immutable**. Every session edit builds a new snapshot and publishes it with a single atomic store (`Session::sessionSnapshot_`, `memory_order_release`).
- **`SessionSnapshot::tracks_`** is the **canonical ordered list** of timeline lanes. Row order is what the user sees (headers + lanes) and what undo/redo and project save preserve.
- **`Track::kind`** is `TrackKind::Audio` or `TrackKind::Instrument`. Instrument lanes are **first-class domain rows**, not UI-only decorations.
- **`Session::activeTrackId_`** (message-thread only, **not** in the snapshot) selects where **Add clip** targets audio; it does not replace `TrackId` for instrument binding.

**Pointers:** [src/domain/SessionSnapshot.h](../src/domain/SessionSnapshot.h), [src/domain/Track.h](../src/domain/Track.h), [src/domain/Session.h](../src/domain/Session.h), [src/domain/Session.cpp](../src/domain/Session.cpp).

---

## Multiple instrument tracks (current truth)

- **Multiple `TrackKind::Instrument` rows in one session** are normal, supported architecture — not a transitional exception.
- **`TrackId`** is the stable identity that binds:
  - instrument **runtime** (host + controller maps),
  - **UI** (header, MIDI event lane),
  - **MIDI pattern / editor** binding for that lane,
  - **project** experimental payloads (`experimentalInstrumentTracks[].trackId` for v13+),
  - **playback bridge** entries (`ExperimentalInstrumentPlaybackEntry::trackId`).

Do **not** design new features around a single “the” instrument or “primary” lane unless a future doc explicitly revives that product decision.

---

## Instrument runtime (TrackId-keyed)

- **Registry** (owned by [`InstrumentRuntimeCoordinator`](../src/app/InstrumentRuntimeCoordinator.h), constructed from `TransportControlsContent` in [src/app/MainAppWindow.cpp](../src/app/MainAppWindow.cpp)):
  - `std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>> instrumentHostsByTrackId_`
  - `std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>> instrumentControllersByTrackId_`
- **`ExperimentalInstrumentHost`**: **one instance per instrument track**. Each instance holds **at most one loaded plugin** at a time (`activeOwner_`). Multi-track = **many host objects**, not one host managing many TrackIds internally.
- **`InstrumentTrackController`**: **one per instrument track**; binds that lane’s MIDI clips / render snapshot to the paired host.
- **Staging pair** (`instrumentStagingHost_` / `instrumentStagingController_`): used only around **first-time add / promotion** flows when a runtime is not yet bound to a timeline `TrackId`. It is **not** a second global instrument model alongside the TrackId registry.

**Pointers:** [src/app/InstrumentRuntimeCoordinator.h](../src/app/InstrumentRuntimeCoordinator.h), [src/plugins/ExperimentalInstrumentHost.h](../src/plugins/ExperimentalInstrumentHost.h), [src/instruments/InstrumentTrackController.h](../src/instruments/InstrumentTrackController.h).

---

## Playback bridge

- **`ExperimentalInstrumentPlaybackSnapshot`** is an immutable list of **`ExperimentalInstrumentPlaybackEntry`** `{ trackId, ExperimentalInstrumentHost*, InstrumentTrackController* }`.
- **Publisher**: message thread calls `PlaybackEngine::publishExperimentalInstrumentPlaybackSnapshot(...)` (`release`-store atomic `shared_ptr`).
- **Consumer**: audio callback `acquire`-loads and retains the `shared_ptr` for the block — same handoff discipline as the session snapshot.
- The callback **walks instrument rows in `SessionSnapshot` track order** and resolves playback entries **by `TrackId`**. Mixed audio/instrument order in the snapshot is intentional; engine logic must handle **every** instrument row — **never** regress to “first instrument only.”

**Pointers:** [src/engine/PlaybackEngine.h](../src/engine/PlaybackEngine.h) (`ExperimentalInstrumentPlaybackEntry`, `ExperimentalInstrumentPlaybackSnapshot`), [src/engine/PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp).

---

## Project file (schema v13)

- **`ProjectFileV1::kCurrentVersion`** is **13** ([src/io/ProjectFile.h](../src/io/ProjectFile.h)).
- **`tracks[]`** persists **mixed lane order** and, for v13+, per-row **`kind`** (`"audio"` / `"instrument"`; absence reads as audio).
- **`experimentalInstrumentTracks[]`** holds Groove/experimental payloads; for v13+ each row binds with **`trackId`** to a timeline instrument lane.
- **Pre-v13 projects**: `migrateProjectFileExperimentalInstrumentLanePreV13` in [src/io/ProjectFile.cpp](../src/io/ProjectFile.cpp) may **append** an instrument shell track and bind a legacy payload; extra experimental rows beyond the first supported binding can be dropped with a log line.

---

## MIDI lane and editor

- Per-track **MIDI event lanes** and **instrument header** widgets are owned by [`InstrumentTimelineRowCoordinator`](../src/app/InstrumentTimelineRowCoordinator.h) (private `MidiEventLane` + `TrackHeaderView` maps), embedded in `TrackLanesView`. They close over that lane’s **`TrackId`** (host/controller lookup via `InstrumentRuntimeCoordinator`).
- **MIDI editor orchestration** lives in [`MidiEditorPresenter`](../src/app/MidiEditorPresenter.h); the **window** is [`ExperimentalMidiEditorWindow`](../src/ui/experimental/ExperimentalMidiEditorWindow.h). Opening a clip rebinds so the editor uses the correct host/controller for that track (single window instance, track-scoped binding — not “whichever instrument is global”).

**Pointers:** [src/app/MainAppWindow.cpp](../src/app/MainAppWindow.cpp) (wiring), [src/app/InstrumentTimelineRowCoordinator.cpp](../src/app/InstrumentTimelineRowCoordinator.cpp), [src/app/MidiEditorPresenter.h](../src/app/MidiEditorPresenter.h), [src/ui/experimental/ExperimentalMidiEditorWindow.h](../src/ui/experimental/ExperimentalMidiEditorWindow.h).

---

## Undo / redo (message thread)

- **[`UndoRedoCoordinator`](../src/app/UndoRedoCoordinator.h)** owns **`SessionHistory`** (undo-1 stack), orchestrates **Ctrl+Z / Shift+Ctrl+Z** and **executeUndoable**\* / **commitInstrumentMusicalUndoPair** recording, and runs **restore → plugin chain → instrument musical state → UI refresh** in a fixed order.
- It also owns **`PluginInsertHost` wiring**: **`setUndoRecorder`** (plugin-parameter steps) and **`setEditorShortcutCallbacks`** (undo/redo while a plugin editor is focused). The coordinator **constructor** registers these; the **destructor** clears them (`nullptr` / empty callbacks).
- **`TransportControlsContent`** ([`MainAppWindow.cpp`](../src/app/MainAppWindow.cpp)) **delegates** undo/redo to the coordinator (thin forwards / lambdas); it does not keep a parallel `SessionHistory`.
- **Project load / new session**: [`ProjectIoCoordinator`](../src/app/ProjectIoCoordinator.h) calls a **clear history** callback that forwards to **`UndoRedoCoordinator::clearHistory()`** so the stack matches the loaded file (same as clearing before the extraction).

---

## Power (Off) vs Mute

- **Power / Off** is **structural** (`Track::trackOff_` in the snapshot): engine skips the lane. It **must not** toggle while **playing, recording, or count-in** (UI no-op; no session write).
- **Mute** is **realtime** (`trackMuted_`): effective silence while still running the lane’s processing path as designed.
- **Power visuals** reflect only real On/Off: **On = green**, **Off = grey**. Do not add a third “locked” dim state for power.

**Steering history:** [status/DECISION_LOG.md](../status/DECISION_LOG.md) (2026-05-01 — Track Off vs Mute).

---

## Rescan (instrument header)

- **Rescan** is an out-of-process VST3 **description** scan path (not the in-process insert picker scan). It is invoked from the **instrument track header** context menu.
- **Known risk**: the child process can crash or exit badly (e.g. during `findAllTypesForFile`). That fix is **out of scope** until explicitly scheduled.
- **Required behavior today**: on failure, **parent** must **not** corrupt or replace the **loaded instance** or working cache; user messaging should state that existing state was left unchanged.

**Pointers:** [src/app/Vst3PluginPickerCoordinator.h](../src/app/Vst3PluginPickerCoordinator.h) (`runExperimentalInstrumentPluginDescriptionRescanForTrack`), [src/plugins/Vst3ChildProcessScan.cpp](../src/plugins/Vst3ChildProcessScan.cpp).

---

## Diagnostics and logging

- **`experimental-playback-routing.log`** and **`experimental-instrument.log`** (`%APPDATA%\MiniDAWLab\`) are **compile-gated**: `MINIDAW_DIAG_PLAYBACK_ROUTING` and `MINIDAW_DIAG_INSTRUMENT_LIFECYCLE` in [`src/diagnostics/DiagnosticBuildFlags.h`](../src/diagnostics/DiagnosticBuildFlags.h), both compiling to **`0`** by default — **never** architectural guarantees.
- Other `%APPDATA%\MiniDAWLab\` diagnostics (undo / drum name configs, scans, etc.) are also **implementation detail**, not contracts; prefer **compile-time or config gates** for anything that affects normal users.

---

## Naming debt (do not mistake for design)

Legacy identifiers and strings may still say **Experimental**, **primary**, **canonical**, **single slot**, or **global slot**. They reflect **history and file-format keys**, not the current model.

**Current model**: **TrackId-keyed** registries, **one host per instrument track**, **snapshot-ordered** instrument rows, **v13** project binding.

When adding features, **follow this document and the code paths above**, not the oldest comment or helper name. If a name still encodes singleton behavior (e.g. “first instrument in session” helpers), treat it as **technical debt** until renamed or removed in a dedicated cleanup slice.

Some **API/class header comments** may lag multi-instrument reality (e.g. wording that implies a single experimental shell). Prefer **implementation behavior** and this document over stale prose until a doc-only cleanup aligns comments.

---

## Explicit non-goals (this document)

- Does not define **HALion** or broad third-party instrument policy (future work).
- Does not prescribe further **transport / MainAppWindow** decomposition (planned refactors are separate).
- Does not replace [PHASE_PLAN.md](PHASE_PLAN.md) for historical phase narrative.

---

## Quick file index

| Concern | Primary files |
|--------|----------------|
| Track kind, snapshot tracks | [Track.h](../src/domain/Track.h), [SessionSnapshot.h](../src/domain/SessionSnapshot.h) |
| Session publish | [Session.cpp](../src/domain/Session.cpp) |
| Instrument host | [ExperimentalInstrumentHost.h/.cpp](../src/plugins/ExperimentalInstrumentHost.h) |
| Instrument controller | [InstrumentTrackController.h/.cpp](../src/instruments/InstrumentTrackController.h) |
| Playback + instrument snapshot | [PlaybackEngine.h/.cpp](../src/engine/PlaybackEngine.h) |
| Project v13 + migration | [ProjectFile.h](../src/io/ProjectFile.h), [ProjectFile.cpp](../src/io/ProjectFile.cpp) |
| Composition / registry / UI wiring | [MainAppWindow.cpp](../src/app/MainAppWindow.cpp), [InstrumentRuntimeCoordinator](../src/app/InstrumentRuntimeCoordinator.h), [InstrumentTimelineRowCoordinator](../src/app/InstrumentTimelineRowCoordinator.h), [ProjectIoCoordinator](../src/app/ProjectIoCoordinator.h), [MidiEditorPresenter](../src/app/MidiEditorPresenter.h), [UndoRedoCoordinator](../src/app/UndoRedoCoordinator.h) |
