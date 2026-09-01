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
- **`Track::kind`** is `TrackKind::Audio`, `TrackKind::Instrument`, `TrackKind::Midi`, `TrackKind::Group`, or `TrackKind::Master` (Stereo Out). Instrument lanes are **first-class domain rows**, not UI-only decorations. **Midi** rows hold MIDI clips but no plugin and no audio path; they feed an Instrument row via **MIDI To** (see [MIDI tracks](#midi-tracks-and-many-to-one-routing-v18)). Exactly one **Master** row exists per snapshot (final output bus; no timeline clips). **Group** rows are internal summing buses (no timeline clips).
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

## Project file (schema v19 today)

- **`ProjectFileV1::kCurrentVersion`** is **19** ([src/io/ProjectFile.h](../src/io/ProjectFile.h)). **v19** adds optional `experimentalInstrumentTracks[].clips[].ccPoints` — sparse MIDI CC automation points (`startTick`, `controller` 0–127, `value` 0–127, native `channel` 1–16, `interp` = `"hold"`/`"linear"`), omitted when empty; v18-and-older projects contain no key and load with no CC automation and identical sound (see [MIDI CC automation](#midi-cc-automation-stage-d)). **v18** adds `tracks[].kind == "midi"` rows with optional **`midiTo`** (destination Instrument `trackId`; absent = None) and `experimentalInstrumentTracks[].kind == "MidiContent"` payloads for their clips. **v17** adds optional per-track **`midiChannel`** (see [MIDI output channel per track](#midi-output-channel-per-track-v17); absent = `Any`/preserve, which is how every pre-v17 project loads). **v16** adds `experimentalInstrumentTracks[].genericVst3Descriptor`. **v15** adds `tracks[].sends[]`. **v14** adds per-track **`output`** (`trackId` of a Group or Master) and `tracks[].kind` = `"group"` / `"master"`. **v13** introduced mixed `tracks[].kind` + `experimentalInstrumentTracks[].trackId`.
- **`tracks[]`** persists **mixed lane order** and, for v13+, per-row **`kind`** (`"audio"` / `"instrument"` / `"midi"` / `"group"` / `"master"`; absence reads as audio).
- **`experimentalInstrumentTracks[]`** holds Groove/experimental payloads; for v13+ each row binds with **`trackId`** to a timeline instrument lane. For v18, `kind == "MidiContent"` rows bind by id to a `"midi"` timeline row instead (never resolved by name or position).
- **Pre-v13 projects**: `migrateProjectFileExperimentalInstrumentLanePreV13` in [src/io/ProjectFile.cpp](../src/io/ProjectFile.cpp) may **append** an instrument shell track and bind a legacy payload; extra experimental rows beyond the first supported binding can be dropped with a log line.

- **Additive-with-omitted-default is the established migration shape here.** `sends` (v15) and `midiChannel` (v17) both persist only when non-default and read as the legacy-preserving default when absent, so no rewriting migration function is needed. Prefer this over destructive migrations.

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

### MIDI output channel per track (v17)

- **The channel is a property of the track, not of the note.** `Track::getMidiOutputChannel()` ([src/domain/Track.h](../src/domain/Track.h)) is either `kTrackMidiOutputChannelAny` (**0** — preserve whatever channel each event already carries) or a **fixed 1 … 16** that every outgoing event from that row is remapped to. `sanitizeTrackMidiOutputChannel` repairs anything else. New tracks default to **channel 1**.
- **Why this exists.** Before v17 the MIDI editor stamped **channel 10 on every note it created** (`kCreateNoteChannel`), and the whole pipeline faithfully carried that per-note value through to `juce::MidiMessage::noteOn`. Channel 10 is the General MIDI *drum* convention, so multi-timbral melodic instruments never heard anything: GSi VB3-II listens on channel 1 (Upper), 2 (Lower) and 3 (Pedal), and simply stayed silent. The fix is a per-track channel, not a new hardcoded constant.
- **Remapping happens once, on the message thread.** `InstrumentTrackController::publishRenderSnapshot()` resolves the row's channel and bakes it into every `InstrumentNoteRenderEvent::midiChannel`. The audio thread therefore does no session lookup, and **realtime playback, offline mixdown, the deferred note-off queue (`rtPendingOffs_`) and transport panic all inherit the remap for free** because they consume the same events. `InstrumentTrackRenderSnapshot::midiChannel` is a diagnostic copy only. Changing the channel in the Inspector republishes the snapshot (`refreshMidiOutputChannelFromSession`), so notes already on the timeline move to the new channel; notes *currently sounding* keep the channel they started on, and their queued note-offs carry that same channel, so a mid-note change cannot strand them. All Notes Off already sweeps channels 1 … 16.
- **Drums are chosen explicitly, never inferred.** Only the **Add Instrument Track → Groove Agent SE** creation path selects channel 10 (`kTrackMidiOutputChannelDrums`), because that menu entry *is* the drum instrument. Nothing looks at track or plugin names. New notes created in the editor are stamped with the track's fixed channel when it has one, so stored channel and audible channel agree; on an `Any` row the editor still stamps 10, matching the notes already in migrated projects.
- **Migration is non-destructive.** `ProjectFileV1::kCurrentVersion` is **17**, adding optional `tracks[].midiChannel`. The key is **omitted when the value is `Any`**, and every pre-v17 project loads as `Any` — so old projects keep playing exactly as before (their notes still carry channel 10), multi-channel MIDI imports keep the channel assignment that *is* their musical content, and no note data is ever rewritten. A pre-v17 project re-saved by a v17 writer contains no `midiChannel` key at all. The user can move a migrated row from `Any` to a fixed channel whenever they want.
- **MIDI channel is not audio routing.** The Inspector therefore captions them **`MIDI Channel`** and **`Audio Output`**; neither is called just "Output". `MIDI Channel` is shown on `TrackKind::Instrument` rows only, and commits undoably through `TrackLanesEditCoordinator` like every other routing control. Beware that `Track::channelFaderGain` is a *mixer gain* and has nothing to do with a MIDI channel despite the name.
- **Footgun:** `Track` is rebuilt positionally by ~25 copy-on-write sites in [SessionSnapshot.cpp](../src/domain/SessionSnapshot.cpp) and [SessionRouting.cpp](../src/domain/SessionRouting.cpp). A site that forgets to pass the channel silently resets it to the ctor default of 1 — which is exactly how the first attempt at this slice broke legacy migration. Any new per-track field must be threaded through **all** of them, including the routing/send repair helpers.

### Native vs effective channel: inspection and explicit baking

- **Three separate concepts, deliberately not collapsed.** *Native channel* = `TimelineMidiNote::channel`, stored in the note. *Track output channel* = `Track::getMidiOutputChannel()` (`Any` or fixed 1 … 16). *Effective channel* = what the plugin receives after the track setting is applied. A fixed track channel therefore **hides** whatever native mixture a track contains, and switching back to `Any` reveals it again. All of this is defined once, as pure functions, in [`ExperimentalMidiChannelDiagnostics.h`](../src/ui/experimental/ExperimentalMidiChannelDiagnostics.h) — including the rule for which channel new notes are stamped with (`channelForNewNotes`), so the roll no longer restates it.
- **The mixture is intentional and useful.** One instrument track may hold several native channels; that is how a multi-timbral plugin is driven from a single instance. VB3-II: Upper = 1, Lower = 2, Pedal = 3 — draw each part with the track on the matching fixed channel, then set the track to `Any (Preserve)`. [MIDI tracks](#midi-tracks-and-many-to-one-routing-v18) make this a clearer arrangement model (three lanes, one instrument) without changing these semantics: each MIDI track has its own output-channel setting, applied before its events merge into the destination.
- **The editor reports it, read-only.** A `juce::Label` in the MIDI editor toolbar shows `Native Ch: … · N notes · Output …· Effective Ch: …` for the current selection, refreshed by the same 10 Hz poll as the `Vel` / `Off` / `Len` fields (selection changes from too many gestures to be worth a change callback). The native part is derived from the **note data**, never from the track selector — telling them apart is the entire point. Mixed sets wider than four channels collapse to bare `Mixed` with the full set in the tooltip so the control cannot push the toolbar around. With nothing selected it states only the track's output setting: an empty selection must not display a number the user could read as note data.
- **Baking the channel is explicit, scoped and undoable.** The toolbar's `Ch...` menu offers *Remap Selected Notes to Track Channel* and *Remap All Notes to Track Channel…*. Both require a **fixed** track channel — under `Any` there is no single target, so they are greyed out with the reason in a tooltip — and an empty selection is never read as permission to rewrite the track. Selected scope touches the open clip's selection; all scope iterates `InstrumentTrackController::getClips()`, i.e. **every clip on that track** (not the session). Only `channel` is written: time, duration, pitch, velocity, off-velocity, clip ownership, the selection and the track's own output selector are all left alone, and the notes are deliberately **not re-sorted** so the selection's note indices stay valid across the edit.
- **Remap goes through the normal mutation path**, so it inherits everything: one `executeUndoableInstrumentEdit` step (the musical undo snapshot is already track-wide, which is why an all-clips edit is still a single Undo), `markProjectDirty` → autosave, `notifyClipExperimentalMusicalTimingChanged()` → `publishRenderSnapshot()` for realtime and offline, and plain project I/O for persistence. No audio-thread mutation, locking or allocation is added.
- **Remap can be refused.** Channel is part of note *identity* (`timelineNotesSharePitchAndChannel`), so two overlapping notes on one pitch across different native channels are legal today but would become a single note once merged onto one channel. The batch is validated per clip through `validateTimelineNotesNoOverlap` (originals grandfathered) and refused whole, with an explanation — the same all-or-nothing contract as paste, multiselect resize and the `Len` field. This is a real limitation for stacked legacy material: the user must move or shorten the notes first.
- **In-app help.** `Help → MIDI Channels...` opens a scrollable read-only page ([`MainAppDialogs.cpp`](../src/app/MainAppDialogs.cpp), `midiChannelsHelpBodyText`) covering all of the above, channel 10 as an explicit choice rather than an inference, and legacy projects. It is the one place the wording `Any (Preserve)` is defined; the Inspector dropdown and the editor readout match it deliberately.

### MIDI export and the channel contract

- **Contract (implemented, Stage C):** export under `Any (Preserve)` writes **native** event channels; export under a **fixed** track channel writes the **effective** (remapped) channel — for every channel-voice event (Note On, Note Off, and Stage D CC events). Audio mixdown always uses the effective channel, exactly like realtime playback.
- **How:** [`InstrumentMidiClipExport.h/.cpp`](../src/io/InstrumentMidiClipExport.h) is split into `buildInstrumentMidiClipMidiFile` (in-memory SMF1 builder — the testable seam) and the file-writing `exportInstrumentMidiClipToMidiFile` wrapper; both take `sourceTrackMidiOutputChannel` and apply `midi_channel_diag::effectiveChannel` per event, so exporter and engine share one channel rule. The channel belongs to the **MIDI source track** (the editor passes `ExperimentalPianoRollView::trackMidiOutputChannel()`), never the destination instrument of a routed MIDI-only track. Export builds an output representation only: stored notes, native channels, track settings and CC points are never mutated (asserted by selftests). Meta events (tempo, time signature, track name) are never remapped.
- **Remaining known limitation:** the fixed `4/4` time-signature meta event above (deliberately untouched).

**Pointers:** [src/app/MainAppWindow.cpp](../src/app/MainAppWindow.cpp) (wiring), [src/app/InstrumentTimelineRowCoordinator.cpp](../src/app/InstrumentTimelineRowCoordinator.cpp), [src/app/MidiEditorPresenter.h](../src/app/MidiEditorPresenter.h), [src/ui/experimental/ExperimentalMidiEditorWindow.h](../src/ui/experimental/ExperimentalMidiEditorWindow.h), [src/ui/experimental/ExperimentalMidiChannelDiagnostics.h](../src/ui/experimental/ExperimentalMidiChannelDiagnostics.h), [src/ui/experimental/ExperimentalMidiNoteLengthFormat.h](../src/ui/experimental/ExperimentalMidiNoteLengthFormat.h).

---

## MIDI tracks and many-to-one routing (v18)

- **`TrackKind::Midi` is a MIDI-only lane**: it owns MIDI clips (same clip model, editor and undo as an instrument lane) but has **no plugin, no host, no audio path** — no audio output routing, sends, or inserts. It contributes to the mix only by feeding an Instrument row.
- **MIDI To** (`Track::getMidiDestinationTrackId()`): identity-based (`TrackId`), `kInvalidTrackId` = None/disconnected. Legal destinations are existing `TrackKind::Instrument` rows only; `Session::setTrackMidiDestination` and load-time repair (`session_routing::repairMidiDestinationsInPlace`) enforce this — a stale id is repaired to None, never retargeted. **Many-to-one is the point**: several MIDI tracks may feed one instrument (each with its own [output channel](#midi-output-channel-per-track-v17) baked in before the merge); one MIDI track feeds at most one instrument.
- **Runtime**: a plugin-less **MIDI content controller** — an `InstrumentTrackController` constructed with a **null host** — keyed in `InstrumentRuntimeCoordinator::midiContentControllersByTrackId_`, separate from instrument runtimes. It is never a playback-bridge *instrument* entry; instead the bridge snapshot carries it in **`ExperimentalInstrumentPlaybackSnapshot::midiSources`** (`{trackId, controller}`, published in session track order).
- **Engine merge (audio thread, [PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp))**: each block, after every instrument entry schedules its own lane's events, MIDI sources are walked **in snapshot order** (deterministic merge order). Each source resolves its destination **per block** from the *current* session snapshot (`findTrackIndexById` → `getMidiDestinationTrackId`) and schedules its transport MIDI into the destination's host. Unresolvable destination = silent. No graph solving, allocation, or locking is added.
- **Note lifecycle**: sounding notes belong to the **source** controller (its pending note-off queue), not the destination. A **reroute** mid-playback first flushes that source's pending note-offs into the **old** destination's host (never `allNotesOff` — the destination may sustain other sources), then routes new events to the new destination. **Stop** flushes all MIDI sources' pending offs into their last-routed destinations before the per-instrument transport flush. **Delete** of either end goes through the same snapshot/registry republish protocol as instrument runtimes (publish-before-destroy, drain in-flight callback).
- **Offline mixdown** runs the identical destination resolution and merge order, so a bounce renders routed MIDI exactly like realtime.
- **UI**: the add-track menu has **Add MIDI Track**; the Inspector shows **MIDI To** (Instrument rows + "No destination") and **MIDI Channel** for Midi rows, and hides audio output routing. Opening the MIDI editor on a Midi-track clip resolves the *destination's* host for preview/readouts; with no destination set, an alert explains instead. Rerouting or deleting the destination while the editor is open on that track rebinds/closes it (`resetMidiEditorBookingIfOpenOnTrack`).
- **Persistence (v18)**: `tracks[].kind == "midi"` + optional `midiTo` (absent = None); clips persist as `experimentalInstrumentTracks[]` rows with `kind == "MidiContent"`, bound strictly by `trackId` on load. Additive-with-omitted-default, like v15/v17 — pre-v18 projects contain neither key and load unchanged.
- **Deterministic test seam**: `ExperimentalInstrumentHost::MidiDeliveryCaptureSink` (install via `installMidiDeliveryCaptureSinkForTests`) observes the exact merged `juce::MidiBuffer` at the host's processing boundary — with a sink installed the boundary runs **without any loaded plugin**, so routing is testable without a VST3. The **`--stability-midi-routing`** scenario ([StabilityScenarioRunner](../src/diagnostics/StabilityScenarioRunner.h)) builds the headline **many-to-one** fixture on a copy of the given project: one instrument destination with its **own** clip on channel 1 plus two routed MIDI sources ("Lower" fixed output channel 2 / native 5, "Pedal" fixed 3 / native 6, pitches including the range boundaries 0 and 127). It asserts **exact per-channel note-on counts** (channels 1/2/3 stay distinct, mask `0x07`), that the destination's processing boundary ran **once per block, not once per source** (sink block count vs `getMidiDeliveryBoundaryBlockCountRelaxed`), that stop flushes all note-offs, then runs an **offline mixdown parity pass** over the same capture assertions, and finally verifies the v18 save/reload roundtrip preserves both destinations, fixed output channels, native channels and exact stored pitches. **Stage D extends the fixture with CC11 automation** (destination's own clip, native channel 1; the "Lower" source stored on native channel 5) and asserts exact per-channel CC deliveries (`2/2/0` — effective channels, no repeat-flood, no leak onto the CC-free source's channel), that a CC never arrives **after** a Note On at the same sample offset **on its own channel** (the sink counts only channel-voice controllers < 120 — the stop flush legitimately sends All Notes Off/CC 123 on every channel), and the same CC assertions on the offline pass (realtime/offline CC parity).

**Pointers:** [src/domain/Track.h](../src/domain/Track.h), [src/domain/SessionRouting.cpp](../src/domain/SessionRouting.cpp), [src/app/InstrumentRuntimeCoordinator.h](../src/app/InstrumentRuntimeCoordinator.h), [src/engine/PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp), [src/plugins/ExperimentalInstrumentHost.h](../src/plugins/ExperimentalInstrumentHost.h), [src/io/ProjectFile.h](../src/io/ProjectFile.h).

---

## MIDI editor UX: title, destination status, full range and audition (Phase B.1)

- **Window title is generic and live**: `MIDI Editor — <track name>` built by [`MidiEditorTitleStatus.h`](../src/ui/experimental/MidiEditorTitleStatus.h) (pure, testable text builders; no internal labels like "I2", no legacy "(Drum hits)", no row-mode description). `MidiEditorPresenter` resolves the name by `TrackId` at bind time and installs a **title provider** the window's UI timer polls, so a track rename updates the open editor without a rebind.
- **The status line is destination-aware**: instrument tracks show their own plugin (`Instrument: VB3-II` / `No instrument loaded`); MIDI tracks never claim a plugin they must not own — they show `MIDI To: <dest> — <plugin>`, `No MIDI destination`, or `<dest>: No instrument loaded`. Resolution is `TrackId`-backed on the message thread (`resolveBoundTrackStatus` in [`ExperimentalMidiEditorWindow.cpp`](../src/ui/experimental/ExperimentalMidiEditorWindow.cpp)), refreshed by the existing UI poll so destination change/rename/plugin load all update live.
- **Full MIDI range**: all row modes cover pitches **0–127** (`kFullPitchLow`/`kFullPitchHigh` in [`ExperimentalPianoRollView.h`](../src/ui/experimental/ExperimentalPianoRollView.h)); Piano vs Drum is display only. Labels use JUCE's Cubase-compatible octave naming (MIDI 0 = C-2, 60 = C3, 127 = G8). The old per-kind clamps (~C0–C4) are gone from creation, dragging, hit-testing and audition alike; stored pitches are never rewritten (non-destructive — proven by the roundtrip assertions above). An editor without preserved view state seeds its vertical scroll to reveal existing notes, else centers around middle C (`seedDefaultVerticalScroll`).
- **Audition is destination-aware and gesture-scoped** ([`MidiEditorAuditionModel.h`](../src/ui/experimental/MidiEditorAuditionModel.h) — a pure scheduler with a fake-clock seam, driven by [`ExperimentalMidiPatternPlayer`](../src/ui/experimental/ExperimentalMidiPatternPlayer.h) via the same `enqueueMidiMessageFromMessageThread` path as everything else; MIDI tracks audition through their **MIDI To** destination host, never a plugin call from the GUI thread). Channel choice mirrors playback: `Any (Preserve)` uses the note's native channel, a fixed track channel uses the effective channel; key-strip/drum-row audition uses the channel a newly created note would get, then applies the track output semantics.
- **Two deliberate timing semantics**: clicking an **arranged note** is event audition — Note On at Mouse Down, Note Off at `max(default ≈ 850 ms, hold duration)` (`kArrangedAuditionMs`), never the arranged length. The **left piano keys and drum rows** are a manual test instrument — Note On/Off exactly on Mouse Down/Up with no minimum, using the editor's current `Vel`/`Off` values.
- **Ownership and cleanup**: each active preview is keyed by (channel, pitch) with its Note On-time channel captured for the matching Note Off; retriggering restarts the attack without letting a stale scheduled off cut the newer preview, and completion is per-note (no broad All Notes Off). Editor close and player rebuild release everything; window focus loss releases held gestures while quick-click tails still end via their scheduled off. Reroute/channel changes go through the existing Phase B source-specific flush.
- **Live gesture wiring (regression-hardened)**: `ExperimentalPianoRollView::mouseUp` ends the active audition gesture **first, before any early return** — this is the call that schedules the arranged-note Note Off (max rule) and releases key/drum previews exactly at Mouse Up. Its absence was a real shipped regression (previews stayed "held" forever; only focus-loss cleanup released them), so the player now also has a deterministic **integration seam**: `ExperimentalMidiPatternPlayer::TestSeams` (injected clock + MIDI-capture delivery boundary) lets `MiniDAWSelftests` drive the *real* player scheduling/dispatch and assert the actual dispatched `juce::MidiMessage`s. The production host binding (`enqueueMidiMessageFromMessageThread` + `hasInstrument`) lives in [`ExperimentalMidiPatternPlayerHostBinding.cpp`](../src/ui/experimental/ExperimentalMidiPatternPlayerHostBinding.cpp) (app target only) so the player TU links into the selftests without the plugin host.
- **Level-1 selftests**: `MiniDAWSelftests` (console target, [tests/selftest/MiniDAWSelftestsMain.cpp](../tests/selftest/MiniDAWSelftestsMain.cpp)) covers the title/status wording, octave labels 0–127, channel semantics, the full audition scheduler timing/cleanup matrix against a fake clock, and the audition **dispatch integration** matrix (quick click, >850 ms hold, key/drum exact lifetime, channel pairing, teardown/reroute release, retrigger isolation, no duplicate cleanup, independent simultaneous previews, CC-before-Note-On) — no audio device, UI or plugin.

**Pointers:** [src/ui/experimental/MidiEditorTitleStatus.h](../src/ui/experimental/MidiEditorTitleStatus.h), [src/ui/experimental/MidiEditorAuditionModel.h](../src/ui/experimental/MidiEditorAuditionModel.h), [src/ui/experimental/ExperimentalMidiPatternPlayer.h](../src/ui/experimental/ExperimentalMidiPatternPlayer.h), [src/ui/experimental/ExperimentalPianoRollView.h](../src/ui/experimental/ExperimentalPianoRollView.h), [src/app/MidiEditorPresenter.cpp](../src/app/MidiEditorPresenter.cpp).

---

## MIDI CC automation (Stage D)

- **Model, pure and clip-owned.** [`ExperimentalMidiCcAutomation.h`](../src/ui/experimental/ExperimentalMidiCcAutomation.h) defines `MidiCcPoint { startTick, controller 0–127, value 0–127, native channel 1–16, interpolationToNext = Hold | Linear }` and every rule as pure functions: `normalizePoints` (clamp, drop negative ticks, canonical sort, duplicate identity at the same controller/channel/tick resolved deterministically — **last wins**), `valueAtTick` (exact endpoints; Hold retains; Linear interpolates with round-half-up; **no value before a stream's first point** — no invented default like 0/64/127; the final point holds forever), and `collectCcEventsInTickRange` (the bounded discrete event set: a monotonic linear segment emits at most one event per crossed integer value, endpoints exact; an optional `lastSentValue` chase parameter suppresses unchanged repeats). Points live on `ExperimentalMidiPattern::ccPoints`, i.e. **inside the clip**, so move/duplicate/delete/cross-track-move of a clip carries its controller performance with zero extra lifecycle code, and the musical undo snapshot (which serializes whole clips) includes CC state automatically (`experimentalInstrumentClipMusicalEqual` compares it, so CC-only edits create undo steps).
- **Channel semantics are the note rules, verbatim.** Stored points keep a **native** channel; delivery and export apply the source track's `Any (Preserve)`/fixed setting through the same `midi_channel_diag::effectiveChannel` helper as notes. New points are stamped like new notes (`channelForNewNotes`). MIDI-only tracks route CC through **MIDI To**; the destination's own channel setting is never consulted for a routed source.
- **Realtime.** `InstrumentTrackController::publishRenderSnapshot` bakes points into per-`(controller, effective channel)` **`InstrumentCcRenderStream`s** of sample-positioned events (message thread). On the audio thread `audioThread_scheduleCcForSegment` runs **before** note scheduling each segment, so a CC at a note's start enters the `juce::MidiBuffer` (stable insertion order at equal offsets) ahead of that Note On; multiple sources merge in the established deterministic routing order (last deterministic event wins on conflicts). Per-stream `rtCcLastSentValue_` dedups repeats (no floods); it is forgotten on stop/reroute-flush and on snapshot revision bumps, forcing a fresh **chase**: at any discontinuity (playback start, seek, restart, loop wrap, offline render start) the latest event strictly before the segment start — which equals the evaluated curve value there — is emitted at the segment's first sample. Controllers are sticky: no reset value is ever sent on stop, mute or reroute, because none exists generically. Offline mixdown consumes the same snapshots (realtime/offline parity is asserted by the routing scenario).
- **Editor lane.** A collapsible controller lane sits below the velocity lane in [`ExperimentalPianoRollView`](../src/ui/experimental/ExperimentalPianoRollView.h) (collapsed by default with a labeled `CC` restore knob; resize band on its top edge). Header click opens the controller selector (controllers present in the clip first, then the named common set — CC1/2/4/7/10/11/64/65/66/67/91/93 — then all 128 grouped; **CC11 Expression is the default first view**). Rendering: 0–127 vertical range, points, linear ramps vs hold steps, held tail after the final point, distinct accent colour from velocity bars. Editing: click-empty inserts (snap-floor, value from height), drag moves in time (snap) and value, Shift extends selection, right-click = delete / Hold / Linear, Delete key removes selected points (checked **before** the note shortcut). All edits commit as single undoable steps through the same `undoablePatternEditHandler_` gateway as note edits (dirty/autosave inherited); live drags mutate the pattern and rewind before the undoable commit, exactly like velocity drags.
- **Audition.** Clicking an arranged note first evaluates every CC stream of the clip at that note's tick and enqueues the chased values (effective channels) via `ExperimentalMidiPatternPlayer::sendControllerChangeNow`, **then** begins the preview — the host's message-thread MIDI queue is FIFO, so CC precedes the audition Note On. The left piano keys remain a plain manual test instrument (Phase B.1 semantics untouched; no CC is implied there).
- **Export.** The Stage C builder emits CC automation as standard Control Change events (deterministic bounded ramps, exact endpoints, effective channels, added before notes so equal-tick CC precede Note Ons after the stable sort). `InstrumentMidiClipExportResult::ccEventsExported` reports the count.
- **Selftests.** `MiniDAWSelftests` covers validation/boundaries/duplicates, Hold/Linear/endpoint/before-first/after-final evaluation, bounded deterministic ramp generation, no-flood rescans, export CC correctness (channels/ticks/ordering/non-mutation), v19 round-trip and v18→v19 migration (`ProjectFile.cpp` is compiled into the selftest target). The `--stability-midi-routing` scenario asserts realtime + offline CC delivery and ordering at the capture seam.
- **Out of scope by design:** velocity→CC conversion, VST parameter automation, MPE, NRPN/RPN, SysEx editing, Bezier curves, external MIDI output.

**Pointers:** [src/ui/experimental/ExperimentalMidiCcAutomation.h](../src/ui/experimental/ExperimentalMidiCcAutomation.h), [src/instruments/InstrumentTrackController.h](../src/instruments/InstrumentTrackController.h), [src/io/InstrumentMidiClipExport.h](../src/io/InstrumentMidiClipExport.h), [src/ui/experimental/ExperimentalPianoRollView.h](../src/ui/experimental/ExperimentalPianoRollView.h).

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
| MIDI tracks + MIDI To (v18) | [Track.h](../src/domain/Track.h), [InstrumentRuntimeCoordinator.h](../src/app/InstrumentRuntimeCoordinator.h), [PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp) |
| Main output routing | [SessionRouting.cpp](../src/domain/SessionRouting.cpp), [RoutingPlanBuilder.cpp](../src/engine/RoutingPlanBuilder.cpp), [PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp) |
| Sends V1 (planned) | [sends_fx_reverb_plan](../../.cursor/plans/sends_fx_reverb_plan_9fa7a092.plan.md) |
| Composition root + coordinator map | [MainAppWindow.cpp](../src/app/MainAppWindow.cpp), [InstrumentRuntimeCoordinator](../src/app/InstrumentRuntimeCoordinator.h); see [App-layer coordinator map](#app-layer-coordinator-map) |
