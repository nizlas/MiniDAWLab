# Insert persistence + shared track-header column — verification report (2026-10-01, 1.1.7)

Two user reports on Danielssons Audio Lab 1.1.6:

1. "I added DAL Mono Delay as a Post insert, saved, closed and reopened the project — it was gone."
2. "The Monitor button makes the last button clip in some track headers."

Pre-gain was explicitly out of scope for this round (the user accepts the AmpliTube level explanation).

---

## 1. Inserts not returning after save

### 1.1 Root cause (confirmed in code and by test)

Insert chains live in `PluginInsertHost`, keyed by `TrackId`, for **every** row kind: the Inspector
offers *Add Pre insert / Add Post insert* on audio, instrument, group and master rows and the engine
runs those chains (`renderInstrumentPostStripToStereoScratch`, `applyBusPostChannelStripFromInputToStage`).
Persistence did not cover the same set:

| Stage | 1.1.6 behaviour | Effect |
|---|---|---|
| UI assignment → session/host | `Vst3PluginPickerCoordinator` → `PluginInsertHost::addInsertFromVst3File(trackId, stage, file)` — works for every row kind; records "Add VST3 insert" undo → project marked dirty | OK |
| **Serialization** (`Session::saveProjectToFile`) | `if (pluginHost != nullptr && timelineAudioLane)` — chains were exported **only for `TrackKind::Audio`** | **An insert on an instrument / group / master row was never written to `tracks[].inserts[]`** (the project the user saved never contained it) |
| Read (`ProjectFile.cpp`) | `parseTrackInsertsFromVar` reads `inserts[]` for every row | OK |
| Instantiation (`applyLoadedProjectModel`) | `if (trDto.kind == "instrument") continue;` — instrument rows skipped | Would have dropped an instrument row's inserts even if written |
| State restore (`PluginInsertHost::importChainNoUndo`) | `setStateInformation(opaqueState)` after instantiation | OK for restorable plug-ins; **a non-restorable row was dropped from the live chain** (`continue`), so the next save erased it |

Both gaps date from the commit that introduced instrument-row persistence (`9e124c0`, 2026-05-13),
before instrument/group/master inserts existed in the engine. Classification per the user's list:
**"the entry was never written to the project file"** (for non-audio rows); for audio rows the
chain was intact. The user's project on disk (`TSE_pt2.dalproj`) accordingly carries only the
AmpliTube Pre insert on audio track 4 and no insert on any instrument/master row.

### 1.2 Fix (scope)

- `Session::saveProjectToFile`: export `pluginHost->exportChain(tr.id)` for **every** row.
- `Session::applyLoadedProjectModel`: restore `inserts[]` for every row kind (no instrument skip).
- `PluginInsertHost::importChainNoUndo`: a row whose plug-in cannot be instantiated becomes an
  **unavailable placeholder** (`LiveInsertSlot::unavailableDescriptor`) — same slot id, stage and
  chain position, identity and opaque state untouched, no processor published, Inspector shows
  `"<name> (unavailable)"`. `exportChain` re-emits it byte-for-byte. Identity-aware description
  pick (`pickDescriptionForSavedIdentity`): format + name + uid must match the saved
  `pluginIdentifier` (the bundle-path hash may differ for a relocated copy); a different plug-in at
  the saved path is treated as unavailable so a saved state is **never applied to another identity**.
- `getInsertRowsForTrack` / `hasAnyInsertOnTrack` include placeholders (Inspector rows, track-delete
  undo capture). `removeInsert` on a placeholder removes it for good (undoable, dirty).
- DALMonoDelay's own state handling was **not** touched — the trace never pointed at the plug-in:
  its state round-trips byte-identically through JUCE's VST3 host wrapper (812–831-byte blobs).

Dirty marking verified by code review: every add / remove / move / reorder / editor-parameter step
goes through `PluginInsertHost::recordPluginSlotUndo` → `UndoRedoCoordinator::onPluginUndoRecord`
→ `markProjectDirty()`; the focused test asserts the recorder fires once per add and once per removal.

### 1.3 What is preserved for a temporarily unavailable plug-in

`slotId`, `stage` (Pre/Post), chain position, `pluginVst3Path` (as saved), `pluginIdentifier`,
`pluginStateBase64` — byte-identical through load → save → load. The slot is silent in the audio path,
has no editor, can be moved Pre↔Post and removed. When the plug-in is available again (same identity
at the saved path), the preserved state is applied on the next load and the echo renders exactly as
before (verified: 250 ms / mix 50 % reproduce sample-exactly, max |Δ| = 0 vs the pre-save render).

### 1.4 Verification

`InsertPersistenceFocusedTests` (new, **121/121**, production `Session::saveProjectToFile` /
`loadProjectFromFile` + `ProjectFile` JSON + real `PluginInsertHost`, real plug-ins, no mocks):

| Check | Result |
|---|---|
| DAL Mono Delay Post on **instrument**, Pre on **audio** (+ BassEvening Post, chain with another vendor's VST3), Post on **group**, Post on **master** — four Mono Delay instances with distinct settings (250/125/375/500 ms, mix 50/50/40/30 %) | file carries every row with path + identifier + state; Save and Save As identical |
| Reload (both files): slot ids, stages, chain order, identity, locate path, **state byte-identical** per slot | pass |
| Audio after reload through `audioThread_processChainForTrack` (the strip-pass entry point): echo at 12000 / 18000 / 24000 samples (= saved 250 / 375 / 500 ms), echo/dry = 0.5/0.5, 0.4/0.6, 0.3/0.7 (= saved mix); audio chain Pre+Post max |Δ| vs pre-save = 0 | pass — parameters restored in sound, not just names |
| Simulated uninstall (saved path re-pointed to a non-existent bundle; user plug-ins untouched): row listed as `DAL Mono Delay (unavailable)`, nothing published to the audio thread, other rows live; re-save keeps slot/stage/path/identifier/state exactly | pass |
| Plug-in "returns" (path re-pointed back): live again, preserved state applied, same audio | pass |
| Identity mismatch (path points at BassEvening, identifier says Mono Delay): kept unavailable, state not applied to the other plug-in | pass |
| Explicit removal (placeholder and live): recorded as undo (dirty), gone from the file, does not resurrect on reload; untouched tracks keep theirs | pass |
| Placeholder moved Post→Pre, saved: new stage with untouched state | pass |

`MiniDAWLab.exe --stability-inserts <fixture>` (new in-app scenario; **PASS** on Debug and Release
1.1.7): builds an instrument shell row in the loaded project, adds Mono Delay through the picker's
call on instrument (Post) / audio (Pre) / master (Post), **production Save → reload**, **forced
autosave → in-process recovery**, then the missing-plug-in copy: `(unavailable)` row kept through
load → save → load. The fixture is `InsertPersistenceFocusedTests.exe --make-fixture <dir>`.

Regressions re-run: `PluginInsertTempoFocusedTests` (BPM transfer, pass), `MixdownPreGainFocusedTests --no-ui`
(49/49), `InputRoutingFocusedTests` (56/56), `WaveformReloadFocusedTests` (30/30), `MiniDAWSelftests`
(3220/3220 — one stale assertion fixed: it hard-coded "version 21 must be rejected" from when v20 was
current; now `kCurrentVersion + 1`), `--stability-smoke`, `--stability-open-save-close`.

Not verified through the real editor window: parameter edits made in a plug-in's own GUI. The state
path they feed (`getStateInformation` on the live instance at save time) is the one exercised above
with host-format blobs; the editor-close undo/flush path is unchanged.

---

## 2. Shared, draggable track-header column

### 2.1 Measurement (logical / DPI-independent px, from `TrackHeaderView`)

| Quantity | Value |
|---|---|
| Strip cell | 22 px |
| Widest row | 5 cells ([Instrument][Power][Mute][Monitor][Arm], instrument destination row) = 110 px |
| Left content inset | 8 px outer pad + 6 px active-stripe trim = 14 px |
| Right pad | 8 px |
| **Minimum column width** | 14 + 110 + 8 = **132 px** (`TrackHeaderView::kMinimumHeaderColumnWidthPx`) |
| **Default column width** | 132 + 12 px margin = **144 px** (`kDefaultHeaderColumnWidthPx`) |
| Maximum | 480 px; effective width also clamped to `viewWidth − 160` so lanes stay usable |
| Old fixed width | 120 px → the 5-cell row ended at 124 px: **Arm clipped by 4 px** (reproduced as the negative control in the focused test) |
| Drag handle | 6 px band centred on the boundary, full height, `LeftRightResizeCursor` |

The alternatives button (18 px, bottom-left) and all row-height rules are unchanged.

### 2.2 Implementation

`TrackLanesView` owns the one runtime width; `applyTransportControlsLayout` reads the identical
`effectiveTrackHeaderColumnWidthPxForTotalWidth(area.getWidth())` for the ruler inset, the add-track
corner button and the `PlayheadOverlay` origin. Headers, audio lanes, MIDI lanes, grid, header-reorder
insert line, wheel zoom/pan hit-testing all use `headerColumnWidthPx()`. The handle is a child
component that owns its mouse events (no clip move, row-height drag or header reorder can start from
it); middle-button pan still works. Names are fitted/truncated inside the header and never change
the width.

### 2.3 Persistence

No existing *app-wide* UI-layout mechanism existed (window bounds / Follow / MIDI-editor workspace are
**project-bound** fields in the project file; `audio-device.xml` / `audio-latency.xml` are the app-wide
settings pattern). The column width is a workstation preference, so it is stored **app-wide,
machine-local**: `%APPDATA%\MiniDAWLab\ui-layout.xml` via the new `UiLayoutSettingsStore`
(`<UI_LAYOUT version="1"><TRACK_HEADER_COLUMN widthPx="144"/></UI_LAYOUT>`), written **once per
completed drag** (handle release; no I/O per mouse move), loaded before the first layout. Absent /
malformed / non-numeric / non-positive ⇒ default 144; absurd values are clamped at layout time.
Not a session edit: no undo step, no dirty flag; adding/removing tracks does not touch it.

### 2.4 Verification

`TrackHeaderColumnFocusedTests` (new, **103/103**, production `TrackHeaderView` geometry + offscreen
paint): for audio / instrument / MIDI / group / master models at 120 / 132 / 144 / 240 px — every
present button fully inside the header and hittable (centre above the resize band), ≥ 8 px right pad,
right-most strip button actually painted; at 120 px the instrument row's Arm **is** clipped (negative
control). Long vs short names: identical button geometry, render stays exactly the column width.
Clamp formula (wide / narrow / degenerate views). `ui-layout.xml` round trip + malformed cases.

`MiniDAWLab.exe --stability-header-column <project>` (new in-app scenario on a copy of the user's
TSE project — 2 audio, 4 instrument/MIDI, master rows; **PASS** on Debug and Release 1.1.7, 150 %
display scale): at startup width, after a simulated handle drag to 144, to the clamp at 132, to 240,
after adding a MIDI track and after deleting it — every header spans exactly the shared width, every
button cell inside it, every lane / the ruler / the playhead overlay start at the same boundary x
(144 → 242, 132 → 230, 240 → 338 in window coordinates), the add-track button stays left of it;
`ui-layout.xml` carried 240 after the drag and the user's startup preference was restored at the end.
PNG evidence: `docs/evidence/header-column-2026-10-01/` (minimum-132, default-144, wide-240,
after-add-track, plus the offscreen instrument header at 132).

Covered by code review only: a real mouse drag on the handle (the scenario drives the same
`setTrackHeaderColumnWidthPx` + drag-ended path the handle calls) and the hover highlight.

---

## 3. Build, artifacts, commit

| | |
|---|---|
| Version | **1.1.7** (`CMakeLists.txt`), schema 23 unchanged |
| Release exe | `build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` (FileVersion 1.1.7) |
| Installer / portable | `dist\DanielssonsAudioLab-1.1.7-Setup.exe`, `dist\DanielssonsAudioLab-1.1.7.zip`; symbols `dist\symbols\DanielssonsAudioLab-1.1.7\` |
| Debug exe **with the changes** | `build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe` (the normal Debug location was free this time; the user's running DAL is the **alt** Debug exe `build\ninja-debug-alt\...\MiniDAWLab.exe`, still 1.1.6 and left untouched/running) |
| Focused test exes | `build\ninja-debug-alt\InsertPersistenceFocusedTests_artefacts\Debug\InsertPersistenceFocusedTests.exe`, `build\ninja-debug\TrackHeaderColumnFocusedTests_artefacts\Debug\TrackHeaderColumnFocusedTests.exe` |
| Commit | see `git log -1` on `main` (release notes `docs/releases/1.1.7.md`) |

Preserved and re-verified: ASIO device path untouched; BPM transfer (`PluginInsertTempoFocusedTests`
pass); export fixes (`MixdownPreGainFocusedTests` pass). Real music projects were not modified
(all runs used copies under `%TEMP%`); the user's running app was not closed.

## 4. Gaps / open items

- **AmpliTube 4 teardown crash** (`AmpliTube 4.vpa` offset `0x7675E`, `0xC0000005`) while its
  instance is destroyed: at app shutdown (after the scenario had already passed) and once during
  `--stability-open-save-close`'s reload of the TSE copy. Intermittent and **pre-existing**: the
  unmodified 1.1.6 alt Debug build crashed at the same reload step while the 1.1.7 Debug build
  passed the identical run, and the user's own 1.1.6 session crashed there on close at 20:06:35.
  Dumps under `%APPDATA%\MiniDAWLab\crash-dumps\` (pids 29944, 24944, 26408). Remains the open
  stability-audit item; not addressed here.
- The 11.7 KB size drop of `TSE_pt2.dalproj` between the 20:04:45 save and the 20:06:28 save after
  reload could not be attributed from the surviving files (the earlier file was overwritten); the
  saved files never contained a Mono Delay row, consistent with the write-side root cause above.
- Inserts on `TrackKind::Midi` rows: the Inspector offers them, the engine never processes them
  (MIDI rows have no audio path). They are now persisted like any other row (no silent loss);
  hiding the section on MIDI rows is a separate UI decision not made here.
