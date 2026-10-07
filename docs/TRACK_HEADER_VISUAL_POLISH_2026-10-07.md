# Visual polish: track headers, events and collapsible groups (2026-10-07, evening)

Follow-up to the compact-header slice (`27e59ce`) after the user's first manual pass: the group
tabs, margins and selection edges made the layout busy. UI corrections only — no audio engine,
routing, Solo semantics, plug-in lifetime or musical-data changes, and no new project schema
(everything is derived from existing data).

## What changed

1. **Group name separated from the collapse button.** The "golf club" tab is gone. Each group has
   a small chevron **button** (16 × 12 px component, 12 px painted box on the member marker's
   shaft; ▶ collapsed / ▼ expanded; short click = toggle exactly once; right-click = menu;
   tooltip = the full name) and a separate **name label** (12 px, 11 pt, header width minus the
   margins, ellipsized; tooltip when truncated; long press 500 ms = inline rename without toggling,
   Enter commits, Escape cancels; a short click does nothing). Expanded: both share one vertical
   span across the group's top boundary (10 px above = the previous row's resize band + glyph-free
   chrome, 2 px below = the first member's top pad), so they never cover Power / Mute / Solo of
   either row. The label is shown only when the previous row leaves those 10 px free (Mini / Medium
   / Large / gutter band); between Micro or Small rows only the button appears and the name lives
   in its tooltip (a context-menu Rename lays the label out for the edit). Collapsed: both sit at
   the top of the collapsed block, so a collapsed group always shows its name.
2. **Paint order.** The header-column row separators (painted over the children) now exclude every
   group control's rectangle, and the member marker of an expanded group is painted afterwards as
   ONE unbroken line from the group's top to its last member's bottom. No separator crosses a
   control or the marker; the controls are opaque and brought to front after every layout.
3. **Collapsed block.** `collapsedGroupBlockHeightPx(n) = max(25, 4 n + (n − 1))`: 4 px strips with
   1 px empty gaps (none before the first / after the last), the strips block centred vertically,
   never stretched. The 25 px readable minimum = top pad 3 + control row 12 + 10 px reserved for a
   directly following group's button (two adjacent groups never overlap controls). 2 members → 25,
   8 → 39, 16 → 79. Header column and timeline share the one derived height; members' header /
   lane components keep empty bounds; normal heights are preserved exactly and restored on expand.
   This replaces the earlier "exactly 4 × members, no gaps" rule.
4. **Mini shows content.** `LaneEventDetail` is now Bars (< 32 px) / **Content** (32–47 px:
   waveform or MIDI notes, no name label) / Full (≥ 48 px), decided from the lane's actual height.
   Audio lanes and both MIDI lane kinds (instrument rows and pure MIDI rows) paint their
   illustration clipped to the event box; the raster cache rebuilds through the existing deferred
   timer after a resize, so the content returns without a click or zoom.
5. **One vertical event margin.** MIDI lanes no longer add their own 6 px inset; audio and MIDI
   events both use `timeline_clip_chrome::kEventVerticalMargin` (4 px) on the lane bounds, so
   their outer top / bottom edges line up at every row height. Time positions, lengths, trim,
   layering and editing are untouched (the MIDI hit geometry moved with the paint geometry).
6. **Colour segment fills the row** from its top edge to the separator below (no band gap); the
   resize band stays a hit area without its own visible strip.
7. **Selection edges.** The multi-selection wash is one flat colour over the plate (the active row's
   4 px stripe stays pure); the 1 px blue frame is gone. One black separator between rows.

## Verification

- Handler-level (offscreen production components): `CollapsibleTrackGroupFocusedTests` (190
  checks), `TrackHeaderColumnFocusedTests` (685), `TrackRowHeightFocusedTests` (87),
  `InputRoutingFocusedTests` (56), `MiniDAWSelftests` once for the touched lane code. Rendered
  examples (`--render`): Micro / Mini / Small / Medium with colours and collapsed groups, mixed
  heights, 8- and 16-track groups — inspected.
- Real app, real OS mouse (temp project with audio, instrument placeholder, pure MIDI, Group bus,
  Stereo Out; Solo cells wired): chevron click expands the collapsed group, long press on the name
  opens the inline rename without toggling, Escape cancels, right-click opens the menu, plain +
  shift-click multi-selection shows the flat wash without blue edges. Screenshots confirmed Mini
  waveforms and MIDI notes, aligned audio / MIDI event edges, full-height colour segments, P/M/S
  on every row kind.
- Not re-run: the stability scenarios (`--stability-header-column`, `--stability-duplicate-track`);
  resize drags and dropdown picks by hand.

## Manual checklist

1. Groups at Micro / Small: only the chevron shows at the boundary — hover it for the name; the
   context-menu Rename still works there.
2. Collapse a 2-track group (25 px block, name readable) and a large one (strips with gaps); scroll
   with a collapsed block at the viewport top.
3. Mini rows: waveforms / notes visible, no labels; Small and up: labels back.
4. Shift-select several rows: one flat tint, single black separator, active stripe pure.

## Known limitations

- Between Micro or Small rows the expanded group's name is tooltip-only (no room for a label that
  stays clear of the buttons).
- The chevron button covers the leftmost 16 px of the previous row's resize band (resize works
  along the rest of the band).
