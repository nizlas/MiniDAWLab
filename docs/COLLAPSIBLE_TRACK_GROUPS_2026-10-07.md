# Collapsible track groups — purely visual (2026-10-07)

Delivered on the Solo/track-heights branch (`cursor/solo-and-solo-memories-da54`, PR #5).
Adjacent arrangement tracks can be grouped under a name and collapsed into a compact content
overview: every member becomes a 4 px strip showing its clip time intervals. The feature is
**purely visual** — it is NOT the routing Group-bus track kind, creates no track or bus, and
never changes track order, clips, routing, sends, plugins, instruments, proxy identity,
Mute/Solo/Off/Monitor/Arm, playback, recording, export, proxy rendering or mixer visibility.

## Why musical data cannot change

- Group state (`domain/VisualTrackGroup.h`: id, name, member `TrackId`s, collapsed) lives on
  `Session` **outside** `SessionSnapshot`, exactly like the Solo sets. None of the group commands
  (`createVisualTrackGroup`, `renameVisualTrackGroup`, `setVisualTrackGroupCollapsed`,
  `removeVisualTrackGroup`, `setAllVisualTrackGroups`) publishes a snapshot — the focused tests
  assert pointer-identical snapshots across every group command.
- Display and persistence read the **effective** membership: stored ids filtered to tracks that
  exist in the current snapshot. Stale ids of deleted tracks stay in the stored list, so undoing
  a track deletion restores membership with no extra bookkeeping. A group is *displayable* only
  while ≥2 effective members are contiguous in snapshot order; anything else renders as normal
  tracks — the single safe fallback used for live edits AND for malformed project metadata.
- Load-time repair is metadata-only by design: invalid groups are dropped; tracks and clips are
  never altered to make layout metadata fit.

## Creating a group (header multi-selection)

- Plain click on a header selects exactly that row; **shift-click** selects the contiguous
  visible range from the anchor (another shift-click re-spans from the same anchor). Clear visual
  indication: a selection wash + a 2 px marker on the header plate. The selection is UI-only —
  it never changes the Inspector's active track and never touches clip multi-selection; there is
  deliberately NO mouse-down+drag header selection (that gesture remains track reorder), and
  height-drag zones are untouched.
- Right-click **inside** the selection keeps it; **outside**, the clicked track is selected
  first. Every header context menu (audio, Group bus, instrument, MIDI rows) appends
  **"Create collapsible group…"**, enabled only for ≥2 adjacent, ungrouped, non-Stereo-Out
  tracks (groups never nest or overlap). A name prompt offers `Group <n>`; the group is created
  **expanded** with every member height untouched. Delete/Duplicate stay single-track commands.

## Expanded look and the handle

- Members carry a discreet vertical group marker along the header's left edge, placed right of
  the active-track stripe and left of all buttons (nothing is obscured).
- The **"inverted golf club" handle** (`VisualGroupHandleView`): the marker line continues into a
  short tab extending right at the boundary between the first member and the previous track,
  showing a collapse triangle + the group name. While the group intersects the viewport the tab
  clamps below the timeline gutter, so it stays visible and clickable for a group at the very
  first track and while scrolled; fully scrolled-out groups unmap their handle. The handle
  swallows its mouse events — hit-testing can never activate the previous track or its resize
  band (verified via `getComponentAt`).
- **Short click** toggles collapse (exactly one toggle per click). **Long press (500 ms,
  cancelled by >4 px movement)** opens the inline rename editor — Enter commits, Escape cancels,
  keyboard shortcuts do not fire while typing, and the mouse-up after a long press does NOT also
  toggle. Right-click on the handle: Expand/Collapse group, Rename group…, Ungroup (removes only
  the visual grouping — tracks and clips always survive).

## Collapsed mini-view (4 px strips)

- Each effective member displays as a strip of **exactly 4 logical px**, same order, zero gap
  (in-run separators are suppressed); the group's content height is exactly members × 4 — the
  Small 64 px minimum deliberately does not apply to the collapsed DISPLAY.
- Strips show the row's own clip intervals as grey fields (`0xff8e98a8`, clearly visible against
  the lane background): audio placements with their actual start + effective (trimmed) length;
  instrument/MIDI rows their own timeline clips. Overlaps paint as a continuous union; empty rows
  stay empty; a clean instrument destination gets NO fabricated events from routed MIDI tracks
  (the painter reads only the row's own clips by construction). No waveforms, no note dots.
- The strips use the exact same `TimelineRulerView::sessionSampleToLocalX` mapping (origin,
  visible start, samples-per-pixel) as the full lanes — verified by pixel sampling under zoom and
  scroll.
- **No hidden interaction**: collapsed member rows get EMPTY component bounds (the same mechanism
  as scrolled-out rows), and the strips are painted, not components — there is nothing to click,
  hover, drag or tooltip under the mini-view, and nothing falls through to hidden clips. The
  mixer and Inspector keep working on members; collapsing changes neither the active track nor
  the clip selection.

## Geometry, scrolling, heights

- ONE shared layout model: `rowHeightForVisibleEntry` returns the DISPLAY height (4 px for
  collapsed members) and feeds every layout/paint/scroll/diagnostic path; `rowHeightForTrack`
  keeps the stored NORMAL height and feeds save, preset status, duplicate and resize. 4 px can
  therefore never leak into `rowHeight` persistence or the Custom status.
- The group's top edge anchors on collapse (scroll offset preserved, then the normal clamp);
  the scrollbar uses the actual displayed content height; ruler, zoom and horizontal scroll are
  untouched. A global preset chosen while collapsed updates the hidden members' NORMAL heights;
  expansion restores the stored heights exactly.

## Membership under track changes

- Duplicate of a member puts the copy right after its source inside the group. Deleting a member
  removes it from the effective view; undo (snapshot restore) brings membership back. Below 2
  effective members the group dissolves **visually** and is restorable the same way.
- Reorders run through `Session::checkTrackMoveAgainstVisualGroups` (simulated splice; every
  displayable group must stay contiguous — this also catches an outside track dropped into the
  middle): the drag shows the red no-op line, a refused drop raises an alert naming the group
  with the Ungroup hint, and `Session::moveTrack` no-ops defensively. Within-group reorders work.
  The audio-model order can never diverge from the visible order.

## Persistence (v27) and undo

- Root `visualTrackGroups` array (`name`, `collapsed`, `memberTrackIds[]`), omitted when empty;
  only displayable groups with snapshot-filtered members are written. Older projects simply load
  without groups; malformed metadata degrades per entry (never a read failure, musical data never
  touched).
- Create / Rename / Ungroup are ONE narrow undo step each (`executeUndoableVisualTrackGroupsEdit`:
  full group list before/after, identical timeline snapshot pointer on both sides, no-ops record
  nothing). Collapse/expand marks the project dirty but records **no undo entry**; note that
  undoing an earlier metadata step restores the group list as recorded, including the collapsed
  flags of that moment (bounded metadata restore).

## Verification

**Automated (Linux cloud, offscreen production components + real model/undo/persistence code):**

- New suite `CollapsibleTrackGroupFocusedTests` — **128 checks, 0 failures**. Selection semantics
  and create validation; exact 4 px/zero-gap layout with mixed kinds and varying heights;
  pixel-sampled strip intervals under zoom + scroll; empty-member strips; normal-height
  separation (preset while collapsed, Custom status, `rowHeight` never 4); handle placement
  (first track / viewport top / scrolled out) and short-click vs long-press actions;
  `getComponentAt` hidden-hit-area sweep; scroll anchoring + clamping; Duplicate / delete+undo /
  dissolve / reorder-veto membership rules; narrow undo through the real `UndoRedoCoordinator`
  (timeline compared by content; collapse records no step); v27 round trip, drops of
  non-displayable groups, pre-v27 files, malformed metadata. `--render <dir>` reproduces the
  expanded/collapsed example images (8 and 16 tracks) attached to the PR.
- Full regression: `TrackRowHeightFocusedTests` 70/0, `SoloFocusedTests` 104/0,
  `TrackHeaderColumnFocusedTests` 242/0, `MixerFocusedTests` 107/0,
  `WaveformReloadFocusedTests` 30/0, `ExportLevelFocusedTests` 60/0,
  `InputRoutingFocusedTests` 56/0, `InstrumentParallelFocusedTests` 39/0,
  `LiveMidiRecordingFocusedTests` 110/0, `TrackDuplicateFocusedTests` 23/0,
  `MiniDAWSelftests` 3478 checks with only the two pre-existing environmental `mc-txn`
  failures (Linux `rename()` semantics; documented, not regressions).

**What the automation does NOT cover (handler-level vs real OS input):** the focused tests drive
the exact handler actions the mouse code dispatches (selection clicks, the handle's short-click /
long-press actions, rename commit) and verify hit-testing offscreen — they are not real OS mouse
events, timers or modal menus. The 500 ms long-press *feel*, popup menus, the name-prompt dialog,
drag-reorder with the refusal alert, and tooltip absence under the strips need a real session.

**Local Windows checklist (minutes):**

1. Shift-click a few headers, right-click → Create collapsible group…, accept the default name.
2. Drag-reorder: try pulling a member out and dropping an outside track into the group — red
   no-op line while dragging, alert naming the group on drop; Ungroup via the handle menu, redo
   the move, verify it now works.
3. Handle: short click to collapse/expand a few times; hold ~500 ms for rename (Enter/Escape);
   check the handle at a group on track 1 and while scrolling the group past the viewport top.
4. Collapsed: hover across the strips (no tooltips/cursors), scroll + zoom and compare strip
   fields against the expanded clips, resize the window.
5. Save with a collapsed group, reload: groups/names/collapsed states back, heights restored on
   expand; open a pre-v27 project and confirm it is unchanged.

## Known limitations

- Instrument-row strips are exercised only in the running app: the offscreen harness cannot
  construct the instrument runtime, so the painter's instrument branch (own clips only, nothing
  fabricated from routed MIDI) is enforced by construction + code review there, with the audio
  and empty-row branches pixel-verified.
- Undoing a create/rename/ungroup step restores the collapsed flags recorded with that step (see
  Persistence and undo above) — collapse state after a metadata undo reflects the recorded list,
  not the latest display toggle.
- No keyboard shortcut for collapse/expand and no group-wide commands (mute all members etc.) —
  deliberately out of scope for the visual feature.
