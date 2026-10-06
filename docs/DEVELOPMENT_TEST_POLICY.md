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
| Record run / shared stop boundary / extent growth / proxy recording / schema-1 comparability (1.1.13: `PlaybackEngine` record-run handshake, `RecordingCoordinator::stopRecordRunAndCollectBoundaries`, `Session::restoreArrangementExtentAfterRecording`, `ProxyPlaybackCoordinator::liveRecordingRequested`, `comparableFingerprintSchemaFor` / `snapshotIsLayerInsensitive`, `ProxyCurrentIdentity::publishedComparableFingerprint`, Inspector status without the event counter) | Debug build + `MiniDAWSelftests` (`layer-compat` currency checks; `p1e-cmp` scheduler verdict + re-render consistency) + `--stability-midi-cycle-takes <project copy>` ("past-end" steps: MIDI-only across the old end with a 1.5 s UI stall, audio-only frames = clip length, combined equal lengths, Cycle keeps the stored extent at max(before, R); "device-stop" steps: device closed mid-take, bounded Stop with `acked=no`, clip committed, device restarted; combined cycle / linear takes with identical audio and MIDI lengths and separate placement offsets logged as `[Rec] run boundaries`) + `--stability-proxy-recording <project copy with a schema-1 proxy + a Secondary donor row>` (simulated no-Primary machine through `proxyForcePrimaryUnavailable`: compatible schema-1 generation stays ProxyCurrent without a re-render, broken pairing never Current; proxy destination recording with no Secondary (backing kept, status), with a Secondary and Monitor off / on (SecondaryLive, override flags, own clip silent at the Secondary, live note heard only with Monitor on, status texts, override ends at Stop, real currency after Undo); two source rows into one proxy destination after a Render-now re-render (the other row is heard through the Secondary while the recording row's clips are silent, both directions)) + `--stability-live-midi` (Inspector status must not contain "MIDI received" / "Ready -" / "events") | `--stability-midi-routing`, `midi-track-parity`, `midi-editor-move`, `inspector-panel`, `header-column`, `organ-dc`, `mixdown`, `pregain`, `inserts`; all focused test executables. Block-size independence of the stop handshake is code-inspected only (fixed ASIO buffer on the development machine). |
| Proxy render readiness / no-output rule / publication identity (1.1.14: `verifyInstrumentReadiness`, `NoAudibleOutput`, `publishRenderedProxy` byte-identical reuse + sibling names, `ProxyOfflineSequencer::collectDistinctNoteOns` / `emitInitialControllerState`) | Debug build + `MiniDAWSelftests` (`p1d-ready`: late-loading fake waited for and the artifact sounds from its first block, immediate fake verified in 1 + settle passes, DC-parking fake still answers (AC-coupled) while the render fails honestly at the tail cap, readiness disabled; `p1d-nooutput`; `p1f-collide`: alien file / same-length-different-content / byte-identical sibling reuse / different length / directory under the canonical name / missing temp) + `--stability-proxy-render-probe <project copy> <trackId> <outDir> [--repeat N] [--publish] [--state-blob f] [--wait-after-prepare ms] [--no-readiness] [--realtime-indication]` on a project copy with a real sample-based instrument (TSE copy, Groove Agent SE row 3): readiness verified, first audible sample = first note, complete length in Debug AND Release, publication against a kept non-identical first generation file → sibling name, byte-identical re-render → reuse, saved copy reloads Current. Keep the probe copy between runs (it is not cleaned up) and never run it on a real project. | `--stability-proxy-recording` (Render now path), `--stability-midi-cycle-takes` (fresh proxy sequencer), `--stability-mixdown`, `ExportLevelFocusedTests --probe-vst3-dc` for a plug-in's post-note DC (VB3-II organ-dc) |
| Tail-policy v2 / DC-tracked tail + plausibility / resting-level EOF / policy comparability (1.1.15: `DcTrackingPeakMeter`, `ProxyTailDetector::feedBlock`, `maxResidualPeakLinear`, `comparableRenderPoliciesFor`, `ProxyPlaybackReader::restingLevelAtEnd`) | Debug build + `MiniDAWSelftests` (`p1d-tail`: 4 rates × 3 block sizes — constant DC / DC + decaying tone / tone without DC / 5-10-20-40 Hz decays / unequal and anti-phase L/R offsets / tail still above threshold at the cap / varying floor at −70 / changing offset; `p1d-ready` DC-parking fake now Succeeded with the parked offset handed to the render; `p1d-nooutput` DC-only and probe-only fakes; `tail-compat`: v1 and unrecorded comparable, other drift and newer versions not; F12 changes with the tail version; `p1g-rest`: same-rate / resampled / silent-ending continuation) + `--stability-proxy-render-probe <project copy> <trackId> <outDir> --publish [--tail-policy-v1 --retain-failed]` on a copy of the user's VB3-II project (row 5): v1 reproduces `TailLimitReached`, v2 Succeeded with the full music, Debug and Release byte-identical, reload Current + `--stability-proxy-playback-edges <project copy> <trackId> <outDir>` through the user's insert chain (EOF / past EOF / Stop / restart / loop wrap / mixdown, proxy vs Primary) + the Groove Agent probe as regression (older v1 generation reads Current on load, re-render complete). Evidence `docs/evidence/proxy-tail-policy-v2-2026-10-05/`. | `--stability-proxy-recording`, `--stability-organ-dc`, `--stability-mixdown`; all focused test executables; ears for the Start/Stop step (known, report §6) |
| Mixer window / concurrent meters (1.1.16: `MixerWindow`, `MixerContentComponent`, `MixerChannelStrip`, `MixerSectionLayout`, `MixerStripBindings`, `TrackEditActions`, `LevelMeterHub`, `TrackMeterBank`, `TrackChannelOptions`, `TrackValueFieldText`, `TrackStripButtonGlyphs`, `UiLayoutSettingsStore` mixer keys) | Debug build + `MixerFocusedTests` (bank slots / drains / overflow, hub fan-out — two views receive each window once, Master under its id, hidden views unmetered, acknowledgement without echo —, layout alignment, an offscreen strip bound to a NON-active row editing only that TrackId through the production setters, per-kind controls, Midi rows without audio controls, store round trip) + `--stability-mixer <project copy>` on a project with audio / instrument / MIDI rows (a Group is added when missing): F3 / close / reopen single instance, strip order + fixed Stereo Out under horizontal scrolling, independent section toggles with aligned bands (PNG evidence under `%TEMP%\dal-stability-mixer`), fader / pan / pre-gain / routing / send / insert-stage / mute edits of a non-active row reaching the session and the Inspector, concurrent meters (two rows + Stereo Out; Inspector = mixer on the active row), audio runtime fingerprint unchanged across open / close while playing, strips following delete / undo / reload, bounds + section flags persisted beside the header width; 1.1.17 adds `testDividers` (height clamps, `dividersFor`, `applyDividerDrag` redistribution incl. the fader band's spare), a synthesized press / drag / release + Ctrl-click on the pan stick of a NON-active strip, a six-insert chain with slot-bound row actions, and scenario steps 11–14 (pan drag → Inspector agreement, divider drags with `verifyLayout` in a 900 px window, hide / show keeping heights, six Post inserts listed / scrolled / removed by slot, `MIXER_SECTION_HEIGHTS` persisted and kept in a 500 px window) | `--stability-inspector-panel` (Inspector meters through the hub), `--stability-live-midi` on its pre-gain fixture (Inspector selectors through the shared builders), `TrackHeaderColumnFocusedTests` (shared glyphs + header plate), `ExportLevelFocusedTests`, `InputRoutingFocusedTests`, `MiniDAWSelftests`. Not covered: real OS pointer input (fader / pan / divider drags are handler-level gestures; the pan and fader components are the Inspector's, verified there) |
| Parallel live-instrument generation (1.1.18: `InstrumentRenderPool`, `ExperimentalInstrumentHost` render / add split, `PlaybackEngine` dispatch, `--instrument-workers`) | Debug build + `InstrumentParallelFocusedTests` (production callback + real hosts with a deterministic test instrument: serial vs parallel bit-identity, one `processBlock` per instance per block, Off / muted / instrument-less rows, Group + send, small-workload fallback, 0-worker override, offline mixdown parity, publish-before-destroy + device stop / start under a cycling device thread) + Release `--stability-perf-profile <100-track temp copy> --generation ab` (serial vs parallel in one process: budget %, overruns, start intervals, section wall, join wait) + `--stability-midi-routing`, `--stability-live-midi` (fixture), `--stability-smoke`, `--stability-mixdown` on temp copies with the parallel default | `ExportLevelFocusedTests`; a listening check at the actual buffer size. Report: `docs/PARALLEL_INSTRUMENT_RENDERING_2026-10-06.md` |
| Audio-thread cost diagnosis (`AudioThreadProfiler`, `--stability-perf-profile`; opt-in, never on by default) | Release build + `--stability-perf-profile <temp project copy> --seconds 45 --warmup 5` (engine load window, per-category / per-phase / per-instance profile, proxy underruns, output peak, process CPU / memory; `--profile-off` for the overhead comparison, `--mixer-open` for the UI comparison, `--buffer N` documents the driver's answer). A diagnostic, not a pass/fail gate; report format in `docs/PERF_PROFILE_100TRACKS_2026-10-06.md` | When the profiler call sites change: `ExportLevelFocusedTests`, `PluginInsertTempoFocusedTests` (compile the engine / insert host with the header) and one `--profile-off` run showing the same engine load as before |
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
