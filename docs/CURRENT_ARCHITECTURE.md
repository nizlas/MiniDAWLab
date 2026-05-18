# Current architecture (baseline)

This document states **what the MiniDAWLab codebase is today** — not phase history or roadmaps.

Use it alongside [ARCHITECTURE_PRINCIPLES.md](ARCHITECTURE_PRINCIPLES.md), [PROJECT_BRIEF.md](../PROJECT_BRIEF.md), and [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md).

---

## Composition and threads

- **Message thread**: UI, `Session` mutations that publish snapshots, plugin/host lifecycle, project I/O, device setup.
- **Audio thread**: `PlaybackEngine` callback only. It must not touch `Session`, allocate, block, or take locks. It reads **published** immutable views via atomics (see below).

---

## App layer: `MainAppWindow.cpp` (`TransportControlsContent`) — composition root

[`MainAppWindow.cpp`](../src/app/MainAppWindow.cpp) defines `mini_daw_app_transport::TransportControlsContent`, the inner transport timeline UI subtree. After modularization (**~988 lines**, down from a much larger monolith), its **intended role** is the **transport UI composition root** — not a dump for feature implementation.

**What it owns / does**

- **Top-level JUCE widgets and views** for the transport strip and timeline chrome (buttons, ruler, lanes, inspector, overlays, MIDI editor component handle, etc.).
- **Construction and member ownership** of **app-layer coordinators**, with **explicit member order** so reverse destruction frees coordinators safely while borrowed UI (`TrackLanesView`, `inspectorView_`, …) remains alive — see destructor-order comments beside `trackLanesEditCoordinator_` and related members.
- **Callback wiring** between coordinators (lambdas in the constructor that forward `transport`, `session`, recorder state, undo entry points, instrument runtime hooks, refresh signals, etc.).
- **Thin shims**: JUCE overrides (`resized`, `changeListenerCallback`, `timerCallback`, clipboard shortcut entry points surfaced through `TransportControlsShortcutTarget`), and **`createTransportUiForMainWindow`** as the factory return type that pairs UI + shortcut target ([`TransportControlsFactory.h`](../src/app/TransportControlsFactory.h)).
- **Legitimate cross-subsystem orchestration** kept local because it stitches multiple-owned UI pieces in one place, for example:
  - **`refreshInstrumentUi()`** — timeline row attachment sync, playback bridge / shell sync with `InstrumentRuntimeCoordinator`, lane header rebuild/repaint, optional MIDI editor refresh, layout.
  - **`syncViewportFromSession()`** — arrangement extent, default samples-per-pixel, viewport clamp tied to ruler width and device sample rate.
  - **`clearExperimentalInstrumentRuntimesPreserveBridgeOnly()`** — resets MIDI editor booking, clears instrument attachments on lanes, clears `InstrumentTimelineRowCoordinator` lanes/headers, then `InstrumentRuntimeCoordinator::clearRuntimesPreserveBridgeOnly()`. **This stays in the composition root** because it crosses MIDI editor + track lanes + instrument timeline rows + runtime cleanup.

**What it must not represent architecturally**

- **Not** where “the feature logic” for recording, project I/O, MIDI editor internals, VST picker flow, lane edit semantics, clipboard rules, undo stack implementation, etc. should accumulate — those belong in the **coordinators and domain modules** below (and callers should stay thin).

**Constructor and size**

- The **constructor is intentionally explicit** — a readable wiring block. Do **not** break it apart only to shorten the file unless a dependency truly changes.
- **~988 lines is acceptable** at the composition layer as long as the file stays **mostly ownership, wiring, and small orchestration**.

**Guidance for changes**

- **Prefer** implementing new behavior inside the **appropriate coordinator** (or a new small module under `src/app/`) and only **threading callbacks** through `TransportControlsContent` when integration requires.
- Keep code here only when it is **composition-root wiring**, **top-level shell behavior** (keyboard focus, splitter, dialogs entry), or **genuine multi-subsystem orchestration** like the helpers above.

**Standalone MainAppWindow refactor**

- Treat **standalone `MainAppWindow.cpp` refactoring as closed**: no further decomposition “for hygiene” unless a **concrete regression** or **feature-driven integration need** proves the split. Resume normal feature work instead.

### App-layer coordinator map

| Module | Responsibility |
|--------|----------------|
| [`ProjectIoCoordinator`](../src/app/ProjectIoCoordinator.h) | Project save/load orchestration |
| [`RecordingCoordinator`](../src/app/RecordingCoordinator.h) | Count-in, recording start/stop/commit, cycle recording |
| [`Vst3PluginPickerCoordinator`](../src/app/Vst3PluginPickerCoordinator.h) | VST3 picker / load / rescan flow |
| [`MidiEditorPresenter`](../src/app/MidiEditorPresenter.h) | MIDI editor window, binding, and transport/UI command orchestration |
| [`InstrumentRuntimeCoordinator`](../src/app/InstrumentRuntimeCoordinator.h) | Instrument host/controller registry, staging/keyed runtimes, playback bridge, device lifecycle hooks |
| [`InstrumentTimelineRowCoordinator`](../src/app/InstrumentTimelineRowCoordinator.h) | Instrument track header and MIDI lane row UI |
| [`ClipPasteboardController`](../src/app/ClipPasteboardController.h) | Selected clip delete / copy / paste |
| [`MainWindow`](../src/app/MainWindow.h) (`MainWindow.cpp`) | Document window shell and shortcut routing to the shortcut target |
| [`TransportControlsFactory.h`](../src/app/TransportControlsFactory.h) (`createTransportUiForMainWindow` lives in [`MainAppWindow.cpp`](../src/app/MainAppWindow.cpp)) | Builds transport UI content and exposes the shortcut target |
| [`MainAppDialogs`](../src/app/MainAppDialogs.h) | Audio settings, Help, undo-behavior dialogs |
| [`AudioClipImportCoordinator`](../src/app/AudioClipImportCoordinator.h) | Add/import audio clip at playhead flow |
| [`UndoRedoCoordinator`](../src/app/UndoRedoCoordinator.h) | `SessionHistory`, undo/redo, plugin undo recorder, editor shortcut callbacks |
| [`TrackLanesEditCoordinator`](../src/app/TrackLanesEditCoordinator.h) | Track delete/reorder and clip move / trim / split callback wiring |
| [`TransportLayoutHelper`](../src/app/TransportLayoutHelper.h) (`TransportLayoutHelper.cpp`) | Shared layout geometry for transport controls strip |
| [`TransportPlayPauseStopController`](../src/app/TransportPlayPauseStopController.h) | Play / pause / stop behavior vs transport and recorder |
| [`PluginHostUiBindings`](../src/app/PluginHostUiBindings.h) | Wiring `PluginInsertHost` callbacks into track headers and inspector |
| [`InstrumentMusicalUndoSnapshot`](../src/app/InstrumentMusicalUndoSnapshot.h) (namespace helpers) | Build/sort payloads for instrument **musical** undo snapshots |
| [`AddInstrumentTrackCoordinator`](../src/app/AddInstrumentTrackCoordinator.h) | Groove Agent “add instrument track” menu flow |

**Housekeeping unrelated to layering**

- Do **not** change **`kDrumNamesDiag`** (or drum-name diagnostics behavior) unless the user explicitly requests it — see code that references that symbol.
- **`Experimental*`** identifiers and broader renames tied to **project format, undo, or playback semantics** were **not** migrated in this pass; treat renames as a **separate, deliberate slice** ([Naming debt](#naming-debt-do-not-mistake-for-design) still applies).

---

## Session and snapshot

- **`SessionSnapshot`** is **immutable**. Every session edit builds a new snapshot and publishes it with a single atomic store (`Session::sessionSnapshot_`, `memory_order_release`).
- **`SessionSnapshot::tracks_`** is the **canonical ordered list** of timeline lanes. Row order is what the user sees (headers + lanes) and what undo/redo and project save preserve.
- **`Track::kind`** is `TrackKind::Audio`, `TrackKind::Instrument`, or `TrackKind::Master` (Stereo Out). Instrument lanes are **first-class domain rows**, not UI-only decorations. Exactly one **Master** row exists per snapshot (final output bus; no timeline clips).
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

## Project file (schema v14)

- **`ProjectFileV1::kCurrentVersion`** is **14** ([src/io/ProjectFile.h](../src/io/ProjectFile.h)). **v14** adds `tracks[].kind` = `"master"` (Stereo Out row). **v13** introduced mixed `tracks[].kind` + `experimentalInstrumentTracks[].trackId`.
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
- **`TransportControlsContent`** ([`MainAppWindow.cpp`](../src/app/MainAppWindow.cpp)) **wires** the coordinator in its constructor — lambdas attach **shortcut targets**, **session/instrument** undo entry points, **musical** snapshot **build/sort/apply** (via [`InstrumentMusicalUndoSnapshot.h`](../src/app/InstrumentMusicalUndoSnapshot.h) helpers + [`InstrumentRuntimeCoordinator`](../src/app/InstrumentRuntimeCoordinator.h)), and post-restore refresh hooks — but **never** duplicates `SessionHistory` or undo stack semantics.
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

## Routing (main output — in progress)

**Plan reference:** [routing_mixbus_master_plan](../../.cursor/plans/routing_mixbus_master_plan_08949036.plan.md) (accepted architecture; implementation is sliced).

### Current (Slice A + B1 + B2)

- **Before:** audio and instrument tracks summed **directly** into the device output buffer (implicit master).
- **Now:** sources still sum in timeline order, but into a **stereo master scratch** owned by [`PlaybackEngine`](../src/engine/PlaybackEngine.cpp); the **`TrackKind::Master`** row (“Stereo Out”) applies its channel strip (Pre/Post inserts, fader, mute/off, pan) and writes the **only** signal to the device / offline mixdown buffer.
- **Master row rules:** one per session, normally last in `tracks_`, non-deletable, no clips, no record arm. Pre-v14 projects gain a Master row on load (migration in [`ProjectFile.cpp`](../src/io/ProjectFile.cpp)).
- **Plugin inserts** on Master use the existing [`PluginInsertHost`](../src/plugins/PluginInsertHost.h) `TrackId` map (no separate hidden master object).

### Accepted future model (not all implemented yet)

| Piece | Role |
|--------|------|
| `TrackKind::Audio` / `Instrument` | Sources with optional **output** to Master or a **Group** |
| `TrackKind::Group` | Internal bus; nested Group→Group allowed; graph must be acyclic |
| `TrackKind::Master` | Unique final sink to hardware |
| `RoutingPlan` | Message-thread-built topological order + preallocated bus scratch; audio thread reads plan only |
| Per-track **Input** | Audio interface input / MIDI input per row (Inspector direction only until a dedicated slice) |

### Explicitly deferred (do not assume in code reviews yet)

Group tracks, `routedOutputTrackId_` / output dropdown, full `RoutingPlan` graph, sends (pre/post fader), FX/reverb buses, sidechain, PDC, per-track input wiring, nested-group UI polish beyond the data model.

---

## Explicit non-goals (this document)

- Does not define **HALion** or broad third-party instrument policy (future work).
- Does not schedule further **standalone `MainAppWindow.cpp` / transport composition** refactoring for its own sake; that track is [**treated as closed**](#standalone-mainappwindow-refactor) unless a regression or integration need motivates a change — extend **coordinators** instead.
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
| Composition root + coordinator map | [MainAppWindow.cpp](../src/app/MainAppWindow.cpp), [InstrumentRuntimeCoordinator](../src/app/InstrumentRuntimeCoordinator.h); see [App-layer coordinator map](#app-layer-coordinator-map) |
