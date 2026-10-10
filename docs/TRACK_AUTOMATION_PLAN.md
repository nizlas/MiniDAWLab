# Track automation, stage 1: Channel Volume and Pan (2026-10-10)

> **Status: planned, not implemented.** This document is the steering plan for the first
> automation stage. It does not change production code, the project format, or the 1.3.1
> read-ahead startup policy. Implementation starts only after this plan is reviewed.
>
> **Reviewed revision:** `3a6f3384b80dac6ff7663b538beb4dc446fbff0e` (`v1.3.1`, `origin/main`).
> Parallel audio/instrument strips (A1/A2) and the read-ahead prototype are on this
> revision. Read-ahead is on at depth 3 unless Audio Settings has a saved Off or the
> process was started with `--no-readahead` (`ReadAheadStartupConfig.h`). Project format
> is still `kCurrentVersion = 28` (`src/io/ProjectFile.h`). An implementation of this plan
> will need a later format bump; this planning change does not make one.
>
> File and function references below are against that revision.

---

## 1. Phase scope

One usable automation of Danielssons Audio Lab's own **Channel Volume** fader and **Pan**
control:

- Collapsible automation lanes under the main track in the arrangement.
- Read / Write, with **Touch** as the only write behaviour.
- The same gesture from the Inspector and from the mixer.
- The same audible result in realtime playback and offline export.
- One undo step per finished gesture. Curves, lane visibility, and Read survive save/load.
- A defined interaction with parallel strip jobs, experimental read-ahead, and instrument
  proxies.

The first delivery is this usable Volume/Pan behaviour, not a data model without UI.
The slices in §6 are the implementation order inside that one delivery.

Volume means the existing channel fader (`Track::channelFaderGain_`) at its current place
in the strip. Pan means the existing `Track::stereoPan_` and the existing linear balance
law. Neither moves.

## 2. Out of scope

- VST3 / instrument / insert parameter automation, including VB3-II. §4.16 only reserves
  an address shape.
- Pre-gain, mute, solo, Off, sends, input trim, or any control other than Channel Volume
  and Pan.
- A second write mode (Latch, Touch/Latch, Trim, or a dedicated automation lane record arm).
- Independent Read/Write per lane.
- Moving the fader to after the Post inserts, changing the pan law, or changing read-ahead's
  1.3.1 startup policy (Audio Settings checkbox, default on at depth 3, CLI override),
  depth cap, row selection, or thread model.
- Making ordinary manual fader/pan edits undoable. Today they are not; this stage does not
  change that. Only a finished automation gesture is an undo step.
- A general automation framework, bezier editor, or tempo-mapped automation.
- Rewriting `docs/CURRENT_ARCHITECTURE.md` or the historical wording of the parallel /
  read-ahead backlog rows. Those rows predate 1.3.0; the code on `3a6f338` (1.3.1) is the
  baseline this plan uses.

## 3. What the code does today

### 3.1 Signal order

The strip order is fixed and is the order automation must keep:

clips or instrument/proxy audio → pre-gain (ramped) → Pre inserts → **channel fader / mute**
→ Post inserts → **pan law** → routing and send tap.

`PlaybackEngine.cpp` states this at the top of the file (fader between Pre and Post, then pan).
`playback_mix_helpers` applies it for audio-clip rows, the instrument post-strip (live host
and proxy audio use the same helper; `PlaybackEngine.cpp` passes `fader` and
`tr.getStereoPan()` into `renderInstrumentPostStripToStereoScratch`), and the live-input
monitor path (`renderAudioTrackPostStripToStereoScratch` with the live input in place of
clips). Group and Stereo Out strips are serial on the callback; eligible audio-clip and
live-instrument strips run as parallel jobs. Offline export uses the same fader and pan
values on its own loop (`PlaybackEngine.cpp` around the offline instrument mix).

Mute and Off stay outside this stage. Effective fader gain is still
`effectiveTrackMuted(...) ? 0 : storedGain` (and Off still skips the row). Automating
volume does not write mute.

### 3.2 Who has a fader and a pan

| Kind | Channel Volume in the audio path | Pan in the audio path | Stage 1 lanes |
|------|-----------------------------------|------------------------|---------------|
| Audio | yes | yes | Volume and Pan |
| Instrument | yes (after the host or the proxy) | yes | Volume and Pan |
| Group | yes | yes | Volume and Pan |
| Stereo Out (`Master`) | yes | yes — the mixer treats every non-MIDI kind as an audio path and shows the pan field with the fader (`MixerChannelStrip`, `audioPath = kind != Midi`, `pan_.setVisible(showFader)`) | Volume and Pan |
| MIDI | no audio path | stored on the track, Inspector still lays the pan field out, but the row never enters the strip | **no lanes and no fictional fader** |

`ChannelStripPanel` shows the fader for every non-MIDI kind, including Stereo Out.
`InspectorView::resized` always places `panField_`. MIDI is the exception for lanes because
it produces no audio (`PlaybackEngine` routing plan: MIDI rows are not audio sources).

### 3.3 Stored values and the setters

- Volume is a linear gain in `[0, kTrackChannelFaderGainMax]` with `kTrackChannelFaderGainMax = 8`
  (`Track.h`). `0` is −∞. The travel is mapped to dB by `ChannelFaderScale` (graded, about
  −80 dB at the bottom of the graded region, +6 dB at the top). Non-finite values are rejected.
  Unity is `kTrackChannelVolumeUnityGain`.
- Pan is `sanitizeTrackStereoPan` into `[-1, +1]` (`TrackStereoPan.h`). The law is linear
  balance: centre is L = R = 1, so a centred track does not change level. It is not equal-power.
- `Session::setTrackChannelFaderGain` and `Session::setTrackStereoPan` publish a new
  `SessionSnapshot` on the message thread. They are not undo steps. Pan ignores a change
  smaller than `1e-6`.
- Inspector: `ChannelStripPanel` calls `setTrackChannelFaderGain` on every `mouseDrag`,
  guarded by `faderWiredGuard_` so a refresh does not echo. `InspectorPanControl::onPanChanged`
  calls `setTrackStereoPan`. Refresh uses `dontSendNotification` and skips the pan field
  while `isMouseButtonDown()`.
- Mixer: `MixerChannelStrip` calls the same two setters. Fader refresh uses
  `dontSendNotification`. Pan refresh is skipped while the mouse button is down.
- There is **no gesture begin/end**. `ChannelFaderComponent` has `dragging_`, Ctrl/Cmd+click
  resets to 0 dB, Shift is fine drag, the wheel steps 0.5 dB (0.1 dB with Shift), and a text
  field commits a value. `InspectorPanControl` has drag, double-click text, and the same
  reset idea. The mixer uses that pan control.

### 3.4 Session, undo, save, duplicate

The session snapshot is the project. Undo restores a snapshot
(`restoreSessionSnapshotForUndo`). Save, load, and autosave go through `ProjectFile`
(format v28). `channelFaderGain` and `stereoPan` are already per-track fields.

`SessionSnapshot::withTrackDuplicated` copies fader, pan, pre-gain, mute/off, routing,
sends, MIDI, and clips onto a **new** `TrackId`. The comment there says fields the
constructor does not take must travel through the same helpers as project load, so a new
field is not dropped. Master cannot be duplicated. One "Duplicate track" undo step.

### 3.5 Existing MIDI CC curves — what to reuse

`src/ui/experimental/ExperimentalMidiCcAutomation.h` is a pure, header-only evaluator.
Points are sparse, in the **tick** domain, hold or linear only, no bend. Before the first
point it returns "no value". After the last point it holds. Realtime playback consumes
events that were already written into the render snapshot; the audio thread does not
evaluate the curve and does not allocate.

Reuse that split: a pure function, a normalized stored value, last-wins identity, and a
snapshot the callback only reads. Do not reuse the tick domain, the 0..127 rounding, or
the hold/linear-only model. There is no arrangement curve editor to lift.

### 3.6 Time model

Audio clips, the playhead, and read-ahead segment keys are **audible samples**
(`timelineStartAudible`). `Session::setProjectBpm` is project metadata. MIDI notes and CC
stay in ticks inside clips and are converted when a render snapshot is built. A tempo edit
moves MIDI against audio; it does not move audio clips.

### 3.7 Fader and pan are block-constant today

Pre-gain ramps across the block (`PreGainRampState`, `exchangePreGainRampStart`) so a
target change does not zipper. The channel fader and pan are one multiply for the whole
segment (`scaleStereoScratch` / the pan-law multiply). A value that changes once per block
is a step. Moving automation has to ramp inside the block or it will zipper.

### 3.8 Read-ahead

`docs/READAHEAD_PROTOTYPE.md` (shipped). Since 1.3.1 the process depth is chosen in
`ReadAheadStartupConfig` before the engine is constructed: a start argument
(`--experimental-readahead[=N]`, `--no-readahead`, last one wins), else the saved Audio
Settings checkbox (`%APPDATA%\MiniDAWLab\read-ahead.xml`), else on at depth 3. Depth 0
still builds no renderer. The worker, when present, renders the **whole
strip**, so fader and pan are baked into the queued segment. §6: a fader, pre-gain, pan,
mute, solo, or plugin-parameter edit is heard late by at most `depth` segments, because
those segments were already rendered. Discarding the queue on every such edit was rejected
(it re-processes stateful plugins).

Monitor / record uses an immediate discard. If the worker is inside `processBlock`, the
row becomes Abandoning: silent, not processed concurrently, and the callback does not wait
for a bounded number of blocks. The race fix `b8eaf93` re-reads the ring after stop and
`busy == 0` before it declines a published prime.

### 3.9 Proxies

`ProxyRenderSnapshot` / `ProxyFingerprint` cover musical content (notes, CC, pitch bend,
plugin state revision). They do not include `channelFaderGain` or `stereoPan`. Playback
applies fader and pan at mix time, after the proxy buffer. A Volume or Pan change must not
invalidate the proxy.

### 3.10 Plugin parameters

`PluginInsertHost` holds a `juce::AudioPluginInstance` and calls `processBlock`. A search
of `src/plugins` finds no `getParameters`, `beginChangeGesture` / `endChangeGesture`,
`setValueNotifyingHost`, or `AudioProcessorParameter` use. JUCE exposes those on
`AudioPluginInstance` and `AudioProcessorParameter::getParameterID()`. DAL's wrapper does
not call them. Sample-accurate VST3 automation is not available through the current host.
VB3-II's Leslie speed and the control the user calls Presence are **not identified** in
this revision: the wrapper never enumerates that plugin's parameters, and this plan does
not guess an id or a function.

---

## 4. Recommended design

### 4.1 Four values, kept apart

| Value | Where it lives | Who writes it |
|-------|----------------|---------------|
| **Manual** | Existing `channelFaderGain_` / `stereoPan_` | The user, when Read is off. Also the value captured when Read is turned off. |
| **Curve** | New per-track automation data in the session snapshot | A finished Touch, or direct point edits on the lane. |
| **Gesture** | One slot per track on the message thread, published atomically (value + active flag). Not project data. | Mouse down/drag/up, text commit, Ctrl-reset, while the gesture lasts. |
| **Effective** | What the strip multiplies by for this block | Derived. Never stored as a fifth copy, never written back through the session setter. |

The widgets show **effective** while Read is on and no local gesture is active. Those
updates use `dontSendNotification` and the existing fader/pan guards. They must not call
`setTrackChannelFaderGain` / `setTrackStereoPan`, must not open a gesture, and must not
mark the project dirty.

### 4.2 Parameter identity

Stage 1 address:

- `TrackId`
- a stable parameter id: `ChannelVolume = 1`, `ChannelPan = 2` (numeric, stored in the file)

Not a display name and not a list index. The lane dropdown shows "Volume" and "Pan" as
labels only.

The stored object is a small address plus the curve, so a later stage can add a second
kind of address without a new container. That later kind is **not implemented now**:

- owner: the instrument host on that `TrackId`, or one insert identified by its existing
  stable slot id (not by chain index alone, and not by the plugin's display name)
- parameter: `AudioProcessorParameter::getParameterID()` once the wrapper actually reads it

Do not build a plugin-parameter table, a gesture bridge, or a proxy-fingerprint field in
this stage.

### 4.3 Time base

Points are **audible samples**, the same domain as clips, the playhead, and read-ahead
segment keys.

A tempo change does not move Volume or Pan relative to audio. It can move them relative
to MIDI notes. That is the split the project already has (audio in samples, MIDI in ticks).
Channel Volume and Pan sit on the audio strip, so they follow audio. Tick-domain points
would slide against every clip when BPM changes, which is the wrong default for a fader.

### 4.4 Curve model

A curve is an ordered list of points `(sample, value)` plus one **bend** per segment
(the span from point *i* to point *i+1*). Bend is a scalar `b` in `[-1, +1]`. `0` is
linear. Points are square. The circular handle is drawn only while the pointer is over
that segment or while that segment is being dragged.

Stored domains, so the file never contains NaN or infinity:

- Volume: linear gain in `[0, 8]`, same clamp as the fader. `0` is −∞ and is a real
  number. The lane readout converts through `ChannelFaderScale` for display only.
- Pan: `[-1, +1]`, same sanitizer as today.

Interpolation is in that stored domain. Do not interpolate volume in dB: −∞ is not a
finite dB, and a dB ramp would not match the linear multiply the strip already uses.

**How a drag sets the curve.** The handle is not a free point in time. It is the curve
at the time midpoint of the segment. A vertical drag anywhere on the segment sets that
midpoint. Let the endpoints be `(y0, y1)` and the normalized time be
`u = (t - t0) / (t1 - t0)`.

```
C = (y0 + y1) / 2 + b * (y1 - y0) / 2
B(u) = (1 - u)^2 * y0 + 2 * (1 - u) * u * C + u^2 * y1
```

`C` is always between `y0` and `y1` for `b` in `[-1, +1]`. `B(u)` is a quadratic Bezier
of `y0`, `C`, `y1`, so it stays inside `[min(y0, y1), max(y0, y1)]`. There is no
overshoot past the endpoints. `b = 0` puts `C` on the chord and `B` is the straight line.

The drawn handle is `B(0.5)`. A drag chooses a desired `B(0.5)`, clamped to the same
endpoint range, and solves

```
b = 4 * (B(0.5) - (y0 + y1) / 2) / (y1 - y0)
```

when `y0 != y1`, then clamps `b` to `[-1, +1]`. When `y0 == y1` the segment is constant
and the handle does not change the value. The handle's time stays at `u = 0.5`; the
mouse `x` does not move it along the timeline.

Point edits: click on empty lane adds a square point, drag moves it (time and value,
clamped to the domain and not past its neighbours' times), Delete or a right-click removes
it. Adding, moving, or deleting a point is its own undo step, separate from a Touch.

### 4.5 Before the first point, after the last point, and a new curve

Two different "what was the fader doing before this curve existed" problems have to stay
apart. Using the manual value for both of them conflicts with Touch (§4.12).

- **Anchor.** Each curve stores one scalar, `anchor`, in the same domain as the points.
  It is not a point: it has no sample, it is not drawn as a square, and it cannot be
  dragged. Evaluation when Read is on:
  - no points: the manual value (a lane with no curve does not change playback)
  - `t` strictly before the first point: `anchor`
  - `t` at or after the last point: hold the last point
  - otherwise: `B(u)` on the segment that contains `t`
- **When the anchor is set.** The moment a curve goes from empty to having its first
  point, `anchor` is the effective value **just before** that gesture. Later touches do
  not change it. Deleting the last point clears the curve and the anchor.
- **Why this does not rewrite earlier music.** The first recorded point is at the
  touch-down sample, never at sample 0. Everything before that sample keeps evaluating
  to the value that was already playing. The user changes the song start only by adding
  a point there.

After the last point, Read holds that point's value. There is no return to the anchor
after the last point.

### 4.6 Touch contract

Read and Write are **one pair per main track**, shared by Volume and Pan. Write is not
per lane.

1. **Read off, Write off.** The control is manual. Widgets show the manual value. The
   curve is left alone.
2. **Turning Read off** copies the last effective value into the manual field, once.
   Playback holds that manual value. The curve is not deleted.
3. **Read on, Write off.** The curve (with the anchor rule) drives. A pointer drag may
   take over for the duration of the gesture: the strip uses the gesture value. On
   release, the control returns to the curve's value at the current sample. Audio
   returns along the release ramp in §4.7. That takeover is **not** written.
4. **Turning Read on** (no gesture in progress): the widgets jump immediately to the
   curve at the current transport sample. Audio does not step. The strip ramps from the
   previous effective value to the curve value over the same 10 ms ramp. That ramp is
   **playback state**, like the pre-gain ramp. It is not written into the curve. The next
   time this spot plays with Read already on, it follows the curve without that ramp.
   The ramp exists so that enabling Read is not a click. It is not a claim that a later
   pass reproduces the click-avoidance of the mode switch.
5. **Write on, and transport playing, and the user is actually manipulating that
   parameter.** The gesture is recorded. The first recorded sample of a parameter creates
   the curve if needed, sets the anchor from the pre-touch effective value, and shows
   the lane. Both parameters can be touched in one gesture (fader and pan); each gets
   its own stroke and they share one undo step.
6. **On release.** Writing stops. If Read is on, the release ramp returns to the
   pre-gesture curve (or to the anchor / manual evaluation if the curve had no point
   covering the ramp end). If Read is off, the release value is stored as the new manual
   value and **no return ramp is written**. The stroke itself stays in the curve.
7. **Write on while playing, with no manipulation.** Nothing is written. Transport
   running is not a write.
8. **The whole gesture is recorded**, including holds, not only the first and last
   sample. See the resolution bound in §4.9.
9. **One undo step per finished gesture.** Audio or MIDI record-arm does not need to be
   on. The step restores the previous curves (and the manual value if this gesture
   changed it). Point edits on the lane are separate undo steps.

Write can be switched on while stopped. It stays lit and writes nothing until playback
**and** a real manipulation. Moving a control while stopped, even with Write on, updates
only the manual value when Read is off, or is a non-recorded takeover when Read is on
(same as rule 3). It does not create points.

### 4.7 How a write punch and the release ramp are stored

A Touch replaces only the time span of the gesture.

- Keep every point with `sample < touchStart` and every point with `sample >= rampEnd`.
- Delete points strictly inside `[touchStart, rampEnd)`.
- The segment that used to cross the punch does not keep its old bend. The new segment
  that enters the stroke has bend `0`. Bends on segments that lie wholly outside the
  punch stay.
- Append the recorded points (§4.9) and, when Read is on at release, two ramp points:
  - at the release sample: the release value, bend `0`
  - at `releaseSample + rampLength`: the value the **pre-gesture** curve evaluates to
    at that sample (anchor rule, points that fell inside the punch ignored)
- `rampLength` is **10 ms**, rounded up to the next whole device block so it lines up
  with the block ramp the strip already has to do. At 48 kHz that is 480 samples before
  the block rounding. The ramp is linear in the stored domain (`b = 0`), which is what
  was heard: the strip ramps linear gain and linear pan, not dB.
- If Read is off at release, the return point is not added. The last recorded point is
  the release value. Manual becomes that value.

Later playback with Read on therefore follows the same samples the gesture produced,
including the return, for the Write case. The Read-on mode-switch ramp (§4.6 rule 4) is
the exception and is intentionally not stored.

### 4.8 Stop, seek, cycle wrap, and Read/Write changes during a drag

- **Stop.** End the gesture at the last rendered sample. Apply §4.7 at that sample
  (return ramp only if Read is on). Further motion while stopped follows the stopped
  rule in §4.6 and does not extend the stroke.
- **Seek, or a cycle wrap.** End the stroke at the last sample **before** the jump.
  Do not draw a segment across the jump. If playback continues at the new position and
  the pointer is still down and Write is on, start a **new stroke** there (its own
  touch-down sample, same undo step). The return ramp of the first stroke is evaluated
  against the curve at the old position, not at the seek target.
- **Read or Write toggled during a drag.** End the gesture first, commit with the mode
  that was in force for the stroke, then apply the new mode. A Read-off mid-drag stores
  the current effective value as manual and does not write a return ramp. A Write-off
  mid-drag stops recording; if Read stays on, the return ramp is written and the rest of
  the pointer drag is a non-recorded takeover.

### 4.9 Timestamps, resolution, and non-drag edits

The UI timer does not decide when the sound changes.

- The message thread publishes the gesture value with a relaxed atomic store. No lock,
  no allocation, on the audio thread or in a strip job.
- Each audio callback reads that atomic once. If a gesture is active it ramps the strip
  from the previous block's applied value to this value across the block (same shape as
  the pre-gain ramp: per-sample linear in the stored domain). That ramp is the sound.
- The callback also writes `(blockStartSample, appliedValue)` into a preallocated
  single-producer ring for that track and parameter. The message thread drains the ring
  and, on gesture end, turns the drained points into the curve edit. A drain may be
  prompted by mouse-up or by a message-thread timer; the **timestamps and values** still
  come from the callback.
- Bound: one point per device block. The stored block-start value is the value the
  callback applied. That is the error bound. No second thinning in stage 1. A later
  thin, if one is added, may drop a point only when the reconstructed curve stays within
  `1e-4` linear gain or `1e-3` pan of this block-start sequence, and must say so in the
  change that adds it.
- Ring capacity: 32768 points per active parameter (about three minutes at a 256-sample
  block). If the ring would overflow, the callback sets an overflow flag and stops
  pushing. The message thread ends the gesture and commits what it has. The callback
  never waits and never allocates.

**Text commit and Ctrl/Cmd-reset** while playing with Write on are a one-block stroke:
one point at the block where the new value is applied, then the §4.7 release rule.
While stopped they only change the manual value (Read off) or do nothing to the curve
(Read on: the widget shows the typed value only until the edit ends, then snaps back;
the typed value is not kept, because a stopped Read-on edit is not a recording). Wheel
ticks during playback with Write on are the same as a drag: they publish a gesture value
and the callback records the blocks they cover.

### 4.10 What the widgets do

- While a local gesture is active, the widget shows the gesture, not the curve.
- Otherwise, if Read is on, refresh shows the effective value with
  `dontSendNotification` and the existing guards (`faderWiredGuard_`,
  `panField_.isMouseButtonDown()`, the mixer's fader guard). Playback refresh must not
  call the session setters.
- If Read is off, the widget shows the manual value, as today.

### 4.11 Persistence

Saved with the project (and therefore with autosave), which is a format bump when the
implementation lands. Suggested next version is 29. This plan does not bump it.

- curves, including anchors and bends
- which lanes are visible
- the Read flag, per track

**Write is not saved.** After open, Write is off. Old projects have no curves, Read off,
no lanes: playback matches today.

Hiding a lane does not remove its curve and does not stop playback.

### 4.12 Undo, duplicate, delete

Curves live in the session snapshot, keyed by `TrackId` + parameter id. They are not a
second document.

- Delete track: the snapshot that removes the track removes its curves.
- Duplicate: `withTrackDuplicated` deep-copies the source curves onto the **new**
  `TrackId`. The copy does not share storage with the source. Same undo step as the
  rest of duplicate.
- Undo/redo of a track add/delete/duplicate restores the snapshot, so the curves follow
  the track identity that snapshot already uses.
- A finished Touch is one undo step even if it wrote both Volume and Pan, and even if a
  cycle wrap split it into two strokes.
- Manual edits with Read off stay on the current non-undoable setters.

### 4.13 Publishing to the engine

- Curve data is an immutable snapshot published with `std::atomic<std::shared_ptr<...>>`,
  the same pattern as `SessionSnapshot`. The callback and a read-ahead worker copy the
  `shared_ptr` once per block or segment and then only read.
- No lock and no allocation in the callback or in a strip job to evaluate or to apply.
- The gesture atomic (§4.9) is separate and smaller: it is the live override, not a
  snapshot publish per mouse move.
- Do not publish a new `SessionSnapshot` per block and do not write the snapshot from
  the audio thread.
- Jobs receive the already-acquired snapshot (or the block's start/end gains computed
  on the callback before dispatch) and ramp inside the job. Group, Stereo Out, the
  monitor pass, and offline export call the same pure evaluator at the block's audible
  sample range. Offline does not use read-ahead; it evaluates the saved curve at the
  export timeline, which is the same function realtime uses when no gesture is active.

### 4.14 Arrangement UI

Automation lanes are **not** tracks. They are not `VisibleTrackEntry` rows, they have
no routing, host, Mute, Solo, Off, or number, and `rebuildVisibleTrackEntries` does not
list them. Main-track order is unchanged. They are not visual-group members and they
do not change group tab geometry or member counts.

**Chevron.** A small chevron at the bottom-left of the main track's colour segment
(the icon/number segment, which is already the full header height). It toggles lane
visibility for that track.

- Pointer: drawn faintly; full opacity on header hover.
- Keyboard: the chevron is a tab stop whenever the header is focused, including when
  the pointer is elsewhere. Enter or Space toggles it. Hover-only hit targets are not
  enough for keyboard use.
- Micro (28 px) has no second header row. The chevron still sits in the bottom of the
  colour segment and stays a tab stop. It does not add a header row and does not change
  the stored 28 px height.

**Read and Write.** Two letter cells, `R` and `W`, using the existing strip-cell chrome
(`TrackStripButtonGlyphs.h`).

- Read on: a green that is **not** `kPowerOnArgb` (`0xff2d9d53`).
- Write on: a red that is **not** `kArmOnArgb` (`0xffd01818`) and **not** `kSoloOnArgb`
  (`0xffd2402e`).
- Off: `kNeutralFaceArgb`.
- Placement: the right end of the second control row (Monitor, Arm, editor, …), so they
  do not sit on the Record Arm cell. The second row exists only when the header is at
  least `kMinimumHeightForSecondRowPx` (Small, 56 px, and taller).
- Micro and Mini have no second row. On those heights the same two cells are drawn at
  the right of the title row, always visible, still clear of the Arm cell (Arm is not
  on the title row). They must not be hover-only: an invisible Write is too easy to
  leave on.

**Lane.** Indented under its main track. Left side: parameter dropdown (only the
parameters that kind actually has) and a value readout in the same units as the fader
(dB via `ChannelFaderScale`) or the pan field. Right side: the curve, using the
arrangement's time-to-pixel mapping, horizontal zoom, and horizontal scroll.

Volume and Pan are two lanes when both are visible. Lane height is fixed (48 px), not
on the 14 px track-height grid — the same kind of exception as the 4 px collapsed-group
strips. Lane height is display-only. It does not change the stored per-track height.

Showing lanes adds their height to the scrollable content, so the vertical scrollbar
grows with them. Hiding removes that height. Playback does not care.

**Collapsed visual groups.** `rowHeightForVisibleEntry` already substitutes a 4 px strip
while a member is collapsed, and it does not rewrite the stored height. While that
substitution is in effect, that member's lanes are omitted from the layout. The
visible-lane set is unchanged. Expanding the group shows the same lanes again.

**Colours.** Curve and points use the track's existing colour. Handles, dropdowns, and
text use the current arrangement/header palette. No new theme.

### 4.15 Read-ahead

Saved automation is evaluated at the **segment's audible time** inside the worker, from
the published curve snapshot. Not at the UI playhead.

- Passive playback (Read on, no gesture): new segments see the current curve. Segments
  already queued before a curve edit stay stale for at most `depth` blocks. That is the
  existing §6 late window. This stage does not discard the queue on every point drag.
- Gesture begin (Touch, or a Read-on takeover): `audioThread_discardResetRow` for that
  track, the same transition monitor uses. If the worker is inside the plugin the row
  is Abandoning until `processBlock` returns. No concurrent `processBlock` on that
  instance. No promise that the handover completes in the same callback.
- While a gesture is active, that track is not offered for adoption. After release, the
  normal offer rules apply and new segments evaluate the curve that now includes the
  stroke and the return ramp.
- This exclusion is per gesture. It does not change the 1.3.1 startup policy, the depth
  cap, which rows are eligible when idle, or the thread model.
- Stage 1 does **not** need a blanket "never read-ahead a track that has automation".
  Only an in-progress gesture pulls the row back to the live callback.

### 4.16 Proxies, and the VST3 boundary

Volume and Pan run on the instrument's proxy audio through the same post-strip as live
instrument audio. They are outside `ProxyRenderSnapshot` and `ProxyFingerprint`. Changing
them must not schedule a proxy render.

A later **instrument-parameter** automation would change the rendered musical content, so
it would have to enter the proxy snapshot and the fingerprint. That is not this stage.
Do not add a fingerprint field "just in case".

When that later stage exists, the address in §4.2 is the extension point. The wrapper
will need an explicit parameter read (`getParameters` / `getParameterID`) and an explicit
gesture (`beginChangeGesture` / `setValueNotifyingHost` / `endChangeGesture`), which it
does not have now. Discrete values such as a Leslie speed switch would be **hold**
segments (bend forced to 0), not the quadratic used for Volume and Pan. Leslie speed and
Presence are not identified here.

---

## 5. Conflicts and the choice this plan makes

**"Before the first point, hold the manual value" vs "Read off leaves the release value
as manual".** If both were the manual field, a Write with Read off would change what
Read later plays *before* the first point, including music from before the touch.
**Choice:** the anchor (§4.5) is a separate scalar, set once from the pre-touch
effective value. Manual remains the Read-off value. Before the first point, Read plays
the anchor.

**"On release, return to the existing curve" vs "after the last point, hold".** These
agree because the return is real points inside the punch (§4.7). Hold applies only past
the last of those points.

**"Turning Read on must not click" vs "later playback matches what was heard".** The
return ramp of a Write is stored, so later playback matches the Write. The ramp used
only to soften enabling Read is playback state and is not stored (§4.6 rule 4). A later
pass with Read already on follows the curve directly.

**Read-ahead §6 late fader vs "the write must be heard" vs "evaluate the samples that
are actually rendered".** Steady state evaluates the curve at segment time, so queued
*future* segments match the saved curve. A gesture discards the row (existing handover,
no overlap, no same-block promise). A point edit that is not a gesture keeps the §6
window rather than resetting every plugin on every point drag.

**"One point per UI event" vs "the timer must not define audio timing".** The callback
records one point per block at the sample it actually rendered. The message thread only
commits that ring.

---

## 6. Implementation order

The stage is delivered when slices 1–3 are all in. Slice 1 alone is not a release.
Slice 4 is part of the same stage because read-ahead is already in the product (on at
depth 3 unless saved Off or `--no-readahead`); it does not wait for a later version.

**Slice 1 — model, evaluator, playback, export.** Address, anchor, points, bend, pure
evaluator, snapshot publish, project round-trip behind the new format version, block
ramp in the audio-clip strip, the instrument/proxy strip, the monitor strip, Group, and
Stereo Out, and the same call in the offline loop. No arrangement UI yet.

Done when: a fixture curve produces the same samples live and offline; values outside
the domain cannot be stored; a project without the new keys loads as today; the callback
and the strip jobs do not allocate to evaluate.

**Slice 2 — Touch, Read/Write, Inspector, mixer.** Gesture begin/end on the fader and
the pan control, shared by both panels. The contract in §4.6–§4.10. One undo step.
Display updates do not feed back into setters.

Done when: the nine Touch rules hold, including Write-while-stopped and a punch that
leaves points outside the gesture in place; Inspector and mixer cannot recurse; a
finished drag is a single undo.

**Slice 3 — lanes.** Chevron, R/W cells, dropdown, readout, points, bend handle,
Micro/Mini placement, scrollbar content height, collapsed-group hide/restore. Duplicate
and delete carry the curves. Lane visibility and Read persist; Write does not.

Done when: Volume and Pan can be shown together; hiding a lane does not change playback;
collapsing a group hides lanes and expanding restores them; a duplicated track's curve
diverges from the source after an edit; MIDI rows have no lanes.

**Slice 4 — read-ahead.** Worker evaluates at segment time. Gesture begin discards and
blocks adoption. Proxy fingerprint unchanged by Volume/Pan automation.

Done when: a segment rendered ahead matches the curve at that segment's sample, not at
the playhead; a gesture never overlaps `processBlock` on the same instance; a fader
move does not change the proxy fingerprint.

---

## 7. Verification

No full matrix. No time budget. Focused checks only.

**Cloud / Linux (logic and determinism, stub device):**

- Evaluator: anchor, hold after the last point, bend stays inside the endpoints, domain
  clamps, 10 ms ramp points, punch keeps outside points, seek/cycle splits the stroke.
- Read/Write rules that do not need a physical mouse (programmatic gestures): Write
  alone writes nothing; stopped Write writes nothing; one undo restores the surrounding
  curve.
- Save, reload, old-project load. Duplicate independence. Delete drops curves.
- Realtime vs offline on a deterministic fixture, same samples.
- Read-ahead pump: segment-time evaluation, discard on gesture, no second `processBlock`
  while the worker is inside the plugin. Proxy fingerprint stable across a fader change.

**Windows / ASIO and a real pointer, on the user's machine:**

- Fader and pan from Inspector and mixer while playing, including text and Ctrl-reset.
- No zipper on a slow move; the Read-on ramp is a short fade, not a click.
- Lane hit targets: add, move, delete, bend; chevron and R/W on Micro, Mini, and Small;
  collapsed group; vertical scrollbar.
- A gesture against a read-ahead row that is inside a plugin: the row goes quiet until
  the worker returns, then follows the live value. Not a same-block guarantee.
- One listening pass and one offline export of the same short fixture.

Cloud success does not cover the listening pass, ASIO, or the pointer.

---

## 8. Decisions already made in this plan

These are recommendations, not open questions. They are recorded so implementation does
not reopen them silently.

| Topic | Choice |
|-------|--------|
| Write mode | Touch only |
| Read/Write scope | Per main track, shared by its lanes |
| Write after open | Always off |
| Time base | Audible samples |
| Volume file domain | Linear gain `[0, 8]`, `0` = −∞ |
| Pan file domain | `[-1, +1]`, existing linear balance law applied at the strip |
| Bend | One scalar in `[-1, +1]` per segment; handle is `B(0.5)` |
| Before the first point | Anchor, set once from the pre-touch value |
| After the last point | Hold, while Read is on |
| Release ramp | 10 ms, block-rounded, linear in the stored domain, written only when Read is on at release |
| Enabling Read | Display jumps; audio ramps 10 ms; that ramp is not stored |
| Recording resolution | One point per device block, timestamped by the callback |
| Manual fader edits with Read off | Stay non-undoable |
| MIDI | No Volume or Pan lanes |
| Stereo Out | Volume and Pan, because the strip already has both |
| Read-ahead | Evaluate at segment time; discard only on gesture; 1.3.1 startup policy and cap unchanged |
| Proxies | Volume/Pan do not enter the fingerprint |
| Format | Bump when the feature is implemented, not in this planning change |
