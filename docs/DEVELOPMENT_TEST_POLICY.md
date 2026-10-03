# Development Test Policy — tiered testing for MiniDAWLab (DAL)

Purpose: pick the **smallest test that can falsify the change**. The full stability matrix and
release certification are deliberate gates, not per-edit checks. Running the full matrix after
every small implementation slice is explicitly **not wanted** — it is slow and adds no signal for
low-risk changes.

Related documents:

- `docs/STABILITY_TESTING.md` — what each stability scenario does, logs, and flags.
- `docs/RELEASE_CERTIFICATION.md` — the release gate (Level 4).
- `docs/VALIDATION_CHECKLIST.md` — general slice validation (code review side).

---

## Test levels

### Level 0 — Build check

Use for: documentation-only changes, comments, or tiny isolated code changes with no runtime path
impact.

```powershell
.\scripts\build-windows.ps1 -Config Debug
```

Expected: build succeeds, no new relevant compile warnings/errors.

### Level 1 — Targeted feature check

Use for: normal feature/bug/polish work. This is the **default level** for implementation slices.

Run:

1. Debug build.
2. The smallest manual or automated test that exercises the changed feature.
3. Inspect the relevant diagnostic log if applicable (`%APPDATA%\MiniDAWLab\*.log`).

Examples:

- **Window bounds change:** Debug build; move/resize main window, save, reload, verify restore.
  If MIDI editor bounds changed: open MIDI editor, resize, save/reload, verify. No mixdown or
  matrix required.
- **MIDI note editing change:** Debug build; manual test in the MIDI editor; targeted MIDI
  stability scenario only if one exists. No mixdown/export tests unless touched.
- **Mixdown change:** Debug build; `--stability-mixdown` wav (and mp3 if LAME is available);
  manual GUI check if the dialog changed. No delete-loop unless routing/audio callback changed.
- **Autosave change:** Debug build; `--stability-autosave` and `--stability-recover-autosave`.
  No mixdown unless the project save/export path changed.

### Level 2 — Relevant stability scenario(s)

Use when: a change touches **one** high-risk subsystem. Run only the scenarios that cover it:

- **Project load/save:** `--stability-load-loop` (and open/save/close paths); autosave/recover if
  save state was touched.
- **Track delete/undo/redo:** `--stability-delete-loop`; optionally `--stability-smoke`.
- **Routing/audio callback/scratch buffers/mix graph:** `--stability-smoke`,
  `--stability-delete-loop`; mixdown wav/mp3 if rendering is affected.
- **Export/mixdown:** `--stability-mixdown` wav/mp3 only.

Low iteration counts (2–3) are fine for a targeted regression check; use 5+ only when hunting
intermittent problems.

### Level 3 — Full stability matrix

Run **only** when:

- explicitly requested by Niclas,
- finishing a batch of runtime changes before commit,
- the change touches multiple core subsystems,
- a fix follows a crash/use-after-free/race,
- before packaging a tester build,
- after routing/session/audio-callback/plugin-lifetime changes,
- after stability tooling/invariants themselves changed.

```powershell
.\scripts\stability-matrix.ps1 -Project "<reference.dalproj>" -Iterations 5 -IncludeMixdown -IncludeAutosave
```

Do **not** run Level 3 automatically for every small UI change.

### Level 4 — Release certification

Run **only** when preparing a tester/release build. See `docs/RELEASE_CERTIFICATION.md`.

```powershell
.\scripts\certify-release.ps1 -Project "<reference.dalproj>" -Iterations 5
# Optional: -IncludeAsan -IncludePageHeap
```

---

## Subsystem-to-test mapping

| Changed area | Required normal tests | Optional stronger tests |
|---|---|---|
| Docs/scripts only | Level 0 build (or none if no code is compiled) | — |
| Main window / UI bounds | Debug build + manual bounds save/reload | load-loop, 3 iterations |
| MIDI editor UI only | Debug build + manual check in editor | — |
| MIDI note model/editing | Debug build + manual note edit test | MIDI-specific scenario if implemented; full matrix only if note model/persistence changed broadly |
| Project save/load schema | Debug build + load-loop (2–3) + manual save/reload of a real project | autosave/recover; full matrix before commit batch |
| Autosave/recovery | Debug build + `--stability-autosave` + `--stability-recover-autosave` | full matrix before release |
| Mixdown/export | Debug build + `--stability-mixdown` wav/mp3 (asserts exact file set, temp cleanliness, controlled failure, playback health) + `MixdownPreGainFocusedTests` (LAME runner, progress window on screen) + manual dialog click | full matrix before release |
| Audio-track pre-gain / mix helpers | Debug build + `MixdownPreGainFocusedTests` + `--stability-pregain` on the audio-only fixture (`--make-fixture`) | InputRoutingFocusedTests; smoke |
| VST3 insert chains / insert persistence (`PluginInsertHost`, `tracks[].inserts`) | Debug build + `InsertPersistenceFocusedTests` (real DAL Mono Delay + one other VST3 on audio / instrument / group / master rows; unavailable-plugin placeholder; removal) + `--stability-inserts` on its `--make-fixture` project (Save, reload, autosave recovery, missing plugin) | `PluginInsertTempoFocusedTests`; smoke; open-save-close on a real project |
| Export levels / meters / Inspector channel panel (`LevelMeterAccumulator`, `PlaybackEngine` meter taps, `MixdownExportLevelReport`, `InspectorPanel`, `ChannelStripPanel`, `ChannelFaderComponent`, `LevelMeterComponent`) | Debug build + `ExportLevelFocusedTests` (realtime callback vs offline render sample identity with stub device; accumulator; fader scale; −∞/0/+6 dB round trip) + `--stability-export-levels` on a project copy (realtime Stereo Out vs float/24-bit/MP3 exports, per-track sweep, −12 dB master check; analyse the kept files with `ExportLevelFocusedTests --analyze`) + `--stability-inspector-panel` (panel per row kind — hidden for MIDI / none, Stereo Out meter only on the master row — meters while playing, overload latch, fader paths, scrolling, low window, PNG evidence) | `--stability-mixdown` wav/mp3; `MixdownPreGainFocusedTests`; manual listening of an export |
| Live MIDI input / monitoring / recording (`LiveMidiInputBus`, `LiveMidiInputCoordinator`, `LiveMidiTakeBuilder`, `RecordingCoordinator` take paths, `Track::midiInputAssignment`, pitch bend in the clip model / render / proxy / export, Inspector `MIDI Input`, header Monitor/Arm on Instrument + Midi rows, `ProxyPlaybackCoordinator::liveMonitorRequested`) | Debug build + `LiveMidiRecordingFocusedTests` (bus routing / ownership / overflow / time mappings, take builder boundaries, v24 round trip, pitch-bend export) + `--stability-live-midi <project copy>` (drives the USER'S controls — header Monitor/R cell clicks through the header's own hit test, Inspector combo picks through the combo's own `onChange` — and checks session vs published routing after each step; the 1.1.10 wrong-row sequence; refusal texts for no input / ghost device; production Record / count-in / Stop with injected device-style MIDI; monitoring with stopped transport; **measured audio** from a live note on a project with a loaded instrument; channel filters + Force mapping to one host; Monitor-off release; real physical port picked by name, opened + Device-mode routing; Cycle guard; take content incl. held notes / sustain / CC / pitch bend; undo/redo; save/reload; SMF export; editor open; offline export without live leak; playback; empty take; audio + MIDI combined take). `LiveMidiRecordingFocusedTests --hold-midi-input <name> <s>` holds a port from another process to provoke the busy-port explanation (the RME driver is multi-client, so it does not trigger there) + `MiniDAWSelftests` (proxy override) + `TrackHeaderColumnFocusedTests` (live-MIDI rows). With a loopMIDI-style loopback port installed the scenario drives the real device path automatically. | `--stability-midi-routing`, `midi-track-parity`, `midi-editor-move`; `ExportLevelFocusedTests`; the user's keyboard for audible confirmation |
| MIDI clip layering + cycle takes + take preview (`MidiLayeredRenderBake.h`, `publishRenderSnapshot` / `audioThread_scheduleTransportMidiForSegment` merged note list, `ProxyOfflineSequencer`, `ProxyFingerprint` schema 2, bus anchors / wrap markers, `buildTakePasses`, `RecordingCoordinator` cycle commit, recording-row clip suppression, `MidiEventLane` preview geometry) | Debug build + `MiniDAWSelftests` (layer helpers; sequencer: two overlapping takes with sustain / CC11 / wheel, empty top clip over notes, long note cut + resumed, Upper/Lower/Pedal per-source selection, channel-sharing sources, stack-order swap; fingerprint schema 2) + `LiveMidiRecordingFocusedTests` (anchors across a wrap incl. late delivery, several wraps per dispatch, stop anchor, placement offset once; `buildTakePasses`: 4 passes with held key / pedal / wheel across wraps, controller-only pass, stop on a wrap, silent passes, after-stop discard, linear) + `--stability-midi-cycle-takes <project copy>` (real Record / count-in / Stop with Cycle on: windows, contents, one undo step, playback / delete-top / undo / redo / reload / mixdown / fresh proxy sequencer selecting the topmost take, preview geometry at start / after wrap / zoom / scroll with PNGs under `%TEMP%\dal-stability-midi-cycle`, editor open across delete / undo / move, recording-row suppression, combined audio + MIDI cycle pass boundaries) | `--stability-live-midi`, `midi-routing`, `midi-track-parity`, `midi-editor-move`, `organ-dc`, `mixdown`; `ExportLevelFocusedTests`; a real keyboard + ears for the attack restart of a resumed note |
| Instrument mute / plug-in residual signal (`PlaybackEngine` instrument loops, `renderInstrumentPostStripToStereoScratch`, `ExperimentalInstrumentHost` MIDI buffer) | Debug build + `--stability-organ-dc <project copy>` (selects the VB3-II row like a header click with the transport stopped; measures DC and AC RMS of the row's current post-strip samples and of Stereo Out in 2 s windows before playback, muted, unmuted, after 4 s playback + stop, muted again; asserts the instrument host keeps processing blocks while muted) + `ExportLevelFocusedTests --probe-vst3-isolate <bundle> <project> <trackId> <outDir>` when a plug-in is suspected (fresh instance per comparison, one parameter at a time, writes a `.vstpreset` for an external host) | `--stability-export-levels`; `InputRoutingFocusedTests` (mix helpers) |
| Track-header column / arrangement boundary (`TrackLanesView`, `TrackHeaderView`, `TransportLayoutHelper`, `UiLayoutSettingsStore`) | Debug build + `TrackHeaderColumnFocusedTests` (geometry + offscreen paint per row kind at 120/132/144/240, clamp formula, ui-layout.xml round trip) + `--stability-header-column` on a real project (every header/lane/ruler/overlay on one boundary at startup/default/minimum/wide, add/delete track, persistence, PNG evidence) | InputRoutingFocusedTests (header paint); manual drag of the boundary handle |
| Routing/master/groups/sends | Debug build + smoke + delete-loop | mixdown wav/mp3; full matrix |
| Instrument/plugin hosting | Debug build + smoke + manual plugin load/edit | delete-loop; ASan delete-loop |
| Track delete/undo/redo | Debug build + delete-loop | smoke; full matrix |
| Stability tooling/invariants | Debug build + the scenarios the tooling change affects | full matrix (tooling changes gate everything else) |
| Installer/package | Release build + install + launch + open reference project | Level 4 certification |

---

## Known accepted visual tradeoffs (do not file as regressions)

- **Arrangement waveforms are approximate during active zoom/pan.** Audio lanes blit the previous
  wave raster scaled while the viewport is moving, so waveforms may look blurry/soft and a rapid
  zoom-out can briefly show a clipped waveform. Each lane rebuilds once after ~200 ms of viewport
  idle and then renders correctly. This is a deliberate part of the main-window playback/zoom
  responsiveness fix (see `docs/CURRENT_ARCHITECTURE.md`, current-time/playhead rendering).
  Report as a bug only if the waveform is still wrong **after** the viewport has been idle, or if
  chrome/labels/selection are missing. Higher-fidelity rendering during the gesture is future
  polish, not a correctness blocker.

---

## Default test discipline for implementation agents

- **Do not run the full stability matrix** unless the task explicitly asks for it or the change is
  high-risk per Level 3 criteria above.
- **Always report which test level was chosen and why.**
- Prefer the smallest test that can falsify the change.
- If a small targeted test fails, **stop and report** — do not mask it by running larger suites.
- If the change touches multiple high-risk subsystems, ask or explicitly justify escalating to
  Level 3.
- Every implementation report must include:
  - tests run,
  - tests intentionally **not** run,
  - the reason,
  - whether a full matrix run is recommended later (e.g. "before the next tester build").
