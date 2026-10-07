# Compact track headers, Micro/Mini, row-height grid, type icons, numbers and track colours (2026-10-07)

Delivered on `main` on top of the merged PR #5 work (Solo, row-height presets, collapsible
groups). UI work and its persistence only: the audio engine, routing, proxies, MIDI data,
recording and plug-in lifetimes are untouched; there is no mixer redesign and no automation
sub-tracks.

## Header structure (every row kind, every height)

Left → right, logical px (`track_header_geometry` in
[`src/ui/TrackHeaderView.h`](../src/ui/TrackHeaderView.h)):

| Zone | Width | Content |
|---|---|---|
| Group margin | 8 | active-track stripe (x 0–4) + collapsible-group member marker (x 5–7) — outside everything else |
| Colour segment | `3 + 14 + 3 + digits × 7 + 3` → **44** for the 3-digit minimum, 51 for 4 digits | track-type icon (14 px, vector) + right-aligned 1-based **arrangement number** (11 pt); full chrome height; coloured with the track colour |
| gap | 3 | |
| Title-row strip | 3 × 22 (always reserved) | `[Power][Mute][Solo]`, present cells collapsed left (Group bus: `[Mute][Solo]`, Stereo Out: `[Mute]`). Unavailable cells have EMPTY bounds — no hit area, no tooltip |
| gap | 4 | |
| Name | ≥ 48 (`kNameMinWidthPx`) | same 14 pt as before, **ellipsized** instead of shrunk; Instrument/MIDI rows keep a 9 px slot at the right for the live-MIDI dot so the ellipsis never jumps |
| right pad | 8 | |

Minimum header column **181 px** (was 154), default **200 px** (was 166); larger saved widths are
preserved, smaller saved widths are clamped up at layout time (as before).

Vertically: title row at y 2 (22 px); a **second control row** at y 27 with
`[Monitor][Arm][Instrument editor][Alternatives]` (present cells collapsed left) only when the row
height is ≥ 53 px (`kMinimumHeightForSecondRowPx`); the 4 px bottom resize band is kept at every
height (hit order: band → cells → plain click / shift-selection / reorder), so Micro rows can be
resized without stealing button clicks. Hidden controls never change state. Painting, state and
command paths of all buttons are the pre-existing ones — only the placement changed.

Type icons (`track_strip_glyphs::TrackTypeIcon`): Audio = waveform bars, Instrument = keyboard,
MIDI = DIN connector, Group bus = merging lines, Stereo Out = two rings. The number is the
current arrangement order (not the `TrackId`), refreshed with every insert / duplicate / delete /
reorder, counts hidden collapsed members, uses one common digit column
(`max(3, digits of the track count)`) and is never stored. Visual (collapsible) groups have no
number.

## Row-height grid (logical px)

`height = 28 + n × 14` ([`src/ui/TrackRowHeightPresets.h`](../src/ui/TrackRowHeightPresets.h)):

| Preset | n | Height | Derivation |
|---|---|---|---|
| **Micro** | 0 | **28** | top pad 2 + one 22 px title row + 4 px resize band (minimal margin) |
| **Mini** | 1 | **42** | the same single row with air |
| **Small** | 2 | **56** | two stacked title rows = title row + second control row (alternatives / editor may still hide when the second row has no room) |
| **Medium** | 5 | **98** | ≈ the old 96 default (Small + 3 steps ≥ Small + 2) |
| **Large** | 12 | **196** | ≈ 2 × Medium (Medium + 7 steps ≥ Medium + 2) |

Step **14 px** = half a Micro row. Minimum **28**; the old 480 px *user* cap is removed — only the
documented safety cap **1120 px** (n = 78) remains, so heights above Large are ordinary grid steps.
All values are `static_assert`ed against the real header chrome. The old Small = 64 is gone;
96 / 192 were guides only.

- **Snapping drag**: `snapRowHeightPxToGrid(startHeightOfTheDrag + totalPointerMovement)`
  (nearest step, ties up) — a pure function of the pointer, stable back and forth, every
  intermediate height on the grid; the drag-end snap is a no-op.
- **Dropdown**: Micro / Mini / Small / Medium / Large; **Custom** is status only. A preset sets
  EVERY row's normal height in one layout pass — including hidden members of collapsed groups
  (the 4 px display is untouched; the new normal height shows on expand) — and is the default for
  new tracks. Duplicate copies the source height. Status is derived from normal heights only.
- **Load**: saved heights are only **clamped** to [28, 1120] — older 64 / 96 / 192 heights stay
  exactly as saved and read as Custom until the next resize or preset; a below-minimum value →
  28, an absurd value → 1120, a missing one → the preset default (unknown preset key → Medium).

## Event painting at low heights (painting only)

`LaneEventDetail` from the lane height (shared by audio lanes and instrument/MIDI event lanes):
< 32 px **Bars** (thin 8 px field showing the real time extent, no waveform / notes / text; the
selection outlines the bar), < 48 px **Compact** (rounded body only), otherwise **Full**
(unchanged waveform / notes + label + overlap hatch). Micro = Bars, Mini = Compact, Small and up
= Full. Hit geometry, trim handles, drag/drop, clip functions and musical data are identical at
every height. The collapsed-group grey overview is a different painter and unchanged.

## Track colours

Palette ([`src/domain/TrackColour.h`](../src/domain/TrackColour.h),
[`src/ui/TrackColourPalette.h`](../src/ui/TrackColourPalette.h)): **Default grey** (explicit
reset), blue, teal, green, ochre, orange, red, purple — matte header-segment fills with darker
event-body fills; the Default keeps the historic `0xff343c4d` body so uncoloured projects look as
before. Right-click on the icon / number segment opens the palette popup (swatch + name, current
ticked) for the right-clicked track — active or not, never the header selection. The colour
applies to the segment background and the track's audio / MIDI event bodies; text, waveforms,
notes, selection, mute and record markers keep their colours; the header plate, the empty lane
area and the active-track marking are not recoloured. Events derive the colour from their track
at paint time (moving a clip follows the destination; no per-event colour); Duplicate inherits
the colour; the audio lanes' cached waveform raster keys on the fill
(`ClipWaveformView::rasterContentFingerprint`), so the change shows on the very next paint.

Undo: `UndoRedoCoordinator::executeUndoableTrackColourEdit` records ONE narrow step
(`TrackColourUndoSides { trackId, before, after }`, identical timeline snapshot pointer on both
sides, no step for an identical colour); undo / redo move only the colour and mark the project
dirty. Height changes keep the existing dirty-only (no undo) policy.

## Persistence (schema v28)

`tracks[].colour` (palette key, written only for non-default colours) beside the v26
`tracks[].rowHeight` and `trackRowHeightPreset` (keys now `micro | mini | small | medium | large`).
Pre-v28 projects → Default grey everywhere; saved heights are preserved (never reset to Medium);
unknown / non-string colour values → grey, never a read failure. Solo memories and group metadata
are untouched.

## Collapsible groups

The group marker sits in the 8 px margin outside the colour segment. The "golf club" tab keeps the
group's top as its anchor and adapts to the neighbouring row heights
(`TrackLanesView::layoutVisualGroupHandles`): group top at / above the gutter → 16 px tab in the
gutter band right of the add-track corner; previous row with ≥ 12 px free chrome under its lowest
control row (Medium 45, Mini 14 — Small 3 and Micro 0 do not qualify) → inline tab inside that
band with its bottom on the boundary; otherwise (Micro / Small neighbours, collapsed strips,
adjacent groups) → a compact 12 px tab centred on the boundary and confined to margin + segment,
so it never covers a Power / Mute / Solo cell or a name — the full name is the tab's tooltip.
Short click / long-press rename / menu are unchanged; collapsed members stay exactly 4 px.

## Verification (handler-level, offscreen production components — not real OS mouse input)

- `TrackHeaderColumnFocusedTests` (657 checks): derived limits, per-kind geometry at 120 / 181 /
  200 / 240 incl. overlap / alignment / ellipsis / paint evidence with icon + 3-digit number; grid
  values, snapping, clamps, keys, detail thresholds; every kind × Micro / Mini / Small / Medium
  (title row inside + hittable, second row ≥ 53 px only, band clear). Header PNGs inspected.
- `TrackRowHeightFocusedTests` (87): Micro / Mini presets, snapping drags from a start height,
  Custom, heights above Large / past 480, saved 64 / 192 kept as Custom, first resize snaps,
  `"micro"` key, cap 1120.
- `CollapsibleTrackGroupFocusedTests` (157): handle placement gutter / inline / compact incl. the
  first track and two adjacent Micro groups; free-chrome table; colour: narrow undo through the
  real coordinator on a non-active track, immediate lane + segment pixel change, Duplicate, v28
  round trip, pre-v28 → grey, bad values → grey. `--render <dir>` images (Micro / Mini / Small /
  Medium with several kinds + colours + a collapsed group, and a mixed-heights image) inspected.
- Also green: `InputRoutingFocusedTests`, `SoloFocusedTests`, `MixerFocusedTests`,
  `TrackDuplicateFocusedTests`, `InsertPersistenceFocusedTests`, `WaveformReloadFocusedTests`,
  `MiniDAWSelftests` (3478). No broad stability audit or full matrix was run for this slice.

## Manual checklist (real mouse, Release + ASIO)

1. Micro / Mini / Small / Medium / Large from the dropdown; drag a bottom edge up and down — the
   height snaps in 14 px steps, Custom appears, heights above Large work; resizing a Micro row
   does not trigger Power / Mute / Solo.
2. Right-click the icon / number segment of a NON-active track → palette; the segment and the
   track's events recolour immediately; Ctrl+Z / Shift+Ctrl+Z move only the colour; Duplicate
   inherits; Save → reopen keeps colours and heights.
3. Long track names ellipsize at the default width and at the 181 px minimum; the live-MIDI dot
   never overlaps the name.
4. Group tab in Micro next to Power, at the first track and with two adjacent groups: click
   toggles the group only; collapse / expand restores mixed normal heights.
5. Open an older project (saved before the compact-header slice): heights kept, Custom status, grey colours.

## Known limitations

- Older saved heights (64 / 96 / 192) load as **Custom** until resized or a preset is chosen.
- The compact 12 px group tab truncates long group names (tooltip shows the full name).
- Right after a height change the audio lanes blit the previous raster scaled for ~200–340 ms
  before the deferred rebuild (pre-existing zoom-freeze design); the detail level then updates.
- Verification is handler-level; real OS mouse passes are the manual checklist above.
