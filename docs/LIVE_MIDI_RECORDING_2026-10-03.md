# Live MIDI input: monitoring and recording (1.1.10)

| | |
|---|---|
| Date | 2026-10-03 |
| Baseline | 1.1.9 (`639b2e2`) |
| Delivered as | 1.1.10, commit `aeafa90` on `origin/main`; fix release 1.1.11 (commit `976f26d`, section "1.1.11 — field failure and fix"); layering + Cycle takes + preview in 1.1.12 (section "1.1.12 — layering, Cycle takes, take preview") |
| Build with the changes | `build\ninja-debug-2\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe` (Debug, FileVersion 1.1.12 — built in a separate directory because the user's 1.1.11 Debug exe was running), `build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` + `dist\DanielssonsAudioLab-1.1.12-Setup.exe` / `dist\DanielssonsAudioLab-1.1.12.zip` (Release, 1.1.12) |
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

## 1.1.11 — field failure and fix

**Report:** VB3-II instrument row, Power on, Mute off, Monitor orange, R red, "All MIDI inputs"
selected; playing gives no sound; Record answers *"Arm a track for recording first … also needs a
MIDI Input selected"*.

**Where the state went (established from the user's saved project and the code path).** The
user's project file, saved at the end of that session, carries `"midiInput": "all"` on **Track 8**
(another instrument row) and nothing on the VB3-II row — the Monitor / R flags are runtime-only
and were on the VB3-II row, the input assignment was not. The Inspector edits
`Session::getActiveTrackId()`. In 1.1.10 the instrument / MIDI row's Monitor and R cells toggled
their runtime flags but — unlike Mute, Power and the audio rows' R / speaker — did **not** activate
the row. Clicking Monitor + R on VB3-II therefore left the previously active row (Track 8) in the
Inspector; the pick "All MIDI inputs" was applied to that row. Both symptoms follow from this one
state transfer:

| Symptom | Mechanism |
|---|---|
| Monitor on, "All MIDI inputs" shown, keyboard silent | VB3-II's session assignment was still `None`, so the live-MIDI routing snapshot contained no route for it; the opened device's events matched nothing. Track 8 (the row that actually received "All") was neither monitored nor armed. |
| R red, Record refuses with "Arm a track …" | `armedTracksReadyToRecord()` found the armed VB3-II row but required a configured input (`None` → not ready); with no audio track armed the start fell into the generic "arm a track" message, which was wrong while R was on. |

The audio / routing chain itself was intact: with the assignment on the right row the same build
delivers live MIDI to the host and the host produces audio (measured below).

**Changes (1.1.11).**

- `InstrumentTimelineRowCoordinator`: the Monitor and R cells of Instrument / MIDI rows call
  `Session::setActiveTrack(laneTid)` like Mute, Power and the audio rows — the Inspector always
  shows the row whose buttons were just pressed.
- `TrackLanesEditCoordinator` → `LiveMidiInputCoordinator::refreshDevicesAndRouting()` right after
  a MIDI Input pick (devices opened, routing published at once, not on the next tick).
- `LiveMidiInputCoordinator`: per-row readiness (`inputAvailabilityForTrack`, `armedRowsStatus`)
  distinguishing *no input selected*, *device not connected (assignment kept)*, *device could not
  be opened (in use by another application?)* and *All MIDI inputs but no device connected*; open
  failures are logged and retried every 2 s while the device is wanted; the Inspector status says
  "Monitor / R is on, but this track has no MIDI Input", "Ready — no MIDI received yet on this
  track's input" / "MIDI received: N events" (these two counter texts were removed in 1.1.13), or
  the problem above.
- `RecordingCoordinator`: the refusal names the armed rows and their reasons; "Arm a track" only
  when nothing is armed. Ready rows record even if other armed rows are not ready (logged). The
  Cycle and device refusals are recorded for diagnostics (`getLastRecordStartRefusalForDiagnostics`).
- Validation is not more permissive: a row with no usable input still cannot start a MIDI take.
  Instrument availability is still irrelevant for capture (recording works into a plugin-less row).

**Verification through the previously failing workflow** (`--stability-live-midi`, now driving
the real controls: `TrackHeaderView::click…CellLikeMouseForStabilityTest` runs the header's own hit
test + callback; `InspectorView::chooseMidiInputByTextForStabilityTest` fires the combo's own
`onChange`):

| Step (user's real control path) | Result |
|---|---|
| Another row (Pedal) active; click R + Monitor on the instrument row's header cells | active row becomes the instrument row |
| Inspector status with R + Monitor on and no input | "Monitor and R are on, but this track has no MIDI Input - choose a device or All MIDI inputs above" |
| Record with R on, no input | refused: "LiveMidiDest: no MIDI Input selected (Inspector > MIDI Input) … The track stays armed" — not "Arm a track" |
| Pick "All MIDI inputs" + Input Channel 1 in the Inspector | session `inst = all ch=1`, Pedal unchanged; published route `slot=-1 (All) filter=1 monitor=yes capture=yes` immediately |
| Record | count-in starts |
| Pick the physical device ("Babyface Midi Port 1") by name in the Inspector | session `device:Babyface Midi Port 1`; device opened on the manager, slot callback registered; a message entering its slot is delivered |
| Ghost device (identifier no device here has) | Inspector "Ghost Keyboard (missing)" + "not connected (assignment kept)"; Record refused naming it; assignment kept; re-pick works |
| **Audible** (TSE copy, VB3-II loaded): All MIDI inputs via the Inspector, Monitor via the header cell, ch1 note 60 injected with the transport **stopped** | row post-strip meter 0.0000 before → **0.3126 / 0.3269 (−9.7 dBFS)** during the note |
| The rest of the scenario (filters, 1/1/1 to one host, Monitor-off release, MIDI-only take, undo/redo, save/reload, SMF export, editor, offline export without leak, playback, empty take, audio + MIDI combined take) | PASS on the fixture and on the TSE copy |
| Busy port: `LiveMidiRecordingFocusedTests --hold-midi-input Babyface 90` running alongside | the RME driver is multi-client — DAL still opened the port, so the "could not be opened" explanation path could not be provoked here (verified by code only) |

**Still open.** No key was pressed on a physical keyboard (no loopback driver on this machine);
the physical port is enumerated, opened and callback-registered, and audio from live MIDI is
measured through the whole chain from the bus's device-thread entry. The user's keyboard test is
the remaining confirmation.

## 1.1.12 — layering, Cycle takes, take preview

**Decided behaviour.** MIDI follows the audio lanes' overlap model: on each source track the
topmost clip owns its whole window `[start, end)` (rests and empty clips included), clips
underneath are heard only where nothing covers them, a new take or paste lands on top. The rule
is the same for all MIDI clips (no legacy mode, no migration dialog); underlying clips and their
data are kept. Selection per **source track** — the instrument's Upper clips and the routed Lower
/ Pedal rows are layered independently and still sound together.

**Where it lives.** `src/instruments/MidiLayeredRenderBake.h` (the one definition: audible spans,
note segmentation with resumption, controller streams restricted to the audible spans with a
chase restatement at every span start and the clip's own end events kept); used identically by
`InstrumentTrackController::publishRenderSnapshot` (one merged note list per track;
`audioThread_scheduleTransportMidiForSegment` walks it, so a same-pitch Off at sample X precedes
an On at X) and by `ProxyOfflineSequencer::bakeUnit`; the offline mixdown drives the live
scheduler. `ProxyFingerprint` serializes clips in stored order and bumps the schema to 2.
*(Corrected in 1.1.13 — the 1.1.12 text said both "stale" and "re-rendered": a stale proxy is
never selected for playback (Secondary or silence) and is re-rendered only in Auto mode on a
machine where the Primary is present; a schema-1 generation over provably layer-insensitive
content stays Current, see "1.1.13" below.)*

**Cycle recording.** `PlaybackEngine` keeps a monotone device clock; `LiveMidiInputBus` records
timeline anchors (play start / seek / wrap / stop) and pushes a wrap marker (exact mono sample +
transport wrap serial) at every wrap, from both wrap branches of the callback. Gestures are
mapped mono→timeline through the anchors (a key pressed before the wrap but delivered after it
lands before R), passes are the intervals between markers on the mono clock, the stop is read
as a consistent `(position, wrap serial)` pair, later markers / events are discarded.
`live_midi_take::buildTakePasses` builds `[start, R)`, `[L, R)` …, `[L, stop)` (no zero-length
pass on a stop exactly at the wrap), each pass from the real row state at the end of the previous
one (held key: ends on the pass end, continues at tick 0 with channel + velocity; sustain /
expression / wheel restated at tick 0; held pedal released on the pass end). With Cycle every
pass of positive length is a clip (silent and controller-only too — they mask like audio
passes); with Cycle off an empty take still creates nothing. Combined audio + MIDI cycle takes
slice the audio with the same stop wrap serial; the whole run is one undo step. Count-in once.
While a row records, its own earlier clips are not scheduled (no doubling); Monitor still rules
live audibility; other rows keep playing.

**Preview.** Exact time geometry: left edge = current pass start (left locator after a wrap,
from the transport's wrap count), right edge = the playhead overlay's frame position through
the playhead's transform; dim wash for earlier passes; 2 px start marker; "REC" only when it
fits; drawn above the clips; growth-strip invalidation per frame. The 40 px minimum-width
helper is no longer used for it (that helper remains for clip chrome and hit areas).

**Verification (what and how).**

| Check | How | Result |
|---|---|---|
| Spans / segments / stream restriction; two overlapping takes with sustain, CC11, pitch bend; rest + empty top clip; partial overlap mid long note with resumption; Upper + Lower + Pedal per-source; channel-sharing sources; stack-order swap; fingerprint schema 2 | `MiniDAWSelftests` (pure helpers + the offline sequencer with literal expected events) | 3246 checks, 0 failures |
| Anchors across a wrap incl. late delivery, several wraps between dispatches, stop anchor, placement offset applied once; `buildTakePasses`: 4 passes, held key / pedal / wheel across wraps, late-delivered pass-0 gesture, controller-only pass, partial last pass, stop exactly on a wrap, silent passes, after-stop discard, linear, key pressed after Stop | `LiveMidiRecordingFocusedTests` | 110 checks, 0 failures |
| Real UI path with Cycle on: Record / count-in / Stop, 4 passes on the instrument row and the routed MIDI row (windows, contents, boundary state), ONE undo step, playback selects only the topmost take (origin per source = channel, per take = pitch), delete top → previous heard, undo / redo both ways, MIDI editor open across delete / undo / move, save / reload keeps order and selection, offline mixdown and a fresh proxy sequencer select the same events, preview geometry at start / after wrap / zoom ×1.5 / zoom ×4 + scroll with PNGs (`%TEMP%\dal-stability-midi-cycle`), recording-row clip suppression (earlier take silent, live note heard), combined audio + MIDI cycle take with shared pass lengths in one undo step | `--stability-midi-cycle-takes` on the 120 BPM fixture and the TSE copy (VB3-II loaded) | PASS / PASS |
| Previous live-MIDI matrix (now with the Cycle step inverted: Record starts) | `--stability-live-midi` on the fixture and the TSE copy | PASS / PASS (−9.77 dBFS measured on VB3-II) |

Synthetic input (bus device-thread entry with device-style timestamps), real UI handlers (Record
key, header cells, Inspector combos, Delete key, undo / redo, save / reload), real engine +
instrument host (VB3-II on the TSE copy, capture sink on the fixture). **Not done:** a physical
key press (no loopback port on this machine) and listening — the measured audio and the
delivered-event traces stand in for both; the attack restart of a resumed note is audible only
with ears.

**Limitations (as shipped in 1.1.12; the first, third and fourth are resolved in 1.1.13 below).**
A destination playing a proxy with Monitor off still renders its earlier takes from the WAV
during a take (a proxy cannot mute one row). A controller the winning clip never defines keeps
its last delivered value (no reset is invented). Recording longer than 30 minutes past the
previous arrangement end would freeze the transport again (the extent grows once at record
start). The audio slice and the MIDI pass of a combined take may differ by one device block at
the stop (audio length comes from the recorder's sample count).

## 1.1.13 — corrections after 1.1.12

Five corrections, all through the production paths; commit after `0b23421`.

**1. Inspector without the event counter.** `LiveMidiInputCoordinator::describeInputStatus` no
longer produces "Ready — no MIDI received yet…" / "MIDI received: N events". A working input shows
nothing and the status area collapses (the layout already reclaims the space); incoming MIDI is
signalled by the header's activity dot. Real obstacles stay: no MIDI Input chosen, device not
connected (assignment kept), device could not be opened, All MIDI inputs with no device, no
destination, overflow. The per-route counters stay internal (`routeActivityCount`, tests).

**2. Recording into a destination that plays a proxy.** `ProxyPlaybackCoordinator` has a second,
independent need next to `liveMonitorRequested`: `liveRecordingRequested(destination)` — true
while a MIDI take (count-in included) runs on a source row into the destination
(`LiveMidiInputCoordinator::liveRecordingRequestedForDestination`, fed by the take rows or, during
the count-in, the pending rows via `setTakePending`). Either need switches a proxy-backed
destination with a usable Secondary to **SecondaryLive** for its clips and the live input; the
evaluation reports both flags (`liveMonitorOverride`, `liveRecordingOverride`), so Monitor and
Record stay separate functions sharing one mechanism. The Secondary is prepared off the audio
callback (lazy load on the message thread, published at a block boundary). Arming alone does
nothing; the override ends with Stop / abort / start failure / routing change
(`refreshProxyDestinationsForTakeRows`), and the normal decision is re-evaluated against the
real fingerprint — a committed take makes the generation Stale honestly, an undone one leaves it
Current; nothing is "restored" blindly, and identity / plugin state / metadata are untouched.
Without a usable Secondary the proxy keeps playing as backing, the take is still captured and
the status says "Recording: earlier material on this track is still heard from the proxy - no
live instrument is available" (no modal; other parts are not silenced). Status lines name the
temporary use: "Secondary instrument used temporarily while recording", "… (Monitor on,
recording)", "Monitoring through the Secondary instrument (temporary, while Monitor is on)". The
diagnostics registry now reports the row's *transport* host (Secondary while active), so the
bridge invariant compares like with like.

**3. One stop boundary for audio and MIDI.** `PlaybackEngine` gained a *record run* handshake
(`RecordRunState` Idle → StartRequested → Running → StopRequested → Stopped): the audio callback
stamps `RecordRunBoundary {monoSample, timelineSample, wrapSerial}` at the first sample of the
block where it acknowledges the start and the stop, before it reads the transport intent; the
recorder receives input blocks only while Running. `RecordingCoordinator` requests the stop,
waits up to 250 ms for the acknowledgement (`waitForRecordRunStop`) and uses that single
boundary for everything: `recorder` frame count = stop.mono − start.mono, MIDI take end =
stop.timeline with stop.wrapSerial (the `LiveMidiInputBus` wrap markers are cut at the same
mono sample), Cycle pass count = stopWrap − startWrap, audio placement = start.timeline +
latency-store offset, MIDI gesture compensation unchanged (`recordingPlacementOffsetSamples` vs
the MIDI offset are still separate, raw boundary vs compensated placement). If no callback
acknowledges (device stopped or lost) the wait returns with a message-thread fallback boundary
(monotone clock + UI playhead + wrap) flagged `acked=no`, the take is still finalized and the
recorder stops using the FIFO before anything is freed (`FinishRunAtExit`). Stop exactly on the
wrap still produces no zero-length take; the last partial pass is kept.

**4. Arrangement extent grows on demand.** The 30-minute reserve at record start is gone. While
a run captures (`recordRunCapturing`, independent of `recorder_->isRecording()` so MIDI-only
takes count), the engine's run-end rule treats the current block end as the timeline end
(`timelineEnd = max(extent, t0 + block)`): the transport never freezes at the old end and no
engine behaviour depends on a UI timer. A 10 Hz message-thread timer
(`RecordingExtentFollowTimer`) grows the stored extent to playhead + 5 s for the navigation area
(grow-only `Session::setArrangementExtentSamples`). At Stop
`Session::restoreArrangementExtentAfterRecording(storedBefore, resultEnd)` sets the extent to
max(stored extent before the run, derived timeline length, end of the recorded result) — the
previous extent plus what the result needs, no reserve. Cycle runs keep the extent (takes end at
R); older projects' saved extents are never shrunk; normal playback keeps its end rule;
locators and export range are untouched.

**5. Older proxies after schema 2.** Verified behaviour: a schema-1 generation whose fingerprint
no longer compares is **Stale** — never selected (Secondary or silence), re-rendered only in Auto
mode with the Primary present; the 1.1.12 wording ("auto re-renders") is corrected above. New:
`comparableFingerprintSchemaFor(meta, snapshot)` keeps a schema-1 generation comparable when the
current content is provably layer-insensitive (`snapshotIsLayerInsensitive`: no overlapping
clips per unit, stored order = plan order, every CC / pitch-bend point inside its clip window at
the reference rate) — only then the layered bake equals the additive bake; absence of overlap
alone is not enough because controller handling changed. The missing-Primary recompute and the
Primary-present verdict both use it; with the Primary present the render identity
(`ProxyCurrentIdentity::expectedFingerprint`) is always the current schema and the comparable
reading travels separately (`publishedComparableFingerprint`, `matchesPublished`). This also
fixed a defect found while verifying: comparing a schema-1 reading against a schema-2 job made
every re-render of such a destination end as "identity changed during finalizing — result
discarded" (observed on the TSE copy before the fix; publishes and becomes Current after it).
Proxy files, Primary identity and plugin state are untouched; Secondary rendering is never
presented as a Primary render; no option for the old additive playback.

**Verification (measured in-app unless marked "code").**

| Check | How | Result |
|---|---|---|
| Inspector status text for a working input contains no "MIDI received" / "Ready -" / "events"; no empty status area | `--stability-live-midi` (fixture + TSE) | PASS |
| Compatible schema-1 generation (GA row "Track 3", pub=83 = save=83, no overlap) on a simulated no-Primary machine (test seam `proxyForcePrimaryUnavailable`): runtime **ProxyCurrent**, destination Current, same generation after 1.5 s (no re-render); with the Primary present: destination Current through the comparable reading (`identity now=7bd0dd…  comparable=0eb66f… published=0eb66f…`) | `--stability-proxy-recording` (TSE copy) | PASS |
| Broken-pairing schema-1 generation (VB3-II, pub=975 / save=1) forced no-Primary: never Current → SecondaryLive (HALion Sonic), destination Stale | same | PASS |
| GA row, no Secondary, Monitor off, Record: proxy keeps playing (runtime ProxyCurrent during the take), status "Recording: earlier material … still heard from the proxy …", clip captured (1→2), after Stop ProxyStale (honest), after Undo ProxyCurrent | same | PASS |
| GA row with a Secondary (HALion Sonic config copied from VB3-II), arming alone: ProxyCurrent, no override; Monitor off take: SecondaryLive, `recordingOverride=yes`, own clip 0 note-ons at the Secondary, live note 0 (not heard), status "Secondary instrument used temporarily while recording"; Stop → override ends, destination Stale; Undo → ProxyCurrent | same | PASS |
| Monitor on take: live note 1 at the Secondary, own clip still silent, status "(Monitor on, recording)"; idle again: ProxyCurrent, no lingering override | same | PASS |
| Two source rows into one proxy destination (routed MIDI row with 48× note 120 added, re-rendered with the Primary via Render now → schema-2 generation Current, forced no-Primary again → ProxyCurrent): recording the GA row — routed row heard through the Secondary (4 note-ons), GA's own clip silent; recording the MIDI row — GA's own clip heard (3), routed row silent, MIDI row status names the temporary Secondary use, take captured, no lingering override | same | PASS |
| Common stop boundary: combined audio + MIDI Cycle take pass lengths audio 144000 / 39424 = MIDI 144000 / 39424 (1.1.12: 128-sample mismatch); linear combined take 85248 = 85248 with separate placements (audio start 170985 = 171136 − 151, MIDI gesture offset −168); `[Rec] run boundaries … acked=yes` | `--stability-midi-cycle-takes` (fixture + TSE) | PASS |
| Device stopped mid-take: Stop returns bounded (~263 ms), `acked=no` fallback boundary, clip committed, device restarted | `--stability-midi-cycle-takes` "device-stop" steps | PASS |
| Several device block sizes | code (the handshake stamps the first sample of the acknowledging block; nothing in it depends on the block size) — the RME ASIO buffer cannot be switched from the app | code only |
| Past the old end: MIDI-only with a 1.5 s UI stall (transport kept running: 247360 → 319424 past the 240000 extent; stored extent after Stop = result end, no reserve), audio-only (frames = clip length), combined (equal lengths, past the old end); Cycle did not change the stored extent beyond the loop end; TSE (1 h saved extent) unchanged | `--stability-midi-cycle-takes` "past-end" steps (fixture + TSE) | PASS |
| Regressions: `MiniDAWSelftests` 3276 (+ `p1e-cmp` scheduler checks, `layer-compat` currency checks), `LiveMidiRecordingFocusedTests` 110, `TrackHeaderColumnFocusedTests` 149, `ExportLevelFocusedTests` 47, `InputRoutingFocusedTests` 56, `MixdownPreGainFocusedTests` 49, `InsertPersistenceFocusedTests` 121; scenarios midi-routing, midi-track-parity, midi-editor-move, inspector-panel (TSE), header-column, organ-dc (TSE), mixdown, pregain, inserts | executables / `--stability-*` | 0 failures / PASS |

**Remaining limitations.** With Monitor off and no usable Secondary, a proxy destination keeps
rendering the recording row's earlier takes (shared proxy; reported in the status). A plug-in
that restores its state asynchronously reports a different live state revision for a moment
after load, so a destination can read Stale for that moment (observed ~1–2 s with Groove Agent
SE on the TSE copy; the verdict corrects itself once the state is restored — the scenario waits
for that before reading verdicts; not investigated further here).
The AmpliTube 4 teardown crash (`AmpliTube 4.vpa+0x7675E`) is still intermittent at shutdown.
Block-size independence of the stop handshake is verified by inspection only (fixed ASIO buffer
on this machine).

## 8. User guide

1. Select the instrument row (e.g. VB3-II) or a MIDI row routed to it (`MIDI To`).
2. Inspector → **MIDI Input**: pick your keyboard (or *All MIDI inputs*). **Input Channel**: *All*,
   or the channel the keyboard sends on. For VB3-II Lower / Pedal use MIDI rows with `MIDI To` =
   VB3-II and `MIDI Channel` = 2 / 3.
3. Click the **speaker** (Monitor) on the row header to hear the instrument while you play — with
   the transport stopped or playing. (Clicking the speaker or R also selects the row, so the
   Inspector shows the track you are configuring; its status line tells you when MIDI arrives.)
4. Click **R** on every row you want to record (several at once is fine; an audio track may be
   armed at the same time).
5. Press **Record** (numpad `*`): 8 count-in clicks, then the take starts at the playhead. Keys
   already held enter at the start; the REC region grows on the lane from the exact start to the
   playhead.
6. Press **Stop** (or Record again): the take becomes one clip per row. **Undo** removes the whole
   take; **Redo** brings it back.
7. **Cycle:** switch Cycle on, set the locators, press Record once. Every loop pass becomes its
   own take on top of the previous one (a pass you stayed silent in is a silent take that masks
   the earlier ones, exactly like audio). Playback plays the topmost take; delete it to hear the
   previous one. Keys, sustain and the wheel held across the loop point carry over correctly.
   Stop ends the last pass where you stopped.
8. Play it back; open the clip in the MIDI editor, move / copy / delete it, save, export MIDI — a
   recorded clip is an ordinary clip.
