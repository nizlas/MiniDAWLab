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
- **`Track::kind`** is `TrackKind::Audio`, `TrackKind::Instrument`, `TrackKind::Group`, or `TrackKind::Master` (Stereo Out). Instrument lanes are **first-class domain rows**, not UI-only decorations. Exactly one **Master** row exists per snapshot (final output bus; no timeline clips). **Group** rows are internal summing buses (no timeline clips).
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

## Project file (schema v14 today; v15 planned for sends)

- **`ProjectFileV1::kCurrentVersion`** is **14** ([src/io/ProjectFile.h](../src/io/ProjectFile.h)) at the time of this writing. **v14** adds per-track **`output`** (`trackId` of a Group or Master) and `tracks[].kind` = `"group"` / `"master"`. **v13** introduced mixed `tracks[].kind` + `experimentalInstrumentTracks[].trackId`.
- **`tracks[]`** persists **mixed lane order** and, for v13+, per-row **`kind`** (`"audio"` / `"instrument"` / `"group"` / `"master"`; absence reads as audio).
- **`experimentalInstrumentTracks[]`** holds Groove/experimental payloads; for v13+ each row binds with **`trackId`** to a timeline instrument lane.
- **Pre-v13 projects**: `migrateProjectFileExperimentalInstrumentLanePreV13` in [src/io/ProjectFile.cpp](../src/io/ProjectFile.cpp) may **append** an instrument shell track and bind a legacy payload; extra experimental rows beyond the first supported binding can be dropped with a log line.

**Planned v15 (sends slice, additive):** optional per-track **`sends`** array — see [Sends V1 (planned)](#sends-v1-planned). v14 projects load with no sends.

---

## MIDI lane and editor

- Per-track **MIDI event lanes** and **instrument header** widgets are owned by [`InstrumentTimelineRowCoordinator`](../src/app/InstrumentTimelineRowCoordinator.h) (private `MidiEventLane` + `TrackHeaderView` maps), embedded in `TrackLanesView`. They close over that lane’s **`TrackId`** (host/controller lookup via `InstrumentRuntimeCoordinator`).
- **MIDI editor orchestration** lives in [`MidiEditorPresenter`](../src/app/MidiEditorPresenter.h); the **window** is [`ExperimentalMidiEditorWindow`](../src/ui/experimental/ExperimentalMidiEditorWindow.h). Opening a clip rebinds so the editor uses the correct host/controller for that track (single window instance, track-scoped binding — not “whichever instrument is global”).

### MIDI timing model, granularity and minimum note length

- **Ticks are the only note time unit.** `TimelineMidiNote::startTick` / `durationTicks` are `std::int64_t` ticks in the clip's own PPQ (`ExperimentalMidiPattern::ticksPerQuarter`, default `kDefaultExperimentalTicksPerQuarter = 960`, saved per clip). Starts and durations share one unit and one granularity: **1 tick is representable everywhere** in storage, editing, project I/O and playback conversion. `durationTicks` is the held length; the overlap end is exclusive (`timelineNoteEndTick = start + max(1, duration)`), so end-to-start touching is legal.
- **Minimum note length is snap-derived, not a fixed constant.** `ExperimentalPianoRollView::minTimelineNoteDurationTicks()` returns the snap grid in ticks when snap is enabled, otherwise `ticksPerQuarter / 16` (**60 ticks = a 64th note** at 960 PPQ). Every editing path funnels through it: create, mouse resize (including multiselect, where the shared delta is clamped so no captured note falls below it), paste, and the typed **Len** field. `validateTimelineNotesNoOverlap` also rejects any candidate below the minimum, so the rule cannot be bypassed by a new caller that forgets to clamp. **Import and project load deliberately clamp only to `max(1, ticks)`**: a foreign file or an older project may legitimately contain shorter notes, and silently lengthening them would alter existing music. So the minimum is an *editing* floor, not a data invariant — code that assumes "every note is ≥ 60 ticks" is wrong.
- **Minimum granularity vs minimum length differ on purpose.** Granularity (1 tick) is what the model can *represent*; the minimum length is what interactive editing is *allowed to produce*. The gap exists because mouse editing at a musical grid should not be able to create notes too short to see, select or hear, while imported material must round-trip unharmed.
- **Snap is global.** One `SnapSettings` on `Session` drives both the arrangement and the MIDI editor (the editor's toggle/combo write straight to the session), so changing snap in one place changes the minimum note length in the other. This is existing intended behaviour, but it is the reason the minimum is not a constant.
- **The `Len` field is exact, not snapped.** Typed lengths use the entered musical value verbatim (subject only to the minimum and to r-unit granularity below); snap constrains *mouse* editing only. Because the minimum is snap-derived, snap still bounds how *short* a typed note may be — with snap at 1/16 the shortest typed length is a sixteenth. Format and conversion live in [`ExperimentalMidiNoteLengthFormat.h`](../src/ui/experimental/ExperimentalMidiNoteLengthFormat.h): `n.p.q.r` = bars . quarters . sixteenths . 120ths-of-a-sixteenth, a mixed-radix number where fields above their base carry (`0.0.0.120` == `0.0.1.0`), display is always normalized, and bar length comes from the project meter (`beatsPerBar`), so odd meters work (a 7/8 bar is 3.5 quarters = 3360 ticks at 960 PPQ). **r has 120 subdivisions** because 120 = 2³·3·5 makes triplets (40 r), quintuplets (24 r) and sextuplets (20 r) of a sixteenth exact whole numbers, which a power-of-two subdivision cannot express. One r unit is `ticksPerQuarter / 480` — **2 ticks at 960 PPQ** — so the field is coarser than storage: a duration that is not a whole number of r units displays rounded to the nearest r while the note keeps its exact ticks, and only a real commit writes the field's value back.
- **Batch note edits are all-or-nothing.** `applyLengthTicksToSelectedNotes` (like paste and multiselect resize) validates the whole candidate batch through `validateTimelineNotesNoOverlap` before writing anything: on conflict nothing changes, no undo step is recorded, and the roll flashes the forbidden cursor. Pairs of selected notes that *already* overlapped in an old project are grandfathered so a legacy clip is not frozen out of editing. Length edits change `durationTicks` only — start, pitch, channel, velocity and off-velocity are untouched — and land as one undo step via `executeUndoableInstrumentEdit`.
- **Known limitation:** MIDI *export* writes a fixed `4/4` time-signature meta event (`InstrumentMidiClipExport.cpp`) regardless of the project meter, so exported files misrepresent bar structure in other meters. The editor itself does not assume 4/4.

**Pointers:** [src/app/MainAppWindow.cpp](../src/app/MainAppWindow.cpp) (wiring), [src/app/InstrumentTimelineRowCoordinator.cpp](../src/app/InstrumentTimelineRowCoordinator.cpp), [src/app/MidiEditorPresenter.h](../src/app/MidiEditorPresenter.h), [src/ui/experimental/ExperimentalMidiEditorWindow.h](../src/ui/experimental/ExperimentalMidiEditorWindow.h), [src/ui/experimental/ExperimentalMidiNoteLengthFormat.h](../src/ui/experimental/ExperimentalMidiNoteLengthFormat.h).

---

## Current-time / playhead rendering (UI only)

- **One driver per window, and in the main arrangement one *moving layer*.** In the arrangement window, [`PlayheadOverlay`](../src/ui/PlayheadOverlay.h) is the **only** sampler of the shared [`UiPlayheadClock`](../src/ui/UiPlayheadClock.h) (owned by `TransportControlsContent`) **and the only component that draws a moving playhead**: its 60 Hz tick reads one display position per frame, stores it, and draws both the long lane line and the short ruler marker from that one value (see the buffering bullet below — [`TimelineRulerView`](../src/ui/TimelineRulerView.h) draws no marker and receives no per-frame pushes). **No `paint` reads time** — every paint draws only the stored frame value, so an externally triggered repaint (lane content below, window damage) reproduces exactly the column the dirty-rect bookkeeping knows about. Two independent samplers, or re-reading the clock inside `paint`, is what produced stale line columns and per-lane lead/lag. The MIDI editor's ruler stroke and grid line both read the piano roll's own smoothed `uiPlayheadDisplaySamples_`.
- **Smoothed clock.** `Transport::readPlayheadSamplesForUi` is published once per **audio block** (a staircase). `UiPlayheadClock` advances with wall-clock time and applies a small proportional correction (~8 %/tick) toward the published value, monotonic during playback; only real discontinuities (seek, cycle wrap, dropout, > 8192 samples) snap. Chasing every new block value re-anchors onto the staircase and reads as jitter.
- **One mapping and rounding.** Every indicator converts through [`playhead_pixel`](../src/ui/PlayheadPixelMapping.h): `x = originX + (sample - visibleStart) / samplesPerPixel`, then `floor(x) + 0.5` (pixel-centre) so 1–1.5 px strokes stay crisp and land in the same column.
- **Narrow invalidation.** Playhead frame advances invalidate the previous and new pixel column instead of the whole component; a frame that stays in the same column repaints nothing. Structural changes (locators, cycle, zoom/scroll, tempo/meter, ruler mode, bounds) still repaint fully. Lanes must **not** blanket-repaint at playhead cadence: the instrument `MidiEventLane`'s old 20 Hz playing-repaint forced the transparent overlay above it to redraw off-frame and was removed.
- **Timers:** arrangement overlay drives at **60 Hz** (ruler keeps a 30 Hz structural watcher); the piano roll uses **60 Hz** while playing and **6 Hz** idle. No repaint work runs on the audio thread, and none of this affects scheduling or the authoritative playhead.
- **Follow autoscroll is page/event-driven and globally budgeted, never frame-driven.** Both follow implementations (main arrangement in `MainAppWindow`, piano roll in `ExperimentalPianoRollView`) admit viewport pages through a per-window [`FollowAutoscrollGovernor`](../src/ui/FollowAutoscrollGovernor.h) **plus** the process-wide `GlobalFollowWorkCoordinator` (same header), because every follow page forces a full repaint of the scrolled surface. A page is admitted only when *all* hold: (1) the playhead crossed the comfort-band boundary of the current view; (2) follow is re-armed — after a page it re-arms only once the playhead was observed *inside* the band again, otherwise (extent clamp, fine zoom) it drops to a sparse re-arm interval (~750 ms) instead of re-firing every opportunity — the edge condition alone is never a standing pan permission; (3) capacity — the measured frame interval is on time and one clean frame passed since the last page; (4) no recent user viewport gesture in this window (~250 ms holdoff); (5) globally: at most one follow page across *all* windows per 250 ms, and follow in other windows yields ~1 s while the user pans/zooms somewhere (Follow stays visually ON; it just never fights interaction — this is also why a MIDI editor restored with Follow ON cannot overload the main window while the user works there). Wall-clock-only rate limits are forbidden here: wall time elapses *while the thread paints*, so an expensive pan re-arms such a limiter immediately and saturates the message thread (see `MAIN_FOLLOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md`). Explicit single-shot moves (seek snap, Follow toggled ON) bypass the gates but register with both objects. The roll additionally skips follow work while hidden/minimised. Main and MIDI editor Follow stay independent (project `mainWindow.followPlayhead` vs per-clip `midiRollFollowEnabled`).
- **The ruler and lane stack are buffered to images; playhead stripes are blits, not paints. This is a load-bearing responsiveness fix — do not remove it.** `TimelineRulerView` and `TrackLanesView` call `setBufferedToImage(true)`, so JUCE caches each fully rendered subtree (ticks/labels/locators; grid + audio lanes + instrument row + headers) and the transparent `PlayheadOverlay`'s ~60 Hz stripe invalidations redraw from the cached image instead of re-running any content `paint`. **Playhead movement must never cause `ClipWaveformView`, `MidiEventLane`, `TrackLanesView`, or `TimelineRulerView` content paints at playback rate.** That steady-state repaint tax — not Follow, not JUCE wheel handling, not the shared clock, not zoom math — was the confirmed root cause of the main-window playback-dependent zoom freeze: the freeze reproduced with Follow OFF and a stable viewport, while the MIDI editor (which has no transparent full-column overlay above its content) stayed responsive under playback + Follow + fast zoom. Consequences future changes must respect:
  - The overlay's bounds include the ruler band, and it draws the short ruler marker segment (`kRulerPlayheadMarkerLengthPx`) plus the long lane line below the band from the same frame value. The ruler must **not** paint a marker or take per-frame pushes: a marker baked into the ruler's buffer goes stale, and per-frame pushes reintroduce the tax.
  - Any code that changes lane or ruler *visuals* must call `repaint()` on the affected component explicitly. A child's own `repaint()` still invalidates the matching region of the parent buffer, so all existing content-change paths keep working — but you can no longer rely on the overlay's stripes happening to expose stale child content, which used to hide missing invalidations.
  - Cost of the design: one viewport-sized ARGB image per buffered subtree, and one extra image copy per full repaint. Both are deliberate and cheap next to the paint storm they replace. History and diagnostics: `MAIN_WINDOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md`.
- **Main-arrangement paint work is culled by the dirty clip region.** During playback the transparent `PlayheadOverlay` invalidates narrow stripes at 60 Hz over the whole lane column, and (when a content repaint has invalidated the buffered image, see above) every component under the stripe repaints. All main-window paint bodies therefore bound their content loops by `juce::Graphics::getClipBounds()` (zoom-freeze fix, see `MAIN_WINDOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md`): `TimelineRulerView`/`TimelineLocatorPainter` tick/label/grid loops take a cull sample window (musical *labels* still run their full placement loop so stateful spacing decisions match full repaints — only the final `drawText` is culled); `MidiEventLane` skips clips outside the dirty rect, culls note bars by a precomputed tick window, and caches each clip's preview pitch range (invalidated on controller change broadcasts); `ClipWaveformView` has a narrow-stripe fast path that only blits the existing wave raster plus dirty-culled chrome — a stripe paint must **never** sync strips, take the pyramid-cache mutex, or rebuild the raster (raster rebuilds happen only in full repaints, whose coalesced flush always follows any `samplesPerPixel` change; a stripe may thus blit a one-message-batch-stale raster, which is corrected within the same batch). New paint code in the arrangement column must keep stripe repaints O(dirty area).
- **Wave-raster rebuilds are deferred, never run inside paint for geometry changes.** `ClipWaveformView::paint` rebuilds its overscanned raster synchronously only when it has *no* raster yet or the *content* fingerprints changed (strips/pyramid — rare, correctness-first). Zoom (`samplesPerPixel`), pan beyond the overscan margin, and resize instead take a stale-blit path: `blitWaveRasterApproximate` maps the current visible range onto the old raster's coverage and draws it scaled (uncovered regions stay background, never stretched garbage; the mapping degenerates to a pixel-exact 1:1 blit in steady state, and the stripe fast path uses the same helper so stripes stay aligned while a rebuild is pending). Each stale paint restarts a per-lane one-shot timer (~200 ms, staggered ~35 ms per lane at construction), so an active wheel gesture rasterizes **zero** intermediate zoom levels and each lane rebuilds exactly once — for whatever the viewport says when the timer fires (the restart *is* the stale-generation guard) — in its own message-loop turn. Rebuilds also reuse the existing image allocation when dimensions are unchanged. This removed the remaining zoom-freeze spike: previously every wheel step's coalesced full repaint rebuilt (and reallocated, ~1.4 MB) every audio lane's raster synchronously in one turn.
- **Known visual tradeoff (accepted): waveforms are approximate during active zoom/pan.** While the user is zooming or panning the arrangement, audio lanes show the previous wave raster scaled to the new mapping, so waveforms can look slightly blurry or soft, and a rapid zoom-out can briefly show a clipped waveform where the old raster does not cover the new visible range. After ~200 ms of viewport idle each lane rebuilds once and the display becomes correct. This is intentional: it is what keeps the message thread responsive during zoom, and it is releaseable for current DAL goals. Improving waveform fidelity *during* the gesture (e.g. a background rasterizer or multi-resolution pyramid blit) is future polish, not a correctness blocker — a stale waveform that never refreshes after idle **is** a bug.
- **User viewport repaints are coalesced per message batch.** Wheel zoom/pan and drag-pan handlers must not call `repaint()` per input event: under a fast gesture the OS interleaves forced window repaints with the queued input events, so per-event dirty-marking costs one full recomposition *per event* and freezes the message thread for many seconds (the second follow-freeze root cause). Viewport *state* is applied immediately per event (geometry stays exact), but dirty-marking goes through [`CoalescedRepaintFlusher`](../src/ui/CoalescedRepaintFlusher.h) (a `juce::AsyncUpdater`): the main window flushes ruler + lanes + instrument row once per batch from the `TimelineViewportModel` listener, and the piano roll flushes its own full-roll repaint the same way. Follow pages flush synchronously (their frame tick already paints that turn).

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
- **`playback-ui-load.log`** (`MINIDAW_DIAG_PLAYBACK_UI_LOAD`, default **`0`**) writes **one aggregated line per second** from the transport UI timer: audio callback duration min/mean/max, percent of the block's realtime budget, near-overruns/overruns (`PlaybackEngine::snapshotAudioCallbackLoadAndReset`), plus UI playhead timer intervals and invalidated area (`PlayheadOverlay::snapshotUiRenderStatsAndReset`). Used to separate **UI render jitter** from **audio load**; the audio thread only touches relaxed counters.
- Other `%APPDATA%\MiniDAWLab\` diagnostics (undo / drum name configs, scans, etc.) are also **implementation detail**, not contracts; prefer **compile-time or config gates** for anything that affects normal users.

---

## Naming debt (do not mistake for design)

Legacy identifiers and strings may still say **Experimental**, **primary**, **canonical**, **single slot**, or **global slot**. They reflect **history and file-format keys**, not the current model.

**Current model**: **TrackId-keyed** registries, **one host per instrument track**, **snapshot-ordered** instrument rows, **v13** project binding.

When adding features, **follow this document and the code paths above**, not the oldest comment or helper name. If a name still encodes singleton behavior (e.g. “first instrument in session” helpers), treat it as **technical debt** until renamed or removed in a dedicated cleanup slice.

Some **API/class header comments** may lag multi-instrument reality (e.g. wording that implies a single experimental shell). Prefer **implementation behavior** and this document over stale prose until a doc-only cleanup aligns comments.

---

## Main output routing (implemented)

**Plan reference:** [routing_mixbus_master_plan](../../.cursor/plans/routing_mixbus_master_plan_08949036.plan.md).

- **Output routing** (`Track::routedOutputTrackId_`): every non-Master row routes its **main/dry** signal to **Stereo Out** or a **Group**. Groups may nest (e.g. Drum Group → Mixbus → Stereo Out). Validation prevents cycles ([`SessionRouting`](../src/domain/SessionRouting.cpp)).
- **Engine:** [`RoutingPlan`](../src/engine/RoutingPlan.h) + [`RoutingPlanBuilder`](../src/engine/RoutingPlanBuilder.cpp) build a topological bus order (child groups before parents, Master last) and preallocated bus scratch pointers. The audio callback **does not** solve the graph or allocate; it walks `sourceSteps` and `busSteps` only ([`PlaybackEngine`](../src/engine/PlaybackEngine.cpp)).
- **Sources** (Audio / Instrument) render into the scratch for their **output** destination bus. **Group** buses sum incoming scratch, apply their channel strip, and forward to their output bus. **Master** is the unique hardware sink.
- **Mixdown parity:** offline render uses the same `RoutingPlan` path as live playback.
- **UI:** Inspector **Output** dropdown lists legal Master + Group targets. Group/Master rows cannot host clips or recording; Group headers are bus-style (mute-only strip, no record arm).
- **Persistence:** v14 `tracks[].output.trackId` in [ProjectFile.cpp](../src/io/ProjectFile.cpp).

| Piece | Role |
|--------|------|
| `TrackKind::Audio` / `Instrument` | Sources; main output → Master or Group |
| `TrackKind::Group` | Internal bus; output → Group or Master; acyclic nesting |
| `TrackKind::Master` | Unique final sink to device / mixdown |
| `RoutingPlan` | Immutable plan: source steps, bus steps, bus scratch pool |

**Still deferred (routing-adjacent, not sends):** per-track **Input** (audio interface / MIDI per row — Inspector direction only until a dedicated slice), sidechain, PDC, multi-output instruments.

---

## Sends V1 (planned)

**Plan reference:** [sends_fx_reverb_plan](../../.cursor/plans/sends_fx_reverb_plan_9fa7a092.plan.md). **Not yet in code** at the time of this writing; domain/engine/UI slices follow this doc.

### Mental model

- **Output routing** decides where the channel’s **main/dry** signal goes.
- A **send** is an **additive, level-controlled copy** of the channel’s finished post-channel-strip stereo, scaled by send amount and summed into a **destination Group** bus. Sends do **not** replace or reroute the dry path.

Example: Lead Vocal dry → Vocal Group → Mixbus → Stereo Out; simultaneously Lead Vocal send → Plate Reverb Group → Mixbus → Stereo Out.

### V1 rules

| Topic | Rule |
|--------|------|
| Send sources | `TrackKind::Audio`, `Instrument`, **Group** |
| Send destinations | Existing **Group** rows only (FX/Reverb bus = normal Group by convention: user names it, puts plugins on Group inserts, routes Group output via existing output routing) |
| Master | **Never** sends; **never** a send destination |
| Data model | Variable-length `std::vector<TrackSend>` per non-Master row (no fixed slot cap in domain or JSON) |
| Tap point | **Post-channel-strip:** Pre inserts → fader / mute / off → Post inserts → pan → **send tap** → dry bus + each enabled send destination |
| Amount | Linear `[0, 2.0]` stored as `amountLinear` (unity = 1.0, max ≈ +6.02 dB); UI presents **dB** (`-inf` at 0, `0.00 dB` at unity) |
| Validation | Combined **output + send** DAG must stay acyclic; UI and Session setters reject cycle-creating destinations |
| Engine | `RoutingPlan::SourceStep::sends` and `BusStep::sends`; topo over output **and** send edges; one shared **post-channel-strip stage scratch** per step (render once, add to dry + each send dest); no audio-thread allocation or graph solving |
| Mixdown | Same send path as live playback |
| Persistence | **v15** additive: `tracks[].sends[]` with `{ destTrackId, amount, enabled, tap: "postChannelStrip" }` (omit key when empty; do not use `"postFader"` — ambiguous about Post inserts / pan) |

### Inspector (planned UI)

Section order (top → bottom), mirroring signal flow:

1. Track name / kind  
2. Channel volume  
3. Pan  
4. Output  
5. Inserts (Pre, Post)  
6. **Sends** (below Inserts)

- Visible on Audio, Instrument, Group. Hidden on Master.
- **Four fixed visible send rows** bound to the first four entries in the variable list; empty rows show `(none)`; sends beyond row 4 may persist and process but need not be fully editable in V1.
- Inspector-only: no track-header send controls, no mixer view in V1.

### Explicitly out of scope for sends V1

PDC, sidechain, send automation, pre/post tap toggle, multi-output instruments, per-track input routing, mixer view, MIDI editor / snap changes, new `TrackKind::Fx`.

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
| Project v13+ / v14 routing | [ProjectFile.h](../src/io/ProjectFile.h), [ProjectFile.cpp](../src/io/ProjectFile.cpp) |
| Main output routing | [SessionRouting.cpp](../src/domain/SessionRouting.cpp), [RoutingPlanBuilder.cpp](../src/engine/RoutingPlanBuilder.cpp), [PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp) |
| Sends V1 (planned) | [sends_fx_reverb_plan](../../.cursor/plans/sends_fx_reverb_plan_9fa7a092.plan.md) |
| Composition root + coordinator map | [MainAppWindow.cpp](../src/app/MainAppWindow.cpp), [InstrumentRuntimeCoordinator](../src/app/InstrumentRuntimeCoordinator.h); see [App-layer coordinator map](#app-layer-coordinator-map) |
