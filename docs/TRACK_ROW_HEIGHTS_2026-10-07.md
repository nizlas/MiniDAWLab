# Shared track heights Small / Medium / Large (2026-10-07)

Delivered on top of the Solo branch (`cursor/solo-and-solo-memories-da54`, PR #5). A compact
toolbar dropdown sets the height of ALL arrangement tracks in one step; individual bottom-edge
resizing still works per row; every height plus the chosen preset persists in the project file.
UI-only: no Solo rules, audio engine or plugin handling were touched.

## The three sizes (logical px, DPI-independent)

| Preset | Height | Definition |
|---|---|---|
| **Small** | **64 px** | The smallest row height where the title row, the FULL main button strip (incl. the new S cell and the instrument-editor cell) and the bottom resize band fit without overlap for every row kind. Computed from the real header control geometry — binding case is a row with a subtitle: outer pad 4 + name block 30 + name-to-buttons gap 3 + control cell 22 + resize band 5 = 64 — and locked to it by `static_assert`s in `TrackHeaderView.cpp`. No button or text is shrunk. |
| **Medium** | **96 px** | The pre-existing normal default in the code, unchanged. |
| **Large** | **192 px** | Clearly bigger with a simple documented proportion: exactly 2 × Medium. |

All three live in ONE place: [`src/ui/TrackRowHeightPresets.h`](../src/ui/TrackRowHeightPresets.h)
(values, persistence keys `"small" | "medium" | "large"`, mapping helpers). The existing 480 px
individual maximum is kept; the individual minimum is now uniformly Small for every track type.

At Small the separate small proxy/alternatives button does not fit and hides entirely — empty
`getAlternativesButtonBounds()` is the single structural gate: no painting, no hit target, no
tooltip, no invisible click area — and it returns unchanged at larger heights. All other main
buttons (Power, M, S, Monitor, R, instrument editor) remain visible and clickable at Small.

## Dropdown behavior (§1, §4)

- Sits on the main-window toolbar **between Solo memories 1–4 and the Pointer/Split (scissors)
  tool group**, same row height. The tool strip's left clamp was extended past the dropdown, the
  same tight-toolbar strategy the Solo strip already used — verified disjoint at 1600/1100/900 px
  window widths and with a widened header column.
- Choosing Small/Medium/Large is a **one-shot command**, not a locked mode: every existing
  arrangement track (all kinds, incl. Group and Stereo Out, incl. rows scrolled out of view) gets
  that height in ONE gathered layout pass; mixer strips are unaffected; individual drags still
  work afterwards.
- The dropdown **shows** a preset name only while ALL tracks' actual heights exactly match it;
  otherwise it shows **Custom** — status text only, deliberately never a fourth menu item.
- The last explicitly chosen preset is the default height for NEW tracks, even after individual
  edits. **Duplicate Track** gives the copy the SOURCE track's height and never changes the
  project default.

## Individual resizing (§3)

The existing common drag path (shared by audio, instrument, MIDI, group and Stereo Out headers)
is reused unchanged: vertical resize cursor on the bottom band, drag changes only that track,
header and timeline lane follow. New minimum = Small (64), maximum = 480 as before. A resize
never activates buttons, moves clips or starts a reorder (the band keeps its existing hit
priority). Spec-driven consequence worth noting: the old "name-only" collapse below the full
button row (31/39 px) is no longer reachable, since the new minimum already fits the full chrome.

## Persistence + Custom behavior (§5)

- Project file **v26** (additive, absent-key defaults — no competing storage models; no existing
  fields stored heights before): root `trackRowHeightPreset` = the last chosen preset key, and
  per track `tracks[].rowHeight` = that row's ACTUAL height in px.
- Save, Save As and autosave all pass the same snapshot through one `ProjectIoCoordinator`
  callback into `Session::saveProjectToFile`; load ALWAYS applies (even for pre-v26 files, which
  reset to the historical Medium look). Saved heights are clamped to [64, 480]; malformed values
  (negative, non-numeric, absurd, unknown preset keys) degrade safely to defaults — never a load
  failure.
- A real height change (drag or preset) marks the project dirty; the load apply never does.
- Undo: row heights keep the existing policy — **not undoable** (exactly like the pre-existing
  drag), only persisted. Clip times, routing, Solo memories and musical data are untouched.

## Scroll / layout (§6)

The single existing vertical scroll model is reused: a preset apply is one gathered relayout that
updates content height, preserves the previously topmost visible track where clamping allows, and
clamps the offset when content shrinks below the viewport. Horizontal scroll and zoom are
untouched. Verified with the production `verifyVerticalScrollLayoutForDiagnostics` after preset
changes and mid-list resizes.

## Verified in the cloud (Linux, Debug)

- `TrackRowHeightFocusedTests` — NEW, **70 checks, 0 failures**: production `TrackLanesView`
  offscreen (real Session/Transport/scroll model) — preset changes every row incl. off-viewport +
  Group + Stereo Out; status Small/Medium/Large vs Custom; clamps [64, 480]; new-track default =
  last chosen preset even after drags; Duplicate keeps source height; top-visible-track
  preservation + offset clamping; user-edit vs load dirty notifications; v26 round trip at raw
  JSON level AND through the production load; pre-v26 → Medium; malformed values safe; the
  PRODUCTION toolbar layout keeps Solo memories < dropdown < scissors group disjoint.
- `TrackHeaderColumnFocusedTests` — **242 checks, 0 failures** (90 new): Small = exactly the
  smallest full-chrome height (snap-rule proof); at Small every main button incl. the S cell is
  inside and hittable for every row kind at 154/166 px and paints; the alternatives button is
  fully OFF at Small (empty bounds = the one gate for paint, hit scan and tooltip), present at
  Medium/Large, hides again when shrunk.
- Unchanged suites after the change: `SoloFocusedTests` 104/0 (one expectation updated: the
  writer now stamps v26), `ExportLevelFocusedTests` 60/0, `WaveformReloadFocusedTests` 30/0,
  `MixerFocusedTests` 107/0, `TrackDuplicateFocusedTests` 23/0, `InputRoutingFocusedTests` 56/0,
  `MiniDAWSelftests` 3478 checks with only the two known pre-existing Linux-only `mc-txn`
  failures (Windows `MoveFileEx` semantics; documented in the Solo report).
- Full app target builds clean on Linux.

Known cloud limitations (same class as the Solo delivery): no real OS mouse input (drags and
clicks are handler-level / offscreen), no Windows build, no audio device, no screen rendering of
the running app.

## Short local verification (Windows, one evening)

1. Build + start; open a project with audio + instrument + MIDI + group rows. The new dropdown
   sits between the Solo memory buttons and the Pointer/Split tools; shrink the window and widen
   the header column — nothing overlaps.
2. Pick **Small**: every row (scroll the whole list, incl. Stereo Out) is compact; all header
   buttons incl. S work; the little instrument-alternatives button is gone and has NO hover
   tooltip or click reaction where it used to sit; pick **Large** — it returns.
3. Drag one header bottom edge: only that row changes; the dropdown flips to **Custom**; dragging
   far down stops at the Small height; cursor is the vertical resize arrow; no clip moves or
   button clicks from the drag.
4. Add a track (gets the last chosen preset height) and Duplicate a dragged track (copy keeps the
   source height).
5. Save, reopen: mixed heights and the dropdown status are back. Open an older (pre-v26) project:
   all rows at the normal Medium height. Save it and reopen — still correct.
6. Mixer window: strip sizes unchanged throughout.
