# Decision Log

This file records small but important implementation decisions that are useful to preserve between phases.

It is not a full chat log and not a replacement for the steering documents.  
It exists to capture concrete decisions, rationale, and limits that may matter later.

---

## 2026-04-30 — Cycle recording: continuous master capture, offline per-pass split, opaque preview, deferred master cleanup

**Scope:** Product behaviour and implementation shape for **cycle OD** (loop record) — **not** take lanes / comping UI, **not** a `ProjectFile` schema change for the **final** split model (per-pass clips are normal recorded assets like single-take takes). **RecorderService** realtime capture path (single continuous WAV, SPSC input, writer thread) **unchanged** in intent.

**During recording**

- **One continuous mono master WAV** is written for the whole cycle session so there are **no recording gaps** at loop boundaries (no stop/restart of the writer per lap).
- With **cycle enabled**, **playback / playhead** wrap between **left locator L** and **right locator R** on the audio thread; locators remain session metadata as before.

**On stop / commit (message thread only)**

- After **`stopRecordingAndFinalize()`** completes, the continuous master is **decoded once** and **split offline** into **one WAV per completed full pass** plus an optional **final partial** pass; each file is written under **`<projectFolder>/Audio/`** with a **`cycle_pass_<timestamp>_<index>.wav`** pattern (implementation detail; steering: independent files per pass).
- Each pass is committed via the **same path as a normal recorded take**: **`Session::addRecordedTakeAtSample`** at **timeline L** (left locator), **full file = visible material** (no shared “one long buffer” placement with material windows for **new** cycle commits). **Newest pass is topmost** in the track list (same “prepend as newest” rule as other added clips).
- **Recording preview (UI):** body and peaks are **fully opaque** — **no alpha-blended** active recording preview layers; older pass sketches must not show through the current pass in the drawn region.

**Continuous master after success**

- If **all** per-pass writes and session adds succeed, the app **deletes** the continuous **`take_*.wav`** master. If Windows still holds the file (brief lock), cleanup uses a **message-thread deferred retry** (timer), then **`_debug_cycle_continuous_<name>`** rename + log if delete still fails. **Failed** split/commit does **not** delete or rename the master.

**Alignment / latency**

- An **alignment audit** (runtime traces) found **split math sample-consistent** with device sample rate, **`i * passLen`** slice origins, and placement at **L**; **input monitoring / device latency compensation** remains **future work** (not implemented here).

**Explicit non-goals**

- **Take lanes / comping UI** — not implemented.
- **ProjectFile schema change** specifically for “cycle split” — not required; committed passes are ordinary clip entries referencing their own WAV paths.

Rationale: avoids fragile shared-material-window semantics for cycle takes, keeps gapless capture, and makes every committed pass a normal clip for trim/save/load.

---

## 2026-04-28 — Active-track Inspector channel fader + `ProjectFile` v5 (`channelFaderGain`)

**Scope:** `Track` / `Session` / `SessionSnapshot`, [PlaybackEngine](src/engine/PlaybackEngine.cpp), [InspectorView](src/ui/InspectorView.cpp), [ProjectFile](src/io/ProjectFile.cpp) — **not** recording path, clip import, per-track lane headers.

**Mix point:** Gain is applied at the simplified **track-output sum** into the main stereo buffer (`addWithMultiply` per clip run). Post-fader inserts, sends, or flexible routing **may later require explicit per-track channel buffers** — **not implemented** in this slice.

**Persisted field:** `ProjectFileTrackV1.channelFaderGain` — linear amplitude, **`0`** = minimum fader (**−∞** on the −60…+6 dB slider UI, not a separate mute). Unity **omitted when writing v5** when `abs(g - 1) ≤ 1e-6`; load clamps to `[0, kTrackChannelFaderGainMax]`. Pre-v5 tracks default **unity** on load.

**UI:** [InspectorView](src/ui/InspectorView.h) horizontal dB slider, numeric **"-∞ dB"** at bottom stop, reset **0 dB** / unity gain.

---

## 2026-04-27 — Windows packaging artifacts: `DanielssonsAudioLab-*` (exe/target unchanged)

**Scope:** [package-windows.ps1](scripts/package-windows.ps1), [MiniDAWLab.iss](installer/MiniDAWLab.iss), docs — no `src/**`, no CMake target rename, **`MiniDAWLab.exe`** and installer payload unchanged.

**Names:** Staged folder, zip, and Inno **`OutputBaseFilename`** use the ASCII token **`DanielssonsAudioLab-<version>`** (e.g. `dist\DanielssonsAudioLab-0.1.0.zip`, `dist\DanielssonsAudioLab-0.1.0-Setup.exe`). **Visible** installer strings remain **Danielssons Audio Lab** (`AppName`, shortcuts). The installed binary is still **`{app}\MiniDAWLab.exe`**.

**Prior:** Artifacts were named `MiniDAWLab-<version>.*`; the Inno `[Files]` `Source` path must stay aligned with the staging directory created by the packaging script.

---

## 2026-04-26 — Add clip: copy imports into `<ProjectFolder>/Audio/` (copy-only, no migration)

**Scope:** [io/ProjectAudioImport](src/io/ProjectAudioImport.cpp) + [Main.cpp](src/Main.cpp) add-clip path + [CMake](CMakeLists.txt) — **not** `Session`, `SessionSnapshot`, `PlacedClip`, `AudioFileLoader`, `ProjectFile` schema, `RecorderService`, `PlaybackEngine`, `Transport`, recording/count-in, audio device settings, or packaging.

**Trigger:** **Add clip…** (external file chooser) only. **Unsaved** project: alert *"Save the project before importing audio."* and **no** clip, **no** chooser result processed as import.

**Policy:** If the pick is **already in** `Audio/`, use that path (no second copy). Otherwise **copy** into `Audio/` with a unique name: `name.ext`, `name_1.ext`, `name_2.ext`, … (legal stem; empty stem → `clip`). The clip’s material references the path under `Audio/`. **Copy-only:** the original file elsewhere on disk is **not** deleted, moved, or modified.

**Legacy:** Project files that still reference **external** `sourcePath` values **load as before**; no automatic “collect media” or relink pass.

---

## 2026-04-26 — Audio Settings dialog: `AudioDeviceSelectorComponent` + per-user XML persistence (Stage 2)

**Scope:** [Main.cpp](src/Main.cpp) composition root, [AudioDeviceInfo](src/audio/AudioDeviceInfo.h) persistence helpers, [CMake](CMakeLists.txt) link to `juce_audio_utils` only — no `RecorderService`, `PlaybackEngine` render path, `Session` / `SessionSnapshot`, `ProjectFile`, `Transport`, count-in, packaging, or project load/save.

**UI:** A transport-row **"Audio…"** button opens a modal **`juce::DialogWindow` + `juce::AudioDeviceSelectorComponent`** (no custom selector). **MIDI** UI off (`false`, `false`). Input channel range **0..2**, output **2..2**; `showChannelsAsStereoPairs` **false**. **ASIO** / `JUCE_ASIO` not enabled in this slice (WASAPI/DirectSound only unless the user’s JUCE build already lists ASIO).

**Behaviour:** The dialog is **blocked** while `RecorderService::isRecording()` or count-in is active, with: *"Audio settings cannot be changed while recording or count-in is active."* If transport is **Playing**, it is set to **Stopped** before the dialog so device teardown does not fight playback. The live status line still comes from [describeActiveAudioDeviceOneLine](src/audio/AudioDeviceInfo.cpp) and **updates** on `AudioDeviceManager` `ChangeBroadcaster`.

**Persistence:** On every device broadcast, [trySaveAudioDeviceState](src/audio/AudioDeviceInfo.cpp) calls **`createStateXml()`**; if null, no-op; write failures are **log-only** (status label still refreshes). File: **`%APPDATA%\MiniDAWLab\audio-device.xml`**. On startup, **`initialise(1, 2, savedXml, true)`**; existing **output-only** fallback if init fails, unchanged in spirit from Stage 1.

**Recording path:** Mono capture from **`inputChannelData[0]`** in [PlaybackEngine](src/engine/PlaybackEngine.cpp) is **unchanged**; the selector only chooses *which* device and which channels the host exposes.

---

## 2026-04-26 — Windows shipping: zip + Inno Setup, bundled VC++ x64 redist, no file association (this slice)

**Scope:** packaging scripts, Inno script, and docs only — no runtime/audio/project code changes, no static CRT / CMake switch in this step.

**Distribution:** `scripts\package-windows.ps1` stages **`dist\DanielssonsAudioLab-<version>\`**, copies the **Release** `MiniDAWLab.exe` plus `README.md` / `PROJECT_BRIEF.md` when present, parses the version from **`CMakeLists.txt`**, and writes **`dist\DanielssonsAudioLab-<version>.zip`**. The **Microsoft Visual C++ 2015-2022 Redistributable (x64)** is downloaded **once** to **`dist\vendor\vc_redist.x64.exe`** (official `https://aka.ms/vc14/vc_redist.x64.exe`) and **embedded** in the Windows installer (no [Inno Download Plugin](https://mitrichsoftware.wordpress.com/2019/11/10/inno-download-plugin/), no per-install download).

**Installer:** **`installer\MiniDAWLab.iss`** (Inno Setup 6) — Start Menu shortcut always, **optional** desktop icon via a standard **Tasks/Icons** checkbox; run **`vc_redist.x64.exe /install /quiet /norestart`** during setup; small optional “launch when finished” **Run** entry. **No** `.dalproj` / project-file association in this slice (can be added later with explicit product decision).

**Automation:** If **`ISCC.exe`** is on `PATH` or in the default “Inno Setup 6” install path, the packaging script runs the compiler and emits **`dist\DanielssonsAudioLab-<version>-Setup.exe`**. Otherwise the zip and staged tree are still produced; the user compiles the `.iss` manually (see `installer\README.md`).

---

## 2026-04-26 — Project as folder (`.dalproj`) + `Audio/` for new takes

**Scope:** save/load UX, recording output path, docs only — no `ProjectFileV1` schema change, no file migration.

**Layout:** First-time **Save project** uses a DAW-like layout: **`<ParentChosen>/<ProjectName>/<ProjectName>.dalproj`**. After a project file is known, **Save project** writes to that file **without a chooser** (normal save). A separate explicit **Save As** / **New project** action is **deferred**.

**Extension:** New projects use **`.dalproj`**. **Load** accepts **`.dalproj` and `.mdlproj`** (legacy). Existing `.mdlproj` files and clips whose `sourcePath` points at old **`takes/`** WAVs continue to load; **no** automatic copy/move.

**Recording:** New takes are written to **`<projectFolder>/Audio/`** (not `takes/`). `Session::getCurrentProjectFolder()` is `getCurrentProjectFile().getParentDirectory()`.

**First-time save collisions (conservative):** If the target **`<ProjectName>.dalproj`** already exists, or if **`<Parent>/<ProjectName>/`** already contains **any** other `*.dalproj` / `*.mdlproj` that is not exactly the intended file path, **abort** with an alert — **no** silent overwrite and **no** confirmation dialog in this slice.

**Non-goals:** App / CMake rename (DAL branding), installer, file association, relative paths in the project file.

---

## 2026-04-26 — Phase 4 minimal mono recording (steering; implementation pending)

**Scope:** steering/validation only in this log entry. No `RecorderService` or device re-init in code until implementation begins.

**Transport ownership (approved):** The **root / main application composition** (typically **`MainComponent`** in this codebase, or the equivalent app root that hosts transport UI) **coordinates** numpad `*`, `Transport` (`requestPlaybackIntent`, playhead read), and the recording service. A **`RecorderService` (or equivalent) must not call `Transport` and must not own play/stop/seek policy.** Sequence when starting: **(1)** read playhead, **(2)** `beginRecording` on the message thread, **(3)** only if that succeeds, `requestPlaybackIntent(Playing)` (auto-start playback on record, per product choice). Stopping: **`requestPlaybackIntent(Stopped)` first**, then finalize take on non-realtime path, then publish `SessionSnapshot` only after the take file is closed and loadable.

**Audio callback — SPSC-style contract (approved):** The path from the audio device callback that **pushes** input samples and lightweight preview data into a recorder must be **realtime-safe in the SPSC sense**: in the callback, **no** lock, **no** allocation, **no** block, **no** wait, **no** `Session` / `SessionSnapshot` access, **no** UI state mutation. Disk I/O, `juce::AudioFormatWriter`, and `Session` publish happen on other threads. **Stated** as SPSC (single-producer, single-consumer) for the work done in the callback — not a claim of full lock-free C++ for every line of the program.

**FIFO overrun (approved):** Initial mono **sample** ring capacity = next power of two **≥** `deviceSampleRate * 5` (≈5 s). If a block does not fit: **do not** block the audio thread; **accept** what fits; count the rest as **dropped**; **intended** take duration in samples (and user-visible “recorded length”) **includes** the full logical block; **dropped** samples are written as **silence** in the finalized WAV (or an equivalent explicit “silence fill” for that span). **Do not** silently shorten the file. **Separate** `droppedSampleCount`. After stop/finalize, **warn** if `droppedSampleCount > 0` (e.g. *“Recording overrun: N samples were replaced with silence.”*). **`RecordedTakeResult` (or equivalent)** must carry: finalized file, target `TrackId`, `recordingStartSample`, **intended** sample count, sample rate, `droppedSampleCount`.

**Stop/finalize order (approved):** (1) `requestPlaybackIntent(Stopped)`; (2) clear recording so the next callback no longer **pushes**; (3) signal writer, drain FIFO + any pending **silence** for overruns, flush/close WAV, join writer; (4) return complete **RecordedTakeResult**; (5) **only then** create `PlacedClip` and **publish** a new `SessionSnapshot`.

**Preview peaks (approved):** **Single** consumer per app tick (e.g. `TrackLanesView` or one owner); **drain** once; forward only to the **recording** lane. Other `ClipWaveformView` instances must not race to `drain…`.

**Non-destructive take on non-empty track (approved):** No move/split/trim/delete/mute/overwrite/take-lanes/comping in this slice. New clip is a normal `PlacedClip` at `recordingStartSample`. Overlaps with existing clips: **unchanged** topmost-wins semantics.

**Engine exclusion while recording (approved):** While recording is **active** (testable from engine, e.g. atomic `recording` + `recordingTrackId` readable from `PlaybackEngine` **without** `Session` mutation), **skip** mixing the **armed/recording** track; other tracks unchanged; existing clips on that track stay **visible** in the UI; **not** a persisted mute in `Session` / `SessionSnapshot`.

**Device scope (steering, implementation when coding):** Mono, input channel 0 only; `initialiseWithDefaultDevices(1, 2)` when device init is changed; no full audio settings UI; no SRC; no software/direct monitoring in this slice. **Latency compensation deferred** — document: hook is input push in callback + `juce::AudioIODevice` input/output latency readouts for a future pass.

**Unsaved project (approved):** Recording requires a **known** on-disk project path. If unsaved: **fail** with a **visible** hint (*“Save the project before recording.”*); **no** transport start, **no** orphan take files in a global app directory, **no** empty committed clip.

**Explicit non-goals in this slice:** mixer, pan, sends, groups, split/cut, fades, plugin processing, full Cubase-like audio settings.

Rationale: preserves immutable `SessionSnapshot` handoff, keeps disk off the real-time path, and makes overrun and coordination **explicit** before first line of record code.

---

## 2026-04-26 — Phase 3 arrangement extent + pannable viewport (`.mdlproj` v3)

Decision:

- **`SessionSnapshot::arrangementExtentSamples_`** — [Message thread] persisted **stored** floor; **`getArrangementExtentSamples()`** = `max(stored, getDerivedTimelineLengthSamples())`. Propagated unchanged by all clip/track factories; **`withArrangementExtent`** and **`withTracks(tracks, extent)`** are the only ways to set / load a new stored value. **`Session::setArrangementExtentSamples`** republishes via `withArrangementExtent` (monotonic **non-decreasing** on stored). **`PlaybackEngine`** uses **`getArrangementExtentSamples()`** for `timelineEnd` (play silence in gaps; stop at extent).
- **`TimelineViewportModel` (`src/ui/`)** — [Message thread] **`visibleStartSamples_` + `visibleLengthSamples_`**, not grow-only `visibleEnd`. `panBySamples`, `clampToExtent`, `setVisibleLengthIfUnset`. **Not** in session or on disk.
- **Composition root (`Main` / `TransportControlsContent`)** — seeds **60s** arrangement and **30s** visible length when **stored==0** and **content==0**; `clampToExtent` after session-affecting actions; `syncViewportFromSession` on load / add.
- **Seek / playhead (UI):** `Session::getArrangementExtentSamples()`. `Session::getContentEndSamples()` / deprecated **`getTimelineLengthSamples()`** = derived clip end. Ruler and lanes: linear map on **[visibleStart, visibleStart+length)**. Playhead line only if **inside** the visible window.
- **Project file v3:** `arrangementExtentSamples` optional at root; **save** writes **effective** extent. **read** 1—3.

Rationale: DAW-like navigable/ playable extent distinct from “where clips end”; silent playback in empty time; no trim-induced rescaling (unchanged with pan denominator).

**Out of this step:** scroll bars, follow-playhead, zoom control, persisting `visibleStart` / `visibleLength` in the file, loop.

---

## 2026-04-26 — Phase 3 visible timeline span (UI-only viewport) — superseded

Decision:

- **`TimelineViewportModel` (`src/ui/`)** — [Message thread] stores `visibleEndSamples` (at least 1) as the **shared** horizontal map extent for the ruler and waveforms. **Not** in `Session` or `SessionSnapshot`. **Grow-only** `ensureCovers(derivedEnd)`; never shrinks when the logical/derived end drops (e.g. right-edge trim), so the x-axis does not jump.
- **Composition root (`Main` / `TransportControlsContent`)** owns one instance, wires `setOnVisibleRangeChanged` to repaint ruler + `TrackLanesView`, and calls `ensureCoversWithSession` after load, add clip, add track, and after clip move / trim from UI so the viewport can expand when the session grows.
- **`TimelineRulerView` / `ClipWaveformView`:** x mapping and in-flight move/trim use `getVisibleEndSamples()`. **Playhead and seek** still clamp to **`Session::getTimelineLengthSamples()`** (derived logical end); ruler applies seek then `jmin` to logical before `Transport::requestSeek`.
- **Peak cache:** `rebuildPeaksIfNeeded` keys off snapshot + width + `visibleEnd` so resampling runs when the visible span changes without a new snapshot.

Rationale: prevent trim from feeling like a global “resize the timeline”; decouple presentation from derived length without new transport semantics.

**Out of scope (this step):** scroll, zoom, `visibleStart`, persisting the viewport, bar/beat grid.

---

## 2026-04-25 — Phase 3 minimal project save / load (`.mdlproj` v1)

Decision:

- On-disk format: **JSON v1** (`io/ProjectFile`) with per-clip **absolute** `sourcePath` (same string as `AudioFileLoader` / `AudioClip::getSourceFilePath()`).
- **Load** builds a full `std::vector<Track>` and publishes **one** `SessionSnapshot` through `SessionSnapshot::withTracks` and a **single** `atomic_store` — not `addTrack` / `addClipFromFileAtPlayhead` chains.
- **Transport** stays the only owner of playhead state: after load, `requestSeek` with saved playhead **clamped** to the loaded session extent.
- **Partial load:** per-clip decode failures add `path - reason` lines; if JSON is invalid, **no** session swap.
- **Monotonic ids:** `nextPlacedClipId_` / `nextTrackId_` = `max(file value, max id present in file + 1)`.

Rationale: embryo persistence without inventing a full DAW project system; keeps the add-clip and load paths clearly separate.

---

## 2026-04-24 — Phase 2 minimal timeline ruler (seek on ruler strip)

Decision:

- **Seek** (click and drag) is on **`TimelineRulerView` only**; the event lane is for **selection and clip move**. Empty **lane** background **clears selection** and does **not** call `requestSeek`.
- Ticks: **unlabeled** marks at **round seconds** in session time, with **adaptive** step (1s, 5s, …) so long timelines stay legible. **No** mm:ss text, no bar/beat, no zoom, no markers, no loop, no snap.
- Ruler and lane share the **same linear** session-sample ↔ x mapping and **identical** horizontal bounds from `Main` layout. Playhead: short marker in the ruler, full-height line in the lane, both from `readPlayheadSamplesForUi` + the same clamp to `[0, timelineLength]`.
- `AudioDeviceManager` is read in the **ruler** on the message thread for **tick spacing only** (seconds from samples for drawing).

Rationale:

- Restores a DAW-like place for playhead/seek without overloading the event lane (which also handles drag).

Notes:

- Does not add a shared “timeline model” class; transport/session contracts stay unchanged.

---

## 2026-04-24 — Phase 2 move ordering on committed single-clip move (steering)

Decision (see `docs/PHASE_PLAN.md`, late Phase 2 extension):

- When a **single** placed clip `M` is **moved** and the move is **committed**, order is decided **only** from the **end-state** of the session: whether `M` overlaps any **other** placed clip after the new placement. No `N_before`, no per-drag state, no reorder during in-flight drag.
- If, after the commit, `M` overlaps **no** other clip: **promote `M` to index 0** (remove `M` from its current position, insert at front, shift others that were above `M` down by one).
- If, after the commit, `M` overlaps **one or more** other clips: **preserve `M`’s list ordinal** (no reorder from this move).
- **Selection** does not reorder. **Only** a committed move end-state may reorder, and **only** `M` is reordered.

Rationale:
- Aligns with reference DAW behavior (isolated release ≈ “reset to front”; overlapping release ≈ keep stack order) without session bookkeeping of drag path.
- Keeps a single, inspectable decision from one snapshot: overlap emptiness in the committed state.

Notes:
- Implementation (selection, drag, `Session`/`SessionSnapshot` move API) is **not** implied by this log entry; it requires an explicit follow-up approved step.
- “Newest on top” for **new** clips remains the separate default in `PHASE_PLAN.md`; this decision governs **moved** clips only, once that extension exists.

---

## 2026-04-24 — Phase 1 playback assumptions

Decision:
- Audio files are fully decoded into memory in Phase 1.
- File loading is synchronous on the message thread.
- Files whose sample rate does not match the active audio device sample rate are rejected.
- No implicit resampling is introduced.
- No general upmix/downmix logic is introduced in Phase 1.

Rationale:
- Keeps Phase 1 narrow and predictable.
- Avoids hiding resampling or loading complexity inside playback.
- Preserves clear separation between file loading and playback.

Notes:
- This is a Phase 1 strategy choice, not a permanent architectural commitment.
- Later phases may revisit loading and sample-rate handling explicitly.

---

## 2026-04-24 — Transport source of truth

Decision:
- `Transport` is the single source of truth for:
  - playback intent
  - authoritative playhead position
  - pending seek request

Rationale:
- Prevents duplication of transport state between UI, engine, and other layers.
- Preserves the steering rule that UI must not own playback state.

Notes:
- Playback intent is written from non-realtime code.
- The authoritative playhead is written only from the audio callback path.
- Seek is requested from non-realtime code and applied by the audio callback.

---

## 2026-04-24 — Phase 1 cross-thread transport model

Decision:
- UI/message-thread code reads transport state through read-only APIs.
- The audio callback is the only writer of the authoritative playhead.
- Seek requests are written on the message thread and consumed on the audio thread.
- The audio path uses a lock-free, non-blocking synchronization model.

Rationale:
- Keeps the transport contract explicit.
- Avoids mutexes and blocking synchronization on the audio path.
- Preserves a clean separation between UI interaction and realtime state mutation.

Notes:
- Exact low-level primitives are implementation details unless promoted to steering constraints later.

---

## 2026-04-24 — File loading/import is a distinct Phase 1 concept

Decision:
- File loading/import is treated as its own concept in Phase 1.
- `AudioFileLoader` is separate from playback, transport, and UI logic.

Rationale:
- Prevents file opening/decoding from becoming entangled with playback execution.
- Makes it easier to evolve clip/session behavior later without rewriting loading logic.

Notes:
- In Phase 1 this is intentionally a small concept, not a large subsystem.

---

## 2026-04-24 — Session owns the loaded clip

Decision:
- `Session` owns the currently loaded clip.
- Playback reads clip data from `Session`.
- UI and `PlaybackEngine` do not own clip loading state.

Rationale:
- Keeps clip ownership explicit.
- Avoids hidden ownership in the UI or playback layer.
- Preserves a path from one clip now to multiple clips later.

Notes:
- Phase 1 supports zero or one loaded clip.

---

## 2026-04-24 — Playback engine is the only audio callback object

Decision:
- `PlaybackEngine` is the only object registered as the audio callback.
- No parallel callback path or alternate playback object is introduced.

Rationale:
- Keeps audio-thread behavior centralized and reviewable.
- Prevents accidental duplication of playback logic.

Notes:
- Phase 1 introduced silence first, then real clip playback, while keeping the same ownership model.

---

## 2026-04-24 — Mono-to-stereo Phase 1 special case

Decision:
- If the loaded clip has exactly one channel and the output device has at least two output channels, the mono signal is duplicated to the first two outputs.
- Otherwise, the existing explicit Phase 1 channel policy remains unchanged.

Rationale:
- Makes common mono-file playback behave naturally on stereo devices.
- Keeps the change small and local without introducing general channel mixing.

Notes:
- This is an explicit Phase 1 exception, not a general-purpose upmix/downmix system.

---

## 2026-04-24 — Pause vs Stop behavior

Decision:
- Pause keeps the current playhead position.
- Stop sets playback intent to `Stopped` and requests seek to sample 0.

Rationale:
- Creates a clear and expected user-visible distinction between Pause and Stop.
- Reuses the existing transport request path instead of introducing new stop-specific state handling.

Notes:
- Stop does not unload the clip.
- If no clip is loaded, seek-to-zero is effectively a no-op.

---

## 2026-04-24 — Waveform rendering stays outside playback

Decision:
- Waveform rendering is derived from `Session` / `AudioClip` data.
- The waveform view does not read audio data from `PlaybackEngine`.
- The waveform view does not own transport state.

Rationale:
- Preserves separation between display and playback.
- Avoids accidental coupling between UI rendering and audio execution.

Notes:
- Phase 1 waveform rendering is simple and synchronous.
- This may be revisited later if performance or scale requires it.

---

## 2026-04-24 — Playhead overlay and click-to-seek use transport only

Decision:
- UI reads the playhead through a read-only transport API.
- Click-to-seek maps view position to sample position and calls `Transport::requestSeek(...)`.
- UI does not maintain its own authoritative playhead state.

Rationale:
- Preserves transport as the only source of truth.
- Keeps the waveform/playhead UI aligned with the same state the engine uses.

Notes:
- Phase 1 uses a simple full-width clip coordinate model.
- More advanced timeline semantics belong to later phases.
    
## 2026-04-25 — Phase 1 documentation retrofit completed

Decision:
- The existing Phase 1 codebase was retrofitted to the strengthened six-tier in-code documentation rubric.
- This included the clarified body-readability rule and the bounded readability-refactor allowance.
- The retrofit was completed before continuing with Phase 2 code work.

Rationale:
- The earlier documentation level was not sufficiently pedagogical for a reader browsing the codebase.
- File headers, class/method comments, and technical labels alone were not enough.
- Central method bodies also needed plain-language in-body explanatory comments where branch meaning would otherwise be hidden behind low-level mechanics.

Notes:
- The retrofit was intended to preserve behavior.
- The purpose was to improve pedagogical readability, not to change architecture or functionality.
- The strengthened documentation standard now serves as the baseline for subsequent Phase 2 implementation work.

## 2026-04-25 — Phase 2 resumed after documentation retrofit

Decision:
- Phase 2 planning remains the active next step after the completed Phase 1 documentation retrofit.
- The approved Phase 2 scope is still:
  - multiple clips on one timeline
  - explicit clip start positions
  - overlap allowed
  - front-most covering clip wins
  - add clip at current playhead
  - clamp at timeline end

Rationale:
- The documentation retrofit was completed to ensure the existing codebase is readable enough before building further on top of it.
- With that baseline in place, Phase 2 can now proceed under the strengthened documentation rubric.

Notes:
- Phase 2 implementation order remains the previously approved step sequence.
- No Phase 2 code changes were part of the documentation retrofit itself.
- The next executable Phase 2 step is the approved steering/document update for Phase 2 semantics before new code changes begin.

---

## 2026-04-23 — Phase 2 steering: timeline, overlap, and session ordering (docs)

Decision:

- The transport **playhead** is **timeline-absolute** in Phase 2: a single session timeline in
  device samples (same field as Phase 1; semantics extended), not a position inside a single
  loaded clip.
- **`Session` owns** the **ordered** list of placed clips and the **front-to-back** (overlap)
  order. The UI and waveform must not be the source of that order; they reflect `Session` only.
- **Newest added is front-most** in Phase 2: the most recently added clip is index 0 / on top
  for overlap, until a later phase adds explicit reordering.
- **Multiple clips** may **overlap** in time. **Audibility** is **not** a mix sum: at each
  instant, the **front-most clip that covers** that timeline position is the one heard there;
  other clips are audible only in **uncovered** regions (stacked-events behavior).
- **Add clip** uses the current playhead: placement **start sample** = playhead at add time
  (read on the non-realtime path as specified in the phase plan), not a separate placement UI.
- **End of session timeline** uses **derived** length from placements; at end, behavior stays
  aligned with the **clamp at end, silence, no spurious advance** product intent (generalized
  from single-clip end behavior).
- **Callback path** continues to use an **atomic immutable snapshot** of session state, not
  per-block heap allocation or mutexing for session reads, consistent with the Phase 1
  cross-thread model.

Rationale:

- Locks the previously approved Phase 2 plan into steering documents so implementation cannot
  drift (e.g. into implicit mixing, clip-local playhead, or UI-owned ordering) without a
  documented change.

Notes:

- Recorded in `docs/PHASE_PLAN.md` (Phase 2) and `docs/ARCHITECTURE_PRINCIPLES.md` (Phase 2
  subsection) as part of Phase 2 Step 3. No code changes in this step.

---

## 2026-04-25 — Phase 3 minimal multi-track: domain, summing, UI lanes

Decision:

- **Tracks** are a first-class domain concept (`Track` / `TrackId`). **`SessionSnapshot`** is an
  **ordered list of tracks**; each track owns an ordered `PlacedClip` list.
- **Within a track**, overlap and audibility follow the same rule as Phase 2: **front-most
  covering** clip wins; overlapping clips on the same track are **not** summed.
- **Across** tracks, output is the **arithmetic sum** of each track’s (mono→stereo) per-block
  contribution. This is **not** a mixer, routing graph, or per-track fader; it is the minimal
  extension of Phase 2 coverage to multiple lanes.
- **Session** starts with **one** default track. **Add track** appends a new empty track. New
  clips are placed on the **active** track (the track most recently created by **Add track**;
  at session start, that is the default track).
- **UI:** vertically stacked **one** waveform lane per track; **no** cross-track clip drag in
  this step (move/selection remain within the lane of the front-most hit clip).

Rationale:

- Establishes real multi-lane state and audition without importing mixer complexity or
  cross-lane edit semantics that would need separate steering.

Notes:

- See `docs/PHASE_PLAN.md` (Phase 3) and `docs/ARCHITECTURE_PRINCIPLES.md` (Phase 2/3
  playback wording). **Cross-track move** is covered by the 2026-04-25 late-extension entry
  below. Mixer surfaces remain out of scope for Phase 3.

---

## 2026-04-25 — Phase 3 late extension: cross-track clip drag

Decision:

- **Cross-track** reassignment of an existing `PlacedClip` is a **named** command: **`Session
  ::moveClipToTrack`**, building **`SessionSnapshot::withClipMovedToTrack`**. The clip is **removed
  from its source track** and **inserted as front-most (index 0)** on the **target** track, with
  **`PlacedClipId` and `AudioClip` identity preserved.**
- **Within-track** move remains **`Session::moveClip` / `SessionSnapshot::withClipMoved`** (same
  committed end-state policy as before, **in one lane** only). The two session APIs are **not
  merged** into a single optional-parameter overload.
- **Active track** (where **Add clip** goes) is **not** changed by a cross-track drop. The user
  changes `activeTrackId_` via **Add track**, **Clear** (reset), or **`Session::setActiveTrack`**
  (e.g. header click; no snapshot publish) — a cross-track drop is **not** one of these.
- **Valid drop target** is **geometric** only: any child lane in **`TrackLanesView`**. There is **no
  per-track “type / compatibility”** field or predicate in this step.
- **UI:** A **ghost** (translucent placeholder) is drawn **only** on the lane under the pointer
  after the same movement threshold as within-track move, **except** the source lane does **not**
  get a second ghost (it already shows the in-flight move). **Outside every lane,** all ghosts
  are cleared; the **source** component sets a **non-default** “invalid drop” cursor until
  re-entering a lane or **mouse up** (cursor always restored on release). **Mouse up outside all
  lanes** cancels: **no** snapshot publish.

Rationale:

- Preserves a single-snapshot, lock-free handoff, explicit session commands, and clear separation
  from a future per-track-typed lane model.

Notes:

- `PlaybackEngine` is unchanged. Not in scope: undo/redo, multi-clip drag, snap, mixer.

---

## 2026-04-25 — Phase 3 late extension: minimal track headers (name on `Track`, active on `Session`)

Decision:

- **`Track::getName()`** is the user-visible label, stored in the **domain** and carried through
  **`SessionSnapshot`** factories. **`Session`** assigns defaults when **creating** tracks (e.g.
  `"Track 1"`, `"Track 2"`, …) — the UI does **not** format names from list position.
- **Add-clip target** remains **`Session::activeTrackId_`**. The user can change the active track
  with **`Session::setActiveTrack(TrackId)`** (e.g. header click). That call **must not** publish a
  new **`SessionSnapshot`**; **`activeTrackId_` is not in the snapshot** and is not read on the
  audio path.
- **UI:** `TrackLanesView` shows a **fixed-width** left column (`TrackHeaderView` per track); **`Main`**
  insets **`TimelineRulerView`** by the same width so the **lane** x ↔ session-sample map matches
  the **ruler** in the **timeline** area. Headers are **not** cross-track drop targets (only child
  **`ClipWaveformView`** lanes are, via existing hit-test).

Rationale:

- Single source of truth for the displayed name; **rename** later is a new **`Session`**
  command + factory without changing list-index assumptions.

Out of scope for this step:

- Rename **UI**, faders, mute/solo, track delete, drop-on-header behaviour, **any**
  playback or snapshot-factory change beyond name **copy-through** (see **track reorder** entry
  below for row order).

---

## 2026-04-25 — Phase 3 late extension: track reorder by header drag

Decision:

- **Row order** of tracks in **`SessionSnapshot`** changes only via **`Session::moveTrack`** →
  **`SessionSnapshot::withTrackReordered`**. Each **`Track`** value (id, name, `PlacedClip` list) is
  **moved** as a unit; **no** clip-level factory is involved and **no** `PlacedClip` order changes
  **within** a lane.
- **`activeTrackId_` is not changed** by **`moveTrack`**; the UI highlights the same id on its new row.
- **UI:** drag threshold on **`TrackHeaderView`**; **`TrackLanesView`** owns in-flight state, **snaps**
  the pointer to the nearest **gap** (0..N), maps to **`destIndex`** with
  `dest = (k <= s) ? k : (k - 1)` (with **`s`** from **`findTrackIndexById`**, not a cached index).
  **Insert line** is drawn only in **`paintOverChildren`**. **Invalid** = outside the view or
  **x** in the lane (non-header) column — no line, **forbidden** cursor; **`getForbiddenNoDropMouseCursor`**
  is a **shared** implementation with invalid **clip** cross-lane drop (`ForbiddenCursor.h` / `.cpp`),
  not a second ad-hoc image in `TrackHeaderView`.

Rationale:

- One explicit session command, same snapshot pattern as clip moves, no playback semantics change
  (sum is commutative in track order).

Out of scope:

- Ghost track view, keyboard reorder, track delete, mixer, **any** change to **clip** drag
  behaviour, rename, resize.

---

## Non-destructive right-edge trim (PlacedClip visible length)

Date: 2026-04-26

Context:

- Need a **minimal** way to shorten the **heard** region of a clip without mutating PCM or
  conflating with split/cut.

Decision:

- **`PlacedClip`** stores **`visibleLengthSamples_`** (effective / audible span from material
  sample 0), with **`getEffectiveLengthSamples()`**, **`getMaterialLengthSamples()`**, and
  **`withRightEdgeVisibleLength`**. **`AudioClip`** is never trimmed.
- **`SessionSnapshot::withClipRightEdgeTrimmed`** replaces one **`PlacedClip`** by id; **no** lane
  reorder. **`Session::setClipRightEdgeVisibleLength`** publishes one snapshot.
- **PlaybackEngine**, **overlap** in **`moveOneClipInLane`**, **`getDerivedTimelineLengthSamples`**, and
  **ClipWaveformView** use effective length for span; engine material offset formula unchanged except
  the effective-length cap.
- **Project file:** writers use **v2** (`ProjectFileV1::kCurrentVersion == 2`); optional
  **`visibleLengthSamples`** per clip (0 = full on read / omitted on write when full). Readers
  accept **v1** and **v2**.

Rationale:

- Non-destructive editing, one source of truth for “how long this placement is” on the timeline,
  and clear separation from future split/cut features.

Out of scope:

- Left-edge trim, slip, fades, split, slip-on-timeline material offset inside the buffer.

---

## Mouse-centered timeline zoom (Ctrl+wheel) and default extent

Date: 2026-04-26

Context:

- Need a **real** default navigable span (user feels “two window widths” is too small) and **Cubase-like**
  **Ctrl+wheel** zoom with the point under the pointer staying visually fixed.

Decision:

- **Default empty session (no material, no stored extent):** seed **arrangement extent** to **1 hour** of
  samples and `setSamplesPerPixelIfUnset(sampleRate / 10)` in `Main` (default **10 px/s**; visible length
  in samples = `round(widthPx * samplesPerPixel)` and grows with window width at fixed zoom).
- **Correction (same date):** zoom state is **`samplesPerPixel_`**, not a stored visible length. Resize
  changes how much time is on screen, not the scale. **`Ctrl+wheel`** calls
  `zoomAroundSample(factor, pointerXPx, widthPx, ext, sppMin, sppMax)` with `sAtPointer = visStart
  + round(xPx * spp)` and `visStart' = sAtPointer - round(xPx * spp')`; `spp` clamped to `[0.1,
  max(1, ext/width)]`. **Plain wheel** pan uses `round((width/8) * samplesPerPixel)` sample steps.
- **Mapping:** `TimelineRulerView::xToSessionSampleClamped` / `sessionSampleToLocalX` (shared with lanes):
  `s = visStart + round(x * spp)`.

Rationale:

- Resizing no longer “shrinks the world” by forcing a constant sample count into a smaller pixel count;
  **samplesPerPixel** is the only zoom state.

Out of scope:

- Key bindings other than `Ctrl+wheel`, vertical zoom, persisting the viewport, fit-to-content.

---

## Timeline locator range (markers only; groundwork)

Date: 2026-04-28

Context:

- Need **Cubase-style** left/right **locators** as session metadata (markers on the timeline/ruler)
  **before** loop, punch, or export-range features consume them.

Decision:

- **`SessionSnapshot`** stores **`leftLocatorSamples_`** / **`rightLocatorSamples_`** alongside
  arrangement extent; **`withLocators(previous, left, right)`** publishes updates; loaders use
  four-arg **`withTracks(..., extent, left, right)`**, which clamps both to
  **`[0, max(storedExtent, derivedContentEnd)]`** without swapping or normalizing L/R order.
  **`rightLocatorSamples == 0`** means the **right** locator is **unset**; **pre–schema-v6**
  projects load locators as **0 / 0**.
- **`Session::setLeftLocatorAtSample` / `setRightLocatorAtSample`** clamp to current
  **`getArrangementExtentSamples()`**; getters read the snapshot — **PlaybackEngine unchanged**,
  audio thread does **not** branch on locators.
- **`TimelineRulerView`:** plain click/drag **seeks**; **Ctrl**+click/drag sets **left**; **Alt**+click/drag
  sets **right**. Ruler draws a **translucent** band between locators (**blue** when `right > left`,
  **orange** when **`right ≤ left`** and **`right > 0`**); markers at L (and at R when set). Locator fill
  and markers are painted **before** the playhead so the playhead stays **visibly on top**.
- **Project file v6 (`ProjectFileV1::kCurrentVersion`)** writes **`leftLocatorSamples`** /
  **`rightLocatorSamples`** only when non-zero **(omit zero)**; readers accept versions **1–6**.

Rationale:

- Single immutable snapshot pipeline, deterministic persistence, ruler-only UX until downstream
  features consume locators.

Out of scope:

- Loop playback, punch in/out, export range selection, snapping, key commands, locator labels.

---

## Cycle UI and Numpad 1 jump to left locator (visual only; no loop playback)

Date: 2026-04-28

Context:

- Extend Cubase-like **locator** UX: **upper/lower** ruler zones, transient **cycle armed** state, and
  **Numpad 1** jump to the **left** locator when the range is valid — **without** loop wrap on the
  audio thread yet.

Decision:

- **`Transport`** holds **`cycleEnabled_`** as **`std::atomic<bool>`** (**transient**, not in
  **ProjectFile**). **`requestCycleEnabled` / `readCycleEnabledForUi`** use release/acquire; **PlaybackEngine**
  does **not** read it in this slice.
- **`TimelineRulerView`:** **lower half** plain click/drag **seeks**; **upper half** plain **click**
  toggles cycle (suppressed when a callback reports recording or count-in). **Ctrl**/**Alt** still set
  L/R locators on either half (**override** zones). Locator band: **valid range** (**`right > left`**
  and **`right > 0`**) paints **blue** when cycle **on**, **gray** when **off**; **invalid** order
  (**`right ≤ left`**, **`right > 0`**) paints **orange**; small **bracket** strokes at L/R when cycle is
  on and the range is valid. **PlaybackEngine unchanged.**
- **`MainWindow::routeShortcut`:** **Numpad 1** (incl. raw **VK_NUMPAD1**) calls
  **`invokeJumpToLeftLocatorFromWindowShortcut`** on **`TransportControlsContent`**: if not recording /
  count-in and **`right > left`** and **`right > 0`**, **`Transport::requestSeek(left)`**; else log and no-op.

Rationale:

- Keeps realtime path unchanged while matching familiar DAW gestures; persistence deferred until loop
  playback consumes cycle state meaningfully alongside locators.

Out of scope:

- Actual loop playback (wrap at right locator), punch, cycle record, schema bump, **`PlaybackEngine`** /
  audio-thread changes.
