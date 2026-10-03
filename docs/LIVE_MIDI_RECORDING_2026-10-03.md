# Live MIDI input: monitoring and recording (1.1.10)

| | |
|---|---|
| Date | 2026-10-03 |
| Baseline | 1.1.9 (`639b2e2`) |
| Delivered as | 1.1.10, commit `aeafa90` on `origin/main` |
| Build with the changes | `build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe` (Debug), `build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` + `dist\DanielssonsAudioLab-1.1.10-Setup.exe` / `dist\DanielssonsAudioLab-1.1.10.zip` (Release) |
| Evidence | `docs/evidence/live-midi-2026-10-03/` |
| Test material | sibling copies of `%TEMP%\dal-tse-copy\TSE_pt2.dalproj` (180 BPM, VB3-II, AmpliTube) and `%TEMP%\dal-pregain-fixture\pregain-fixture.dalproj` (120 BPM, audio-only). The user's projects were never opened for writing. |

## 1. Scope

A MIDI keyboard can be assigned to an Instrument row or a plain MIDI row (`MIDI Input` + `Input
Channel` in the Inspector, persisted in the project), heard live through the row's instrument
(Monitor) and recorded as ordinary, editable MIDI clips (R + Record) — alone, on several rows at
once, and together with the existing audio take. Linear takes only: with Cycle on and armed MIDI
rows, Record refuses with a message and leaves Cycle as it is. Audio-only cycle recording is
unchanged.

Out of scope (unchanged / documented): mixer, F3, generic PDC, external MIDI clock, MPE, SysEx,
new automation editors, comping / punch-in, loop recording of MIDI, recording of aftertouch /
channel pressure / program change (aftertouch is *monitored*, program change is dropped).

## 2. How it is wired (flow map)

```
keyboard ──JUCE MidiInput thread──▶ LiveMidiInputCoordinator::SlotCallback (one per device slot)
                                        │ deviceThread_push(slot, msg)        [lock-free SPSC ring per slot]
                                        ▼
PlaybackEngine::audioDeviceIOCallbackWithContext
   beginAudioBlock(all hosts) ──▶ LiveMidiInputBus::audioThread_dispatch(ctx)
                                        │ routing snapshot (acquire) · session snapshot · instrument snapshot
                                        ├─ monitor:  host->audioThread_addMidiEventForCurrentBlock(offset, msg')   msg' = row output mapping applied
                                        │            live-note ownership table (host + effective channel per sounding note)
                                        └─ capture:  CapturedEvent{trackId, timelineSample, bytes, playing} ──▶ capture ring (SPSC)
   transport MIDI scheduling (unchanged) ──▶ same host block buffer ──▶ merge with UI MIDI ──▶ Force mapping ──▶ plugin / capture sink
                                        ▲
message thread, 30 Hz ◀─────────────────┘ LiveMidiInputCoordinator::timerCallback
   drains the capture ring → per-row TakeStateTracker (always) + take event list (while a take runs)
   activity dot · routing rebuild when the session changed · device open/close on device-set change

RecordingCoordinator (Record key / Stop button)
   numpadRecordToggled: armed audio track and/or armed+configured MIDI rows → Cycle guard → count-in
   completeCountInAndStartRecording: ONE boundary = playhead when the count-in ends
        → RecorderService::beginRecording (audio) and/or beginMidiTake(boundary)
   stopRecordingAndCommitFromUi: stop → boundary = stop playhead
        → UndoRedoCoordinator::executeUndoableRecordingCommit("Record take", { addRecordedTake(audio); commitMidiTake(stop) })
   LiveMidiInputCoordinator::commitTake → LiveMidiTakeBuilder::buildTakePattern per row
        → InstrumentTrackController::appendRecordedTimelineMidiClip (clip window = the take)
```

## 3. Responsibility map

| File | Responsibility |
|---|---|
| `src/engine/LiveMidiInputBus.h/.cpp` | Realtime core: per-slot device rings, published routing snapshot, dispatch with sample offsets, live-note ownership, overflow policy, capture ring, the two time mappings. No allocation / locks / host dereference on the audio thread. |
| `src/app/LiveMidiInputCoordinator.h/.cpp` | Message thread: device enumeration + stable slots, enable/disable on `AudioDeviceManager`, per-slot `MidiInputCallback`, runtime Monitor/Arm flags, routing snapshot from `Track::getMidiInputAssignment` + flags, capture drain, take begin/commit/abort, status text, activity. |
| `src/app/LiveMidiTakeBuilder.h` | Pure take finalization: note pairing, velocity-0 = Note Off, held keys at the start boundary, controller/pitch-bend state restated at tick 0, notes + sustain closed at the stop boundary, empty vs controller-only, project tempo/PPQ tick math. |
| `src/domain/Track.h/.cpp`, `SessionSnapshot`, `Session` | `TrackMidiInputAssignment` (None / Device / AllEnabled, device identifier + name, channel filter) via the COW route; `setTrackMidiInputAssignment` (Instrument/Midi rows only). |
| `src/io/ProjectFile.h/.cpp` | **v24**: `tracks[].midiInput` / `midiInputDeviceId` / `midiInputDeviceName` / `midiInputChannel` (omitted = None); `clips[].pitchBend[]` (omitted when empty). Pre-v24 files load with None and no pitch bend. |
| `src/ui/experimental/ExperimentalMidiPitchBend.h`, `ExperimentalMidiPattern.h` | `MidiPitchBendPoint` (raw 14-bit, native channel, hold semantics) on the clip pattern. |
| `src/instruments/InstrumentTrackController.h/.cpp` | Pitch-bend render streams (`InstrumentPitchBendRenderStream`), `audioThread_schedulePitchBendForSegment` (chase + dedup like CC), DTO ↔ clip mapping, `appendRecordedTimelineMidiClip`. |
| `src/instruments/ProxyRenderSnapshot.h`, `ProxyFingerprint.h`, `ProxyOfflineSequencer.h` | Pitch bend in the proxy snapshot, fingerprint (section written only when present — existing generations keep their fingerprint) and offline sequencer. |
| `src/instruments/ProxyPlaybackCoordinator.h` | `Dependencies::liveMonitorRequested`; a Current proxy is replaced by the live Secondary while monitoring needs it (`isLiveMonitorOverrideActive`). Currency untouched. |
| `src/io/InstrumentMidiClipExport.h/.cpp` | Pitch Wheel events in the SMF export (`pitchBendEventsExported`). |
| `src/app/RecordingCoordinator.h/.cpp` | MIDI-only and combined takes, Cycle guard, one boundary, `isRecordingInProgress()`, atomic undoable commit, abort on project replace. |
| `src/app/UndoRedoCoordinator.h/.cpp` | `executeUndoableRecordingCommit`: one step for timeline + instrument musical changes. |
| `src/engine/PlaybackEngine.h/.cpp` | Bus installation, per-block dispatch after `beginAudioBlock`, discard under the offline-export gate / suspended instruments / device start, record placement offset. |
| `src/ui/InspectorView.h/.cpp`, `TrackLanesEditCoordinator.cpp` | `MIDI Input` + `Input Channel` combos, status line, undoable "Set MIDI input". |
| `src/ui/TrackHeaderView.h/.cpp`, `src/app/InstrumentTimelineRowCoordinator.h/.cpp` | Working Monitor / Arm cells on Instrument and Midi rows, activity dot, growing REC region on the lane during a take. |
| `src/app/MainAppWindow.cpp` | Composition: coordinator construction, recording seam, header/lane seam, Inspector providers, project-replace cleanup, `anyRecordingInProgress()`, stability hooks. |
| `src/diagnostics/StabilityScenarioRunner.h/.cpp` | `--stability-live-midi <project>`. |
| `tests/selftest/LiveMidiRecordingFocusedTestsMain.cpp` | Device-free focused tests (79 checks). |

Why this split: the bus is the only realtime-touching piece and is testable without devices or
hosts (fake host pointers, a delivery callback); the coordinator owns everything that needs JUCE
devices, the session and the message thread; the take builder is pure so every boundary rule is
asserted against literal event streams; recording keeps its single entry points and gains one
boundary and one atomic commit instead of a parallel MIDI recorder.

## 4. Time model (what the clip positions mean)

- Device callbacks stamp messages with `juce::Time::getMillisecondCounterHiRes()` (JUCE's
  Windows backend). The audio callback reads the same clock on entry and remembers the previous
  entry.
- **Live delivery offset**: events received in `(previousCallback, now]` are spread
  proportionally over the block's samples; events arriving while the callback runs clamp to the
  last sample. Inherent latency: one block (+ device output latency). Monitoring works stopped
  or playing.
- **Recorded position**: `playheadAtBlockStart − (now − t)·sr + placementOffset`, with
  `placementOffset = −reported output latency` (what the player heard at time `t` was rendered
  that much earlier). Clamped to at most 2 s back and never ahead. The UI playhead, repaint timing
  and the drain time play no part. This is a fixed compensation reported in the stability log
  (`outputLatency=168` on the RME at 48 kHz) — it is **not** the audio input recording offset and
  there is no plugin-latency compensation (no PDC in this slice).
- Ticks come from the **project tempo / PPQ** (`relativeSamplesToTicks`), nothing assumes 120 BPM.
  Measured in-app (device-style timestamps, 512-sample blocks): recorded vs expected tick
  3272/3277 and 5350/5351 at 180 BPM, 2340/2343 and 3729/3731 at 120 BPM (≤ 1.5 ms).

## 5. Behaviour

| Situation | Heard live | Recorded |
|---|---|---|
| Monitor on, transport stopped | yes (host of the row's destination) | no |
| Monitor on, playback | yes, added to the clips playing | no |
| Record running, row armed, Monitor off | no | yes |
| Record running, row armed, Monitor on | yes | yes |
| Row not armed | per Monitor | no |

- Channel: the `Input Channel` filter only decides which incoming channel is accepted; recorded
  events keep their received channel; delivery applies the row's `MIDI Channel` (Preserve / Force)
  exactly like playback; the Secondary's forced mapping is applied at the host boundary and never
  written back. One event is handled once per matching row; several configured rows may deliberately
  receive the same input (Upper / Lower / Pedal on channels 1 / 2 / 3 into one VB3-II instance).
- Note lifetime: every delivered Note On is remembered with its host and effective channel; the
  Note Off goes there even if the output channel, routing, Monitor, device presence or destination
  host changed meanwhile. Monitor off / input or routing change / device loss / track or project
  removal release exactly that row's live notes — never a global All Notes Off. Stop still runs the
  existing transport flush; new key presses after Stop work normally. The 1.1.9 mute rule holds:
  live MIDI into a muted row is processed by the host at gain 0, never accumulated.
- Primary / proxy / Secondary: Primary when usable; otherwise, while a monitored row targets the
  destination and a Secondary is configured, the Secondary becomes the transport source
  **temporarily** for the clips and the live input (one instance — a proxy and a live instrument
  never play the same part together); with neither, MIDI is still recorded and the Inspector says
  it is not heard. Monitor on/off never dirties the project or the proxy; a committed take is a
  musical edit and invalidates the proxy through the normal fingerprint path.
- Recording: one new clip per row and take, clip window = the take; existing clips untouched; no
  quantisation; no CC interpolation invented (recorded points are Hold); keys held across the
  start enter at tick 0 with their velocity; sustain / expression / modulation / volume / pedal
  controllers and pitch bend known at the start are restated at tick 0; keys held at Stop end on
  the stop boundary and a held sustain pedal is released there; an empty take creates no clip and
  no undo step; a controller-only take is content. Audio + MIDI in one take commit in ONE undo
  step (the audio take itself is now undoable as part of that step). Project replace during a take
  aborts the MIDI take without clips. The MIDI editor is never opened automatically.
- Overflow: a full device ring (2048 events) drops the event, counts it, releases that device's
  live notes on the next block and marks the take; the Inspector status reports the drop. A full
  capture ring (16384) is reported after the commit. Nothing is dropped silently.
- UI: Monitor (speaker) and R on Instrument **and** Midi rows (the instrument placeholder is now
  live; Midi rows gain the cell, 4 cells at the 132 px minimum); a green dot at the name row while
  MIDI arrives; the Inspector's status line (device missing / no playable instrument /
  Secondary temporary / overflow); the lane shows a growing red REC region during a take.

## 6. Verification

| What | How | Result |
|---|---|---|
| Bus: time mappings (256/1024/64 blocks, 44.1/48/96 kHz), message filter, routing by slot + channel filter, Preserve/Force delivery to the one host, exactly one delivery per (event, route), sample offsets, capture stamps, Note Off follows the Note On's channel after a setting change, Monitor off / device loss / host swap release notes, stopped-transport capture, overflow release + marker, export-gate discard, two concurrent device threads | `LiveMidiRecordingFocusedTests` (automated) | 79 checks, 0 failures |
| Take builder: positions / lengths / velocity / channel, velocity 0, held at start, chord held through, note + sustain at stop, boundary controller state, empty vs controller-only, retrigger, 44.1 kHz @ 180 BPM, post-stop events, overflow marker | same | included above |
| v24 project round trip (device / All / None, channel filter, pitch bend 14-bit), pre-v24 simulation, MIDI export of pitch wheel under Preserve and Force | same | included above |
| Proxy policy: Monitor + Secondary → SecondaryLive replaces a Current proxy, Monitor off → proxy restored, no Secondary → proxy kept | `MiniDAWSelftests` (automated) | 3225 checks, 0 failures |
| Header cells at 120 / 132 / 144 / 240 px for the live-MIDI instrument and Midi rows (present, inside, hittable, painted) | `TrackHeaderColumnFocusedTests` (automated) | 149 checks, 0 failures |
| In-app, production paths (`--stability-live-midi`): Inspector controls + persistence, Monitor with stopped transport reaches the host and leaves the project clean, channel filters, 1/1/1 to one host, Monitor off releases only its notes, real RME MIDI port opened via the device manager with the slot callback registered + Device-mode routing + Inspector shows the device, Cycle guard, MIDI-only take through the real Record / count-in / Stop (held-at-start, timed note, held-at-stop, sustain release, CC11, pitch bend, Lower on received channel 5, one undo step), undo/redo atomic across rows, save/reload, SMF export, editor opens on the clip, offline export with a live note held (no leak, clip rendered), playback of the take, empty take, **audio + MIDI combined take in one undo step** | automated in the running app, on the 120 BPM fixture (PASS) and the 180 BPM TSE copy (PASS up to the project reload, where the known AmpliTube 4 teardown crash `0x7675E` ended that run) | PASS |
| Visual | PNGs in `docs/evidence/live-midi-2026-10-03/` (recording with Monitor/Arm lit and REC regions; Inspector with the device; header cells at 132 px; MIDI row Inspector controls) | checked |
| Regression | `--stability-midi-routing`, `midi-track-parity`, `midi-editor-move`, `inspector-panel`, `header-column`, `organ-dc`, `mixdown wav`, `pregain`, `inserts`; `ExportLevelFocusedTests` 47, `InputRoutingFocusedTests` 56, `MixdownPreGainFocusedTests` 49, `InsertPersistenceFocusedTests` 121 | all PASS |

**Not verified here — needs the user's keyboard:** no virtual MIDI loopback driver exists on this
machine (only "Babyface Midi Port 1"), so no MIDI message travelled through a physical port's
driver thread; the scenario injects at the bus's device-thread entry from a separate thread with
device-style timestamps and uses the real port only to prove enumeration, opening, callback
registration and Device-mode routing. The scenario detects a loopMIDI-style port automatically
(`--stability-live-midi` logs `injection path: REAL loopback MIDI port`) when one is installed.
No listening test was performed: the fixture destination is a plugin-less shell with a capture
sink, so "delivered Note On" is proven, audible sound is not.

## 7. Known limitations

- Aftertouch / channel pressure are forwarded for monitoring but not recorded; program change
  is dropped; SysEx / MPE unsupported.
- Linear takes only for MIDI; cycle must be off (message, Cycle untouched).
- Fixed placement compensation = reported output latency; no PDC, no user offset for MIDI yet.
- The device manager persists enabled MIDI inputs in `audio-device.xml`; devices referenced by a
  project are opened when it loads and closed when no row references them.
- The pitch-bend data has no editor lane (playback, persistence, export, proxy and undo only).

## 8. User guide

1. Select the instrument row (e.g. VB3-II) or a MIDI row routed to it (`MIDI To`).
2. Inspector → **MIDI Input**: pick your keyboard (or *All MIDI inputs*). **Input Channel**: *All*,
   or the channel the keyboard sends on. For VB3-II Lower / Pedal use MIDI rows with `MIDI To` =
   VB3-II and `MIDI Channel` = 2 / 3.
3. Click the **speaker** (Monitor) on the row header to hear the instrument while you play — with
   the transport stopped or playing.
4. Click **R** on every row you want to record (several at once is fine; an audio track may be
   armed at the same time).
5. Press **Record** (numpad `*`): 8 count-in clicks, then the take starts at the playhead. Keys
   already held enter at the start; the REC region grows on the lane.
6. Press **Stop** (or Record again): the take becomes one clip per row. **Undo** removes the whole
   take; **Redo** brings it back.
7. Play it back; open the clip in the MIDI editor, move / copy / delete it, save, export MIDI — a
   recorded clip is an ordinary clip.
