# Stability audit 2026-09-26 — MIDI paste/move crash and broad stability review

**Status: PARTIAL.** The three user-reported symptoms were investigated and two were fixed
(shipped in 1.1.3, commit `26814d4`). The broad application-wide stability audit requested in
the same task was only partly carried out; the unfinished areas are listed in §4 and are
unfinished parts of the original request, not new scope.

| | |
|---|---|
| Build under investigation | 1.1.2 Release (`C:\Program Files\Danielssons Audio Lab\MiniDAWLab.exe`) |
| Fix shipped in | 1.1.3, commit `26814d4` (pushed to `origin/main`) |
| Release notes | `docs/releases/1.1.3.md` |
| Project used | temp copies of `C:\Users\nicla\Music\TSE_pt2_260827\TSE_pt2.dalproj` (original never written; mtime still 2026-09-26 22:58:00) and of `C:\Users\nicla\Documents\template\template.dalproj` |
| MIDI file used | copy of `C:\Users\nicla\Desktop\Midi\Lead git.mid` (format 0, 480 PPQ, 1 062 bytes, 112 notes) |

---

## 1. Reported symptoms — outcome

| Symptom | Outcome | Classification |
|---|---|---|
| **A.** All sound stops after importing a MIDI file onto a new MIDI track | **Unresolved — not reproduced** | Not reproduced through the production import path; see §1.1 |
| **B.** Ctrl+V pastes a MIDI event *and* opens the MIDI editor | **Fixed** | Code-established defect |
| **C.** DAL crashes when the pasted event is dragged onto the new organ instrument track | **Fixed** | Reproduced defect (user crash dump + AddressSanitizer) |

### 1.1 A — global audio loss after MIDI import (unresolved)

What was done:

- Reconstructed the evening's timeline from `project-load-diag.log`, `project-save-diag.log`,
  `autosave-diag.log`: session 1 loaded 22:51:46, the MIDI file was accessed 22:52, autosaves at
  22:53:36 and 22:55:36 and the manual save at 22:56:25 all show `tracks=7` (no MIDI row
  present), restart/load at 22:57:23, save 22:58:00, undoable instrument edits at 22:58:47
  (paste) and 22:58:50 (move), crash 22:58:53. The MIDI row from step A therefore never reached
  a saved state; its exact configuration (MIDI To destination, transport state at import) is
  unknown.
- Added an independent audio-health probe (device-output peak hold, monotonic transport
  "advanced samples" counter, per-instrument processed-block counters) and a scenario,
  `--stability-midi-import-audio <project> --midi <file>`, that runs the production path: add
  MIDI track via the same entry as the transport menu → import via the production
  parse-and-append (`InstrumentMidiImportCoordinator::importMidiFileOntoTrackNow`) → play.
- Result on the TSE copy with the user's MIDI file: before and after the import the callback
  keeps cycling, the transport advances, device output peak ≈ 1.0, and all three VB3-II /
  HALion hosts (tracks 3, 5, 8) keep processing blocks with non-zero peaks. Repeated 6× —
  all PASS. The first run's FAIL ("transport did not advance") was a probe artifact: the
  project has cycle enabled and the playhead legitimately wrapped backwards; the probe now uses
  the monotonic counter.
- Code read: the engine's `midiSources` loop handles a MIDI row with no destination with
  `continue` (no early return); the offline-render gate is RAII-balanced and `gateDepth=0` in
  every probe reading; `setInstrumentProcessingSuspended` is balanced in project load.

Not tested: import while the transport is playing; import onto a MIDI row whose MIDI To is
already set; import onto an instrument row. Working hypothesis (unproven): A was a downstream
effect of the use-after-free in C corrupting state before the visible crash. **A stays open
until either reproduced or the variants above are exhausted.**

### 1.2 B — paste opened the MIDI editor (fixed)

- Cause: `ClipPasteboardController::invokePasteClipFromWindowShortcut` called
  `openMidiEditorForInstrumentClip` on the pasted clip after creating it.
- Fix: paste now creates and selects the clip only (`src/app/ClipPasteboardController.cpp`).
  Double-click remains the explicit open gesture.
- Verified: `--stability-midi-editor-move` step "assert paste did not open the editor" PASS
  through the production copy/paste controller.

### 1.3 C — crash when moving the pasted event (fixed)

- Evidence: the user's Release 1.1.2 crash (`0xC0000005`, module offset `0x1FF377`)
  symbolizes with `dist\symbols\DanielssonsAudioLab-1.1.2\` to
  `ExperimentalMidiEditorWindow::Body::pushRowsModeToRoll +0x177`,
  `src/ui/experimental/ExperimentalMidiEditorWindow.cpp:1816` — the call
  `pluginNoteNameQueryChannel(boundTimelineClip_)`.
- Mechanism: the editor holds a raw `InstrumentMidiClip*` (`boundTimelineClip_`) into the
  controller's `clips_` vector. A cross-track move (`moveInstrumentMidiClipsBetweenTracks` →
  `removeInstrumentMidiClipsByIds`) frees that object and posts an asynchronous change message;
  the editor's `changeListenerCallback` then dereferences the freed clip. Debug builds do not
  fault (freed memory stays readable); the Release build and AddressSanitizer do.
- Fix (three layers):
  1. `UndoRedoCoordinator::executeUndoableInstrumentEdit` invokes a new
     `reconcileMidiEditorAfterInstrumentEdit` hook synchronously after every applied
     instrument edit (`src/app/UndoRedoCoordinator.{h,cpp}`).
  2. `MidiEditorPresenter::detachOpenEditorIfBoundClipMissing` detaches the editor to scratch
     when its bound clip id no longer resolves on its track — before the async change message
     can run, which also unregisters the change listener (`src/app/MidiEditorPresenter.{h,cpp}`).
  3. `pushRowsModeToRoll` re-resolves the clip by stored id instead of trusting the raw pointer
     (`src/ui/experimental/ExperimentalMidiEditorWindow.cpp`).
  Covers move, delete and paste, since all route through `executeUndoableInstrumentEdit`.
- Verified (see §3): ASan negative control (fixes disabled) exits with the ASan error code
  right after the move; ASan with fixes exits 0 with audio healthy.

---

## 2. Additional findings

| Finding | Classification | Location | Action |
|---|---|---|---|
| Stability invariant forbade a MIDI editor open on a `TrackKind::Midi` row, although Phase B made that legal (the editor borrows the MIDI To destination's host) | Code-established (pre-Phase-B check) | `src/diagnostics/StabilityInvariants.cpp`, "midi-editor" check | **Fixed**: validates the row's routed destination runtime instead |
| Audio-health probe reported "transport did not advance" on a cycle wrap | Tooling artifact | `MainAppWindow.cpp` probe hooks; `Transport::readAdvancedSamplesTotalForDiagnostics` | **Fixed**: monotonic counter |
| Access violation inside `C:\Program Files\IK Multimedia\AmpliTube 4\AmpliTube 4.vpa` (module offset `0x7675E`) during process teardown and during repeated project load/unload | **Cause undetermined.** The failing module is known; DAL's teardown ordering (host release vs. a still-running device callback, quit while the transport is playing) was **not investigated** | Dumps: `%APPDATA%\MiniDAWLab\crash-dumps\MiniDAWLab-crash-20260926-231921-pid41812`, `-233539-pid22008`, `-234229-pid33008`, `-235610-pid34796` | Open — first item of the continuation |

The first AmpliTube dump (23:19:21) followed a scenario that quit while the transport was
playing; the last (23:56:10, last-operation "app startup begin") occurred inside a delete-loop
iteration. A plain load → quit (`--stability-load-loop`, 23:21) did not crash. That is all that
is established.

---

## 3. Verification evidence

Log locations: `%APPDATA%\MiniDAWLab\stability-run.log` and `stability-invariant.log`
(entries 2026-09-26 23:19 → 2026-09-27 00:01). Builds: Debug
`build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe`, ASan
`build\ninja-asan\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe`.

### 3.1 Before / after for C

| Run | Build | Fixes | Result |
|---|---|---|---|
| `--stability-midi-editor-move` on template copy, start 23:44:51 | ASan | disabled (negative control) | **process exit 99 (ASan error) immediately after the "move the open clip cross-track" step**; no RESULT line written |
| same, 23:46 | ASan | enabled | exit 1 — RESULT FAIL only on the audio probe (template's sole instrument is a plugin-less shell → silence); **no ASan error** |
| same on TSE copy, 23:48:05 | ASan | enabled | **exit 0, RESULT PASS**; output peak 0.247, instruments 3/5/8 rendering |
| Debug negative controls, 23:39 and 23:42 | Debug | disabled | PASS — Debug does not fault on the stale read (freed memory readable); not evidence either way |

### 3.2 Scenario battery on the fixed Debug build (all on TSE temp copies)

| Scenario | Runs | Result |
|---|---|---|
| `--stability-midi-import-audio --midi "Lead git.mid"` | 6 | PASS |
| `--stability-midi-editor-move` | 7 | PASS |
| `--stability-midi-track-parity` | 2 | PASS |
| `--stability-midi-routing` | 2 | PASS |
| `--stability-smoke` | 1 | PASS |
| `--stability-mixdown --format wav` | 1 | PASS |
| `--stability-autosave` | 1 | PASS |
| `--stability-delete-loop --iterations 2` | 1 | PASS |

Zero new `INVARIANT FAIL` lines. Crash dumps during the battery: AmpliTube only (§2).

### 3.3 Preserved evidence (local, `dist\` is gitignored)

`dist\crash-evidence\1.1.2-20260926-225853\`: the user's Release crash
(`MiniDAWLab-crash-20260926-225853-pid21812.txt/.dmp/-last-operation.txt`), copies of
`project-load-diag.log`, `project-save-diag.log`, `autosave-diag.log`, and
`fixture\Lead git.mid`. Matching symbols: `dist\symbols\DanielssonsAudioLab-1.1.2\`.

### 3.4 Limitations of the verification

- No OS-level input automation: production command, import, editor-open and move handlers
  were driven through the stability-runner hooks, not real mouse drags / key presses.
- The 20-cycle create/import → paste → move → undo/redo → delete/recreate repeat sequence
  requested in the task was **not** run as one loop; coverage is the per-scenario repeats above.
- Undo/redo of a cross-track move while the editor was bound to the moved clip is
  code-established (editor goes to scratch via the existing undo rebind) but not scenario-tested.

---

## 4. Broad audit — coverage

### 4.1 Areas actually inspected

| Area | Depth | What was done |
|---|---|---|
| Clip ↔ MIDI-editor lifetime | Deep | Root-caused and fixed the use-after-free (§1.3) |
| Import, copy/paste, cross-track move, delete | Deep | Traced to `executeUndoableInstrumentEdit`; reconcile hook; retained regression scenarios |
| Audio callback health across import | Medium | New probe; engine `midiSources` null-destination path read; offline gate balance verified |
| Stability invariants (MIDI editor) | Medium | Stale Phase-B check corrected |
| Track deletion, load/save/autosave, mixdown | Scenario level only | Existing scenarios PASS; no code review of failure branches |

### 4.2 Remaining areas (unfinished parts of the original request)

1. **Plugin teardown at shutdown and on repeated load/unload** — AmpliTube crashes (§2); DAL's
   ordering of device stop, callback drain and host release not examined; quit-while-playing
   not examined.
2. **Symptom A variants** — import while playing; import onto a MIDI row with MIDI To set;
   import onto an instrument row.
3. **Audio-device changes, prepare/suspend/resume, error recovery** — not inspected beyond the
   `setInstrumentProcessingSuspended` balance in project load.
4. **Recording start/stop/finalization and Monitor transitions** — not inspected in this task.
5. **Primary / proxy / Secondary switching and failed instantiation** — not systematically
   inspected.
6. **Project load/save/autosave failure paths and preservation of unavailable-plugin state** —
   scenario PASS only.
7. **Waveform / background jobs against replaced or destroyed UI objects** — not inspected here.
8. **Undo/redo of a move with the editor bound to the moved clip** — not scenario-tested.
9. **Realtime data-race review** (UI edits vs. audio-thread snapshot reads) — only the paths
   touched by import/move were read.

---

## 5. Changed files (commit `26814d4`)

`CMakeLists.txt`, `installer/MiniDAWLab.iss`, `docs/releases/1.1.3.md`,
`src/app/ClipPasteboardController.cpp`, `src/app/InstrumentMidiImportCoordinator.{h,cpp}`,
`src/app/MainAppWindow.cpp`, `src/app/MidiEditorPresenter.{h,cpp}`,
`src/app/UndoRedoCoordinator.{h,cpp}`, `src/diagnostics/StabilityInvariants.cpp`,
`src/diagnostics/StabilityScenarioRunner.{h,cpp}`, `src/engine/PlaybackEngine.{h,cpp}`,
`src/plugins/ExperimentalInstrumentHost.{h,cpp}`, `src/transport/Transport.{h,cpp}`,
`src/ui/experimental/ExperimentalMidiEditorWindow.cpp`.
