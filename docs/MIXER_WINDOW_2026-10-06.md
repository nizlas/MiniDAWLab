# Mixer window — first usable mixer (2026-10-06, 1.1.16)

A separate, resizable mixer window (F3 / Window > Mixer) with one channel strip per row in
arrangement order, the Stereo Out strip fixed at the right, seven globally toggleable sections
(Routing, Pre-gain, Pre inserts, Post inserts, Sends, Faders, Meters) and concurrent level meters.
Architecture: `docs/CURRENT_ARCHITECTURE.md` "Mixer window" and "Concurrent per-row meters";
test policy row in `docs/DEVELOPMENT_TEST_POLICY.md`; evidence `docs/evidence/mixer-2026-10-06/`.

## 1. Planning output (as required by the implementation guide)

**Scope.** A mixer for the EXISTING channel functions: fader, pan, pre-gain (Audio), audio
input / output, MIDI input + filter, MIDI channel, MIDI To, Pre / Post inserts, the four send
slots, Power / Mute / Monitor / Record-arm, instrument editor / alternatives, output meters;
concurrent metering; machine-local layout persistence.

**Out of scope (unchanged model).** No solo engine, no pre-gain on non-Audio rows, no new send
modes or signal paths, no per-channel device configuration, no master processing, no fader
undo (the Inspector's direct setter policy is kept), no insert drag-and-drop in the mixer, no
150-track stress test.

**Gaps / risks found and how they were handled.**
* The Inspector's controls were active-track-only and its handlers were private lambdas → the
  undoable edits are now built once as `TrackEditActions` by `TrackLanesEditCoordinator` and
  handed to BOTH views; the selector item lists and value-field texts were extracted into pure
  headers (`TrackChannelOptions.h`, `TrackValueFieldText.h`) that both views use.
* The engine metered one row with drain-and-reset windows; two views would have stolen each
  other's peaks → `TrackMeterBank` (per-TrackId slots, published slot map) + `LevelMeterHub`
  (single drainer, fan-out, interest set). Cost is bounded: only displayed rows are folded; a
  hidden mixer meters nothing.
* Power / Mute on instrument and MIDI rows go through their controller (the header's path) —
  a direct `Session::setTrackMuted` would have desynchronized the controller's render snapshot.
* A hub acknowledgement echo (lamp reset → hub → every view → lamp reset → …) crashed the
  Inspector scenario once; `LevelMeterComponent::resetOverloadLatch(notifyOwner=false)` is the
  receiving side now.

**Plausible wrong implementations avoided.** Copying `InspectorView` per strip (active-track
coupling, 10 Hz rebuilds × N); strips calling `Session` directly (bypassing undo, validation and
the recording refusal); making the mixer drain the engine's accumulators (Inspector loses peaks);
reading meters by snapshot index instead of TrackId (a reorder between fold and drain would
mislabel a window); per-strip section layout (faders drift between neighbours).

**Deferred (safe).** Insert drag-and-drop in the mixer (context menu covers reorder / stage /
remove through the same host calls); meter bank beyond 256 rows (overflow is reported, not
hidden); a mouse-drag test of the mixer fader (the component is the Inspector's, verified there).

## 2. What the mixer contains

| Strip kind | Routing rows | Sections | Base buttons |
|---|---|---|---|
| Audio | Audio Input (device channels, mono / stereo pairs, "(unavailable)"), Audio Output | pre-gain, Pre / Post inserts, sends, pan + fader, meter | Power, Mute, Monitor, R |
| Instrument | MIDI Input, Input Channel, MIDI Channel, Audio Output | inserts, sends, pan + fader, meter | editor, Power, Mute, Monitor, R, alternatives |
| MIDI | MIDI Input, Input Channel, MIDI To, MIDI Channel | — (no audio path: no fader, pan, meter, inserts, sends) | Power, Mute, Monitor, R |
| Group | Audio Output | inserts, sends, pan + fader, meter | Mute |
| Stereo Out | device output (information) | inserts, fader, Stereo Out meter (never sends) | Mute |

Section heights are computed once per layout pass and applied to every strip (Stereo Out
included); the lower band (fader + pan left, meter right) stretches with the window; with every
section shown the stack is 678 px and fits the default 1200×720 window. A lower window scrolls
the whole stack; the strips scroll horizontally while Stereo Out stays put. Clicking a strip's
name / background activates the row like a header click; the active row is highlighted.

## 3. Verification (measured through the production app unless stated)

`--stability-mixer %TEMP%\dal-tse-vb3\TSE_pt2.dalproj` (sibling copy, Debug, ASIO; a Group row
added by the scenario because the project has none) — PASS, log excerpt in the evidence folder:

| Check | Result |
|---|---|
| F3 shows, F3 hides, F3 shows; window instances | 1 instance throughout |
| Strips vs arrangement order | `1:AUDIO 3:INSTRUMENT 4:AUDIO 5:INSTRUMENT 7:MIDI 8:INSTRUMENT 9:GROUP | master 2:STEREO OUT` |
| Horizontal scroll to the end | Stereo Out strip screen bounds unchanged |
| Hide Routing / Pre inserts / Post inserts / Sends; Meters only; Faders only; all again | flags independent; `verifyLayout` (bands identical to Stereo Out, every control inside its band, fader ≥ 60 px) passes in every state; PNGs `mixer-all-sections-scrolled`, `mixer-faders-meters`, `mixer-faders-only`, `mixer-meters-only`, `mixer-all-sections`, `mixer-playing` |
| Another row active, audio row's strip: fader "-6" | session gain 1.000 → 0.501, the active row's gain unchanged (0.708) |
| pan 0.5, pre-gain "+3" | `pan=0.50 preGainDb=3.00` on the audio row |
| Audio Output → Group 1, send 1 → Group 1 at −6 | `output=9`, send row shows "Group 1" / "-6.00" after the poll |
| Inspector after activating the row | fader "-6.00" = strip, pre-gain "+3.0" = strip |
| Insert row context action "move to the other stage" on Track 4 | `Pre:AmpliTube 4` → `Post:AmpliTube 4` in strip and host; undo restores `Pre:` |
| Mute through the strip | session `muted` toggles and back |
| Playing from 24 s | held peaks Track 3 0.578, VB3-II 0.176, Track 8 0.148, Stereo Out 0.343; Inspector 0.578 = mixer 0.578 on the active row; hub interest = the six audio-carrying rows |
| Hide / show 5× while playing | transport intent, plug-in chains and instrument runtime pointers identical before / after; hidden mixer meters 1 row (the Inspector's) |
| Delete a row / undo | 7 → 6 → 7 strips |
| Save, reload | strips rebuilt in order; metered rows all in the new snapshot |
| Move / resize to 1000×640, hide Sends | `ui-layout.xml`: `MIXER_WINDOW 700 346 1000 640`, `sends=0`, header width 149 kept |

`MixerFocusedTests` — 71 checks, 0 failures (bank, hub, layout, offscreen strip bound to a
non-active row, acknowledgement without echo, store round trip incl. malformed input).

Regressions with a concrete reason: `--stability-inspector-panel` (TSE copy) PASS — Inspector
meters now come through the hub, overload latch sets and resets; `--stability-live-midi` on its
pre-gain fixture PASS — the MIDI Input selector texts now come from the shared builder;
`TrackHeaderColumnFocusedTests` 149, `ExportLevelFocusedTests` 47, `InputRoutingFocusedTests` 56,
`MiniDAWSelftests` 3478 — 0 failures.

The same scenario on the **Release** build: PASS (first attempt crashed at the reload step in
`AmpliTube 4.vpa+0x7675E` — the known intermittent AmpliTube teardown crash, identical offset to
the 1.1.14 occurrence, before any mixer code ran in that step; the rerun passed end-to-end).

Code inspection only: the F3 key path inside the mixer window and the TextEditor guard (the
scenario toggles through the same shell entry point, not a synthesized key press); the `Window >
Mixer` menu tick. Not performed: listening; a real mouse drag on a mixer fader.

## 4. Known limitations

* Insert reordering in the mixer is a context menu, not drag-and-drop; more than three inserts
  per stage show "+N more (Inspector)".
* The mixer polls at 10 Hz like the Inspector; a change made in one view appears in the other on
  the next tick (≤ 100 ms).
* `--stability-live-midi` on the TSE copy fails a pre-existing header-cell geometry check for
  the fixture's MIDI rows (project-dependent lane height); the scenario passes on its own
  fixture. Recorded in the backlog, untouched here.
