# Plan: full Off unloading and one shared out-of-process host for AmpliTube 4 (2026-11-05)

Status: **plan only for the isolation.** Stage 0 (Power guards, rest revision 2) is implemented
on the same branch; everything from stage 1 on is not. Base: branch `claude/resource-saving`
(draft PR #12), which is `main` `8472feb` plus the proxy opt-in, Off lifecycle, automatic rest,
report and stage 0 commits. Result report: `docs/RESOURCE_SAVING_2026-11-05.md` ("the report").

Revision 2 of this plan (after review) corrects: state capture without recurring silence (C.2,
C.3), what the per-slot stand-in needs outside the instance factory (B.3), buffer ownership and
one shared callback budget (D.2, D.3), offline hang handling (D.5), and the prototype's
acceptance (E). "No added block latency" is a goal the prototype must verify, not a property.

Each statement is marked by where it comes from:

* **[code]** read in this repository at `4ad4784`, or in JUCE 8.0.4 as built here
  (`build/ninja-release/_deps/juce-src`).
* **[measured]** run on this machine (Core Ultra 9 275HX, Windows 11, RME Babyface Pro ASIO,
  48 kHz / 512).
* **[to prove]** an assumption the prototype or a test must confirm before anything depends on it.

**Short answer.**

* Off already unloads and frees the row's plug-ins (report §3), and since stage 0 nothing is
  created or destroyed because of Off while the transport runs. What is missing for "full" Off:
  decoded audio of Off-only rows and a crash-safe way to destroy AmpliTube 4.
* AmpliTube 4 moves to **one helper process per DAL session** that owns every AmpliTube 4
  instance. DAL keeps one small stand-in per insert slot that looks to the rest of DAL like an
  ordinary `juce::AudioPluginInstance`. Commands and state go over JUCE's existing child-process
  IPC. Audio goes through shared memory, processed **in the same block**. The goal is no added
  block latency, so AmpliTube rows do not move against other rows; the prototype verifies it.
* Destroying AmpliTube happens only in the helper. If the helper dies, DAL lives on, shows
  "Restoring plugins...", starts one new helper and reloads only the AmpliTube instances that
  should be On, from state captured before the destructive step. Project close and app exit end
  the helper without running AmpliTube's destructors at all.
* Before the full build: a **prototype** (stage 1) that measures round-trip cost, callback peaks,
  parallelism and memory with real AmpliTube instances. It has stop criteria. If it fails them,
  the fallback is the "keep loaded but released" option from the report §3.4.

---

## A. Verified current state and the code it touches

### A.1 Off lifecycle (PR #12) [code]

| Concern | Where | State at `4ad4784` |
|---|---|---|
| Off flag | `Track::trackOff_` (`src/domain/Track.h:482`), `Session::setTrackOff` (`src/domain/Session.cpp:1729`) | Part of the session snapshot, so undo/redo and Save carry it. |
| Runtime follows the flag | `TrackPowerCoordinator` (`src/app/TrackPowerCoordinator.cpp`) | Message-thread timer: 250 ms idle, 20 ms while unloading, 1 ms while waking. Unloads one row per turn and wakes one plug-in per turn. `requestPowerOn` and the final flip of a wake job check `transportBusy`. |
| Insert unload / wake | `PluginInsertHost::makeTrackInsertsDormant` (`:1927`), `wakeNextDormantInsert` (`:2074`), `destroyNextOffRetiredInsert` (`:2007`) | Unpublish, then under acknowledged exclusive access (callback drained, read-ahead paused and acknowledged) capture and release. Destroy one instance per later timer tick. |
| Instance creation | `PluginInsertHost::instantiateLiveSlot` (`:1722`, `formatManager_.createPluginInstance` at `:1754`) and the add path (`:1386`) | The only two places that create insert instances. **This is where an isolated stand-in can be swapped in.** |
| Slot shape | `LiveInsertSlot` (`src/plugins/PluginInsertHost.h:544`) | Owns `std::unique_ptr<juce::AudioPluginInstance>`. Dormant placeholder = `instance == nullptr` + `unavailableDescriptor` + `dormant`. |
| Per-block processing | `PluginInsertHost::audioThread_processEntryChain` (`:2949`, `processBlock` at `:3039`) | Called from the callback, render-pool jobs and read-ahead. A row is processed by one lane per block. |
| Instrument | `InstrumentTrackController` retained Primary row; `ExperimentalInstrumentHost::audioThread_discardPendingMidi` | An Off host holds no plug-in. MIDI addressed to it is discarded each block (`PlaybackEngine.cpp:2109`, `:3778`). |
| MIDI editor | `MidiEditorPresenter::auditionHostForTrack` (`src/app/MidiEditorPresenter.cpp:174`) | Opens without a loaded instrument. Audition goes to the host, which discards it while Off, so audition is silent. Checked in the real editor window without an audio device (`MidiEditorCohortFocusedTests`, "off row") [measured]; not tried in the running app. |
| Project load | `importChain` with `setTrackDormancyQuery`, `deferPendingAutoloadWhileOff` | Rows saved Off instantiate nothing. Tested in `InsertPersistenceFocusedTests` [measured]. |

### A.2 Is "Power only while stopped" enforced? [code]

| Path | Guard |
|---|---|
| Audio / group header Power (`TrackLanesView.cpp:1909`) | `isStructuralTimelineEditBlocked()`: recording, count-in, `PlaybackIntent::Playing`, project load in progress (`MainAppWindow.cpp:1796`). |
| Instrument row Power (`InstrumentTimelineRowCoordinator.cpp:2401`) | Same predicate. |
| Mixer Power (`MainAppWindow.cpp:6970`) | Same predicate. |
| `TrackPowerCoordinator::requestPowerOn` | `transportBusy` (same predicate, `MainAppWindow.cpp:2398`). |
| Wake job's final flip | Waits for `transportBusy` to clear. **Plug-ins are still loaded while playing**; only the flip waits. |
| **Undo / redo** (`UndoRedoCoordinator::invokeUndoFromWindowShortcut`, `:67`) | Refused only while recording or counting in, **not while playing**. Because Off is part of the snapshot, undoing an edit made before a Power change flips Off during playback. `reconcileNow` then **unloads that row while playing** (no transport check on the unload branch), or starts a wake job that loads plug-ins while playing. |
| Stability scenario `switchSourceTracksOff` (`MainAppWindow.cpp:7660`) | Diagnostic only; no guard. |

**Implemented in stage 0** [code, tested]:

1. `TrackPowerCoordinator::reconcileNow` does nothing while `transportBusy`; the snapshot is
   reconciled after Stop. Deferred destruction of released Off instances also waits for Stop.
2. Every wake step checks `transportBusy` first. A stepwise On load that is between two
   plug-ins when the transport starts pauses there and resumes after Stop; the row stays Off
   until everything is loaded. While busy the coordinator ticks at its idle interval.
3. Undo / redo looks at the step it would restore (`SessionHistory::peekUndoTimelineSnapshot`)
   and refuses it while playing when any row present in both snapshots would change Off
   (`SessionSnapshot::anyTrackOffDiffers`). The history pointer and the model are not touched; a
   toast says "Undo would switch a track On or Off. Stop the transport first." Undo / redo steps
   that leave Power alone work as before during playback.

Every path that can destroy or create a plug-in because of Off now runs only while stopped.
Deleting an insert or a track is already behind the same predicate. One plug-in load that is
already running when the transport starts finishes (JUCE's load can run a nested message loop);
the next one waits.

### A.3 Automatic rest: what is safe and what is missing [code]

**Correction.** The first rule (commit 2804928) let any unit rest after "never had input + latency
+ 2 silent blocks", instruments included. That proves nothing about future output: a
transport-driven or free-running generator can stay silent for any number of blocks and start
later without input, MIDI or a parameter change. Waking on transport, parameter and state
changes does not fix that case.

**Rest revision 2, implemented in stage 0** (`src/plugins/InsertChainRest.h`, report §4.2):

* **Who may rest:** only units whose active plug-ins all declare themselves audio-processing
  effects (`categoryAllowsRest`: "Fx" plus EQ, Dynamics, Reverb, Delay, Distortion, Filter,
  Modulation, Pitch Shift, Restoration, Mastering, Spatial or Analyzer; never Instrument, Synth,
  Sampler, Drum, Piano, Generator, External, Tools, Network, OnlyARA or a plain "Fx"). This rests
  on the plug-in's declaration, which DAL cannot verify. Every instrument keeps running.
* **Rules for those units** (unchanged): never had input + latency + 2 silent blocks, or a finite
  reported tail with 2 × tail + latency + 1 block of silence; infinite and zero-reported tails
  keep running after input.
* **Wake** (new): a parameter change from any thread (`AudioProcessorListener` on every live
  insert, owned by the row's rest runtime for the host's lifetime), a state or preset load into
  the same instance, transport start or stop, and a jump in play position (locate, cycle wrap).

In this project that covers Pro-Q 3, Pro-R, AmpliTube 4 and Pro-C 2. Excluded now: HALion Sonic,
Groove Agent SE, VB3-II and BBC SO (instruments), DAL Mono Delay, DAL Tuner and BassEvening
(plain "Fx"), VB3-II Remote Input ("Fx|Tools"). The CPU gain measured for revision 1 (report §5)
included 114 237 rested instrument blocks; that part is gone. It was not re-measured (needs the
ASIO device).

Remaining work for heavy chains after Stop, with no arbitrary cutting of long echoes:

* **Zero-reported tails (AmpliTube).** VST3 `kNoTail` (0) is the default and does not reliably
  mean "no tail". JUCE's wrapper does not expose the plug-in's output silence flags:
  `getTailLengthSeconds` reads `getTailSamples` (`juce_VST3PluginFormat.cpp:3199`), and input
  silence flags are cleared [code]. **No rule is proposed.** It keeps running.
* **Infinite tails (Pro-R).** Nothing to do safely. Rest only when the user switches the row Off.
* **Instruments.** No rest. A rule would need the plug-in to report its voice state, which VST3
  does not standardise.
* **Plain "Fx" plug-ins** such as DAL's own Mono Delay and Tuner: they would qualify if they
  declared a processing sub-category. That is a change in those plug-ins' repositories, not done
  here.
* **Where the remaining cost runs.** The stopped callback still runs the non-resting stages
  serially. Dispatching them through the render pool while stopped is a separate optimisation.

### A.4 Test failures compared with the exact base [measured]

The report compared `AudioStripParallelFocusedTests` and `ReadAheadPrototypeFocusedTests` with an
**older `main` binary**. For this plan both suites were rebuilt at the PR's exact base `8472feb`,
in a separate worktree, with the same toolchain, generator, configuration, JUCE source and ASIO
SDK as the PR build (`build/ninja-release-base`). Each binary was then run twice, base and PR
alternating, on the same machine in the same session:

| Suite | Base `8472feb`, run 1 / run 2 | PR `4ad4784`, run 1 / run 2 | Failing checks |
|---|---|---|---|
| `AudioStripParallelFocusedTests` | 85 checks, 6 failures / 6 | 85 checks, 6 failures / 6 | identical names and order in all four runs |
| `ReadAheadPrototypeFocusedTests` | 217 checks, 39 failures / 39 | 217 checks, 39 failures / 39 | identical names and order in all four runs |

So the failures are **not caused by PR #12**: they come with the exact base under identical build and
run conditions. The failing checks are pool-dispatch and sample-coverage assertions ("parallel:
blocks were dispatched to the pool", "insert chain covered every sample exactly once", read-ahead
adoption and discard counts). Why they fail at base is a separate question this plan does not
answer. Lists: `docs/evidence/resource-saving-2026-11-05/basecmp-*-failures.txt`.

After stage 0 (two runs): `ReadAheadPrototypeFocusedTests` has the same 39 failures. In
`AudioStripParallelFocusedTests` the base's two pool-dispatch failures passed (the pool got 1 and 4
of 49 blocks instead of 0) and the other 4 remain; nothing new fails. The pool check depends on a
measured cost threshold; the cause of the change is not established (report §9).

### A.5 Message thread: observations and conclusions

Observations [measured, `docs/evidence/resource-saving-2026-11-05/message-thread-sample-after.txt`,
user-mode stack sampler, about 10 s, stopped]:

* The process's message thread used about 9 % user and 11 % kernel time of one core
  (`GetThreadTimes`).
* 88.7 % of user-mode samples are inside `NtUserGetMessage`, the thread waiting for messages.
  Frames from `bdhkm64.dll` (a Bitdefender hook) sit **above** `GetMessageW` on 90 % of samples.
* `juce::HWNDComponentPeer::onVBlank` → `D2DRenderContext::handleDirect2DPaint` → `startFrame`:
  about 5 % of samples. DXGI and D3D11 frames: about 1–2 %.
* `juce::Label::valueChanged`: 2.9 %. Timer callbacks: 1.7 %. Groove Agent SE and HALion Sonic
  frames: about 0.6–0.8 % each. `EditControllerParameterDispatcher::flush` (JUCE's VST3 host
  timer): 0.6 %. `atcuf64.dll` (Bitdefender): about 0.5 %.

Conclusions and how strong they are:

* **Supported:** most user-mode samples are idle waits. Painting is triggered from vblank only
  when something is dirty; a label that changes (`Label::valueChanged`, probably the scenario
  runner's own status label during the perf profile) causes repaints. That label is part of the
  measurement, not of normal use.
* **Not supported by this data:** that the hook costs CPU. Its frames sit on the waiting stack, so
  inclusive time there is mostly waiting. That the 11 % kernel time is "the D2D present path and
  the hook": the sampler saw no kernel stacks. The report §6 wording is therefore stronger than
  the evidence. Stage 0 corrects it.
* **To prove:** an ETW capture (WPR CPU profile with kernel stacks, viewed in WPA) of the stopped
  app with and without the main window minimised, and once without the perf profile's status
  label. Whether the security product costs anything can only be checked on a machine where its
  exclusion is configured by the user. This plan does not change security settings.

### A.6 Decoded audio [code]

`AudioClip` (`src/domain/AudioClip.h:27`) owns its decoded PCM by value (`juce::AudioBuffer<float>
samples_`). Placed clips share it through `std::shared_ptr<const AudioClip>`, and every session
snapshot holds that pointer, including undo history. `AudioWaveformCache` builds a min/max pyramid
per `AudioClip*` (`src/io/AudioWaveformCache.h`). Nothing can drop PCM today without dropping the
clip, so releasing Off-only audio needs an indirection (stage 6).

---

## B. Recommended process and thread model

### B.1 Scope of isolation

* **Exactly AmpliTube 4**, identified by **all** of: VST3 component class id, vendor
  "IK Multimedia", plug-in name "AmpliTube 4", and the bundle path from the scanned
  `juce::PluginDescription`. The class id is stored in the slot (`LiveInsertSlot::vst3ClassId`)
  and in the project descriptor. The name alone never qualifies.
* The policy is one small table, `IsolatedPluginPolicy` (identity → "run in shared helper"),
  consulted only by the instance factory. Nothing else in the engine knows about AmpliTube.
* AmpliTube 4 is used only as an insert in this project, and DAL sends inserts no MIDI (report
  §4.1) [code]. **The audio path carries no MIDI.** An instrument in the helper is out of scope.
* Every other plug-in stays in DAL's process, with today's parallel processing.

### B.2 Process

* **One helper process per DAL process**, started on demand when the first isolated instance is
  needed and ended when the last one is gone. It is DAL's own executable started with a worker
  flag (`--minidaw-isolated-insert-host`). That is the same pattern as the existing VST3 scan
  worker (`kVst3OopRawScanWorkerArg`, `src/plugins/Vst3ChildProcessScan.h`) [code].
* The helper opens **no audio device**. It is not a competing audio instance. It never reads or
  writes user settings or projects. It receives sample rate, block size and transport from DAL.
* The helper runs a JUCE GUI message loop (`ScopedJuceInitialiser_GUI`). JUCE's VST3 wrapper
  asserts the message thread for `getStateInformation` / `setStateInformation`
  (`juce_VST3PluginFormat.cpp:3286`, `:3306`) and for editor creation [code]. So the helper's
  message thread is where instances are created, destroyed, state-accessed and edited.

### B.3 The boundary inside DAL

```
PluginInsertHost ── LiveInsertSlot.instance ──► juce::AudioPluginInstance
                                                   ├─ in-process VST3 (today)
                                                   └─ IsolatedInsertInstance  (new, ~1 class)
                                                          │ commands (message thread)
                                                          │ audio (processing lane)
                                                          ▼
                                                   IsolatedHostConnection (one per helper)
                                                          │ JUCE ChildProcessCoordinator (pipe)
                                                          │ shared-memory blocks + events
                                                          ▼
                                                   helper: IsolatedHostWorker
                                                          one owned VST3 instance per slot
                                                          one processing thread per instance
```

* `IsolatedInsertInstance : juce::AudioPluginInstance` stands in for the instance in
  `LiveInsertSlot`. It implements what DAL calls today [code]: `prepareToPlay` /
  `releaseResources`, `processBlock`, bus layout (stereo in and out), `getLatencySamples`,
  `getTailLengthSeconds`, `get/setStateInformation`, `getParameters` (a mirrored list for the
  parameter window and the first-parameter test hook), `createEditor` / `hasEditor`, and
  `getExtensions` (the VST3 preset client: `getPreset` / `setPreset`, used at
  `PluginInsertHost.cpp:674–972`).
* `PluginInsertHost`, `PlaybackEngine`, the render pool, read-ahead, rest, Off, undo and
  persistence keep handling a `juce::AudioPluginInstance`. The only engine-facing difference is
  how long `processBlock` may take and that it can fail (section D).
* The saved project does not change. A slot stores the same descriptor and the same VST3 state
  bytes as in-process, so a project moves freely between builds with and without isolation.

**What `processBlock` alone does not carry, and the changes outside the factory:**

| Needed by the stand-in | How it gets there | Change outside the factory |
|---|---|---|
| Segment transport (position, playing, tempo, cycle) | Its own `getPlayHead()`, exactly like an in-process plug-in: DAL sets the chain's `InsertProcessPlayHead` per segment before processing. The stand-in copies `PositionInfo` into the request header. | None. |
| The callback deadline | Not in `processBlock`. The engine publishes one absolute deadline per callback (D.3) in a small `IsolatedProcessDeadline` (an atomic QPC value per processing context: callback, read-ahead, offline). The stand-in reads it at the start of its wait. | `PlaybackEngine`: write the deadline at callback start and when read-ahead / offline start a segment. About ten lines; no new lock. |
| Which lane calls it (for counters) | Not needed for correctness. | None. |
| Batching adjacent isolated slots in one chain | Not possible from inside one `processBlock`. Concrete mechanism: in `PluginInsertHost::audioThread_processEntryChain`, the slot loop detects a run of consecutive slots whose processors are isolated on the same helper and calls `IsolatedHostConnection::processRun(slots, view)` once instead of `processBlock` per slot. | One branch in that slot loop, plus a "same helper" query on the slot. **Optional**, measured in stage 1 before it is built. |
| Batching several segments of one callback | Not offered. Segments are processed one at a time by the engine with other rows' work between them; merging them would change the engine's segment loop. Each segment is one request. | None. |


### B.4 Two channels

| Channel | Carries | Mechanism | Thread in DAL | Thread in helper |
|---|---|---|---|---|
| Control | create, prepare, release, destroy, state get/set, preset get/set, editor open/close, parameter list, latency/tail changes, liveness | `juce::ChildProcessCoordinator` / `ChildProcessWorker` over a named pipe [code: `juce_ConnectedChildProcess.cpp`, `juce_Files_windows.cpp:1095` overlapped pipe] | Message thread, never the audio thread | Message thread |
| Audio | per-block input and output samples, transport, sequence numbers | One page-file-backed shared section (`CreateFileMapping(INVALID_HANDLE_VALUE, …)`) per helper, plus one auto-reset event pair per instance | Whichever lane processes the row | That instance's processing thread |

JUCE facts that force this split [code]:

* `InterprocessConnection::sendMessage` and `readNextMessage` allocate a `MemoryBlock` for every
  message (`juce_InterprocessConnection.cpp:212`, `:353`), and delivery goes through the message
  queue. That is not usable from an audio lane.
* `juce::MemoryMappedFile` maps files only. A page-file-backed section needs Win32
  `CreateFileMapping` in a small Windows-only source file (DAL is Windows-only for this build).
* JUCE's child-process ping counts whole seconds (`countdown = timeoutMs / 1000 + 1`,
  `juce_ConnectedChildProcess.cpp:68`). It only fits as the control-channel liveness check, not
  for hang detection on the audio path.

### B.5 Helper threads

* One processing thread per live isolated instance. It sleeps on its "request" event, processes
  the request and signals "done". Thread priority: registered with MMCSS "Pro Audio"
  (`AvSetMmThreadCharacteristics`), like an audio thread [to prove: helps peaks].
* Instances are independent. Two AmpliTube rows processed by two DAL lanes in the same block run
  in parallel in the helper. No helper thread runs unless a DAL lane is waiting for it, so the
  helper adds no concurrent load beyond what DAL lanes already represent (section D.4).

---

## C. Ownership and state transitions

### C.1 Who owns what

| Thing | Owner | Survives a helper crash? |
|---|---|---|
| Desired Off/On per row | Session snapshot (`Track::trackOff_`) | Yes. It is the source of truth for recovery. |
| Slot identity, order, Pre/Post stage, Active/Bypass | `PluginInsertHost` slot (unchanged) | Yes. |
| Last good state of each isolated instance | `IsolatedInsertInstance::lastCapturedState` (bytes plus capture time), in DAL | Yes. |
| The live AmpliTube object | Helper | No. |
| Mapping slot ↔ helper instance id ↔ shared-memory block | `IsolatedHostConnection` | Rebuilt by recovery. |

"Desired" (snapshot) and "present" (live helper instances) are tracked separately.
`TrackPowerCoordinator` already reconciles desired against present for Off. Recovery uses the
same comparison: present becomes empty when the helper dies, and only desired-On isolated slots
are rebuilt.

### C.2 When state is captured

Two kinds of capture, so that capturing never makes audio stop during playback:

**Non-exclusive capture** (no pause, audio keeps running). `getStateInformation` on the helper's
message thread while the instance keeps processing. That is what DAL does in-process today for
Save and autosave, and what VST3 allows: `IComponent::getState` is called on the UI thread while
`process` runs on the audio thread. It gives a fresh state but not a guaranteed consistent one if
the plug-in breaks that rule. Used for:

| Trigger | While stopped | While playing |
|---|---|---|
| Save, Save As, autosave | at once | at once (as today in-process; no pause) |
| Editor closed | at once | deferred to Stop |
| Parameter activity settled (helper reports "dirty", DAL captures when activity has stopped for a short debounce) | at once | deferred to Stop |
| Plug-in undo recorder (`onPluginUndoRecord`) | at once | at once (as today) |
| Transport stops | every dirty instance | — |

Deferring to Stop keeps a long getState of a large plug-in off the helper's message thread while
it may also be needed for the editor during playback. The debounce bounds how often a capture
runs, not whether a state is correct.

**Exclusive capture** (C.3): only before a destructive helper operation (Off, delete insert or
track, recovery of others), for **every** isolated instance in the helper, because a crash takes
them all. These operations happen only while stopped (A.2), so the brief pause of isolated stages
does not silence playback; at most a tail after Stop is cut short.

**What can still be lost after an unexpected helper crash:** parameter changes made since the last
successful capture of that instance. That is at most:

* while stopped: the changes in the debounce window before the crash, or in an open editor since
  it was opened or last settled;
* while playing: every change made during the current playback, since dirty captures are deferred
  to Stop (Save and autosave during playback still capture).

The recovery message names the instances that were dirty when the helper died.

### C.3 Exclusive access in the helper

Only for the destructive operations of C.2, so only while stopped:

1. DAL marks the isolated slots of the helper "paused". Their lanes output silence for those
   stages and send no request. This is a published flag read per block, like dormancy.
2. DAL waits for its own drain (callback exit, render-pool idle for those rows, read-ahead paused
   and acknowledged). These are the existing mechanisms (`waitForAudioCallbackExit`,
   `pauseReadAheadWorkerAfterChainPublish`).
3. The helper confirms per instance that no request is outstanding (D.3) and that its processing
   thread is parked. **No acknowledgement means no capture and no destructive step.** The
   operation is cancelled and the instances are resumed unchanged (same rule as
   `DormancyResult::NotDrained`).
4. Capture all, then continue with the destructive step, then resume the others.

### C.4 Transitions

**On → Off of a row with isolated inserts (transport stopped).**

1. The row is Off in the snapshot. The engine stops processing it from the next block
   (unchanged).
2. Pause all isolated stages and get the exclusive access of C.3.
3. Capture state of **all** isolated instances. If any capture fails, stop. Nothing is destroyed,
   the stages resume, the row stays Off, and its runtime stays loaded but not processed. A
   retryable message names the instance. No state is lost.
4. The row's slots become dormant placeholders holding the just-captured bytes (unchanged format).
5. Send "destroy instance X" to the helper. Resume the other stages.
6. The helper replies or dies. If it replies, done. If it dies, go to recovery (C.5). The Off
   result is already safe, because the Off row's state was taken in step 3.
7. If the helper now owns no instances, end it (C.6), which frees its memory.

**Off → On.** Same as today's wake job, except instantiation is a control request to the helper
(started first if not running). One instance per message-loop turn, with progress. A failed load
leaves the slot as today's unavailable placeholder with its state kept.

**Delete insert or track.** Same as On → Off steps 2–7, but the slot is removed. The bytes go into
the undo step as today.

**Project close, project switch, app exit.** State is captured (it has been saved, or the user
declined to save). DAL tells the helper to exit **without destroying plug-ins**: it terminates its
own process, or DAL calls `TerminateProcess` after a bounded wait. AmpliTube's destructor then
never runs. That removes the shutdown and project-switch crash the report §3.4 traces back to
2026-09-26. The OS reclaims the memory.

### C.5 Recovery after the helper dies

Detection: the helper's process handle becomes signalled, or the pipe reports
`handleConnectionLost`. Audio lanes also see the process handle in their wait (section D.3).

1. Every isolated stage switches to "lost": it outputs silence and sends no request.
2. DAL shows **"Restoring plugins..."** with per-instance progress, in the same toast as the wake
   job. The UI stays responsive: one instance per message-loop turn.
3. Blocked while restoring: Play, Record, count-in, Power, insert add/remove/move, track delete
   and duplicate, project open, close and switch, and undo/redo. A `PluginRecoveryCoordinator` in
   `isStructuralTimelineEditBlocked()` and in the transport-start guard does this.
4. If no desired-On isolated instance exists, the helper is **not** restarted.
5. Otherwise start **one** new helper. For each desired-On isolated slot, in slot order: create
   the instance, `setStateInformation(lastCapturedState)`, prepare with the current device
   settings, re-attach to the same slot id (Active/Bypass and Pre/Post come from the slot, which
   never left DAL). Desired-Off slots stay dormant placeholders and are not loaded.
6. Success: lift the block, report "AmpliTube 4 host restarted; N instances restored".
7. Failure (the new helper dies or an instance fails to load): **stop. No second automatic
   restart.** Affected slots stay "lost" placeholders with their state (saved unchanged). A banner
   offers **Retry restore**, and the next attempt starts only from that button. Play is allowed
   again, with those stages silent and marked. Save writes the last captured state, so nothing is
   lost by saving.

What can be lost: parameter changes made after the last capture (C.2). This is stated in the
recovery message when the helper died while an editor was open or the instance was dirty.

### C.6 Helper lifetime

```
 none ──first isolated instance needed──► starting ──handshake──► running
   ▲                                                                │ │
   └──── last instance gone / project close / app exit ◄────────────┘ │
                                                                       │ crash / hang
 failed (Retry) ◄── restore failed ── restoring ◄──────────────────────┘
```

At most one helper exists. "restoring" is entered only from a crash or hang, or from the Retry
button.

---

## D. Audio, latency and real-time contract

### D.1 Same-block processing: no added latency is the goal

The lane that processes a row, at the isolated slot's position in the chain:

1. writes the stage input (2 channels × n samples, float) into the slot's shared input buffer,
   plus a small header: sequence number, n, `PositionInfo` of the segment, sample-rate generation;
2. publishes the sequence number and signals the slot's request event;
3. waits for the slot's done sequence, bounded by the shared callback deadline (D.3);
4. reads the output buffer into the chain's buffer and continues with the next slot.

If the result arrives in time, it is used in the same block and DAL adds **no samples of delay**.
AmpliTube's own reported latency (7 samples [measured, report §4.1]) is unchanged: DAL has no
project PDC and does not compensate it today, in-process or isolated. AmpliTube rows then do not
move against other rows. **This is the design goal; the prototype verifies it** with an impulse
through the helper and in-process, which must line up to the sample (E).

Rejected alternative: **pipelined processing** (send block k, return block k−1). It removes the
wait but delays AmpliTube rows by one block (512 samples = 10.7 ms here) against all other rows.
Without project PDC that is an audible shift. Not accepted.

### D.2 Cost for the callback

* Per isolated slot per processed segment: two buffer copies (2 × n × 4 B each way), two event
  operations, and two cross-process thread wakes. The wakes dominate; their distribution on
  Windows depends on power state and scheduling **[to prove; this is the prototype's main
  number]**.
* Mitigation: the lane spins on the shared done sequence for a short, measured period before it
  sleeps on the event; the helper thread does the same on "request". **Spinning costs CPU on
  both sides**; the prototype measures CPU with and without it and the spin period is chosen from
  those numbers. It is not a correctness timeout.
* One helper thread per instance (B.5) costs a thread stack and a parked thread each; idle threads
  cost no CPU unless they spin. The prototype counts both.
* No allocation on either side per block. Buffers are sized at prepare for the maximum block
  size and reused. No pipe and no message queue on the audio path.
* Round trips: one per isolated slot per segment. The optional run batching of B.3 can make it one
  per run of adjacent isolated slots. Several segments are not merged. Round trips of different
  rows cannot be merged without delaying rows, because each row's input is ready only when its
  earlier stages finish; they run concurrently in different lanes instead (D.4).

### D.3 Shared memory ownership, one callback budget, crash and hang

**Ownership of a slot's buffers.** Each isolated slot has one request block (header, input,
output) in the shared section and two counters: `requestSeq` (written by DAL) and `doneSeq`
(written by the helper).

* DAL owns the block while `doneSeq == requestSeq`. It writes input and header, then increments
  `requestSeq` (release). From then on the **helper owns the block** until it sets
  `doneSeq = requestSeq` (release) after writing the output.
* A deadline miss does **not** return ownership. The slot enters "outstanding": DAL outputs
  silence for that stage, does not touch the block and sends no new request until it sees
  `doneSeq == requestSeq`. Then it discards that late output and resumes with the next segment.
  A late helper therefore never writes into a block DAL has reused.
* If the helper dies, its handle is signalled and the whole section is abandoned; recovery
  creates a new section for the new helper.

**One budget per callback.** At callback start the engine computes one absolute deadline:
`start + budget − reserve`, where `budget` is the block duration and `reserve` is the measured
time the callback still needs after the last isolated stage (mixing, master, output). Every wait
in this callback, in any lane, waits only until that same absolute time
(`WaitForMultipleObjects({done event, helper process}, deadline − now)`). Two isolated slots in
one lane, or ten in ten lanes, never get a block budget each. Read-ahead sets its own deadline per
segment from its lead time; offline has none (D.5).

**Miss:** the stage outputs silence for that block, a counter increments. Silence rather than dry
pass-through, because unprocessed DI guitar at full level is worse than a gap.

**Crash during playback** (process handle signalled): every isolated stage goes "lost" at once,
silent. DAL and all other plug-ins keep playing. A banner says "AmpliTube 4 host stopped".
Restore (C.5) starts when the transport stops, because loading plug-ins while playing conflicts
with the Power rule.

**Hang without crash:** detected when a control request does not return within its per-kind limit,
or when slots stay outstanding while the helper's progress counter (incremented by every helper
processing thread and its message loop) does not move. Audio is already protected by the deadline.
DAL does not kill the helper on its own while it might hold newer state. It shows "AmpliTube 4 host
is not responding" with **Restart host** (uses the last captured state and names what may be
lost) and **Wait**. A hang during a capture counts as a failed capture, so the destructive step is
cancelled (C.3).

The callback never waits without a bound, and the message thread never waits on the helper without
a bound either.

### D.4 Parallelism, render pool and read-ahead

* Rows are processed by the callback, render-pool workers (`InstrumentRenderPool`) or read-ahead
  workers, one lane per row per block [code]. Each isolated slot has its own helper thread and
  event pair, so N AmpliTube rows in N lanes run in parallel in the helper.
* **Overbooking:** a DAL lane waiting on the helper sleeps (after the short spin), and the helper
  thread runs in its place. Effective concurrency stays about the number of DAL lanes. With one
  render worker (a 4-core machine), the waits are on the critical path exactly as in-process
  processing was. Only the round-trip overhead is new.
* **No double processing:** one in-flight request per isolated slot, enforced by sequence number
  (assert plus counter in tests). Read-ahead and the live lane never own the same row in the same
  block (existing invariant). A row's read-ahead adoption must not change which helper instance
  serves it.
* **Read-ahead:** stage 3 keeps rows with isolated slots out of read-ahead adoption. Stage 5 lets
  them in after measurement. Read-ahead calls the same `processBlock`, with its own deadline.
* **Rest:** resting stages send no request (the skip happens before `processBlock`). AmpliTube
  reports a 0 tail, so it keeps running after input (A.3) and its round trips continue after Stop.
  That cost is measured in the prototype.

### D.5 Device, block size, sample rate, transport, offline, state, editor

| Concern | Handling |
|---|---|
| Device change, sample-rate or block-size change | DAL already calls `prepareToPlay` on the message thread with processing drained. The stand-in forwards it as a control request and reallocates the shared buffers for the new maximum block before the reply. Until the reply, the stage outputs silence. |
| Transport, tempo, position, cycle | `PositionInfo` per segment is copied into the request header. The helper gives its instance a playhead that returns exactly that. Segments are not merged, so a cycle wrap is a segment boundary as today. |
| Offline export | `setNonRealtime(true)` is forwarded. There is no real-time deadline, but the wait is never unbounded: it waits in short slices and between slices checks (a) the helper process handle, (b) the export's existing cancel request (`AudioMixdownExporter`'s `cancelRequested`, fed by the progress window's Cancel), and (c) the helper's progress counter. A dead helper, a Cancel, or no progress for an offline hang limit ends the export with a message naming the helper; it never writes silence into the file. An export is bit-identical to in-process only if AmpliTube itself is deterministic **[to prove]**; identity is tested with a deterministic plug-in (DAL Mono Delay) forced into the helper in a test build. |
| State access | Only on the helper's message thread, via control requests, under C.3. Preset get/set goes the same way. |
| Editor | Opened on request in the helper as a **top-level window owned by the helper**, titled with the DAL track and slot. Not reparented into DAL's window. Microsoft documents that a cross-process `SetParent` forces a DPI-awareness reset of the child window's process (`SetParent`, Remarks). A cross-process parent/child pair is also widely reported to share input processing, so a hung helper could stall DAL's UI. That second point is not in the API reference and is **[to prove]** only if reparenting is ever reconsidered. DAL passes `AllowSetForegroundWindow` so the helper can bring the editor to front. Open/close state goes back to DAL, and an open isolated editor keeps the stage awake (rest rule). The editor dies with the helper and is not reopened automatically after a restore. Behaviour with AmpliTube's own windows (licensing dialogs, resize, high DPI) is **[to prove]**. |

---

## E. Stages

Each stage is one or a few commits on its own branch, starting from PR #12. Stages 0 and 6 do not
depend on isolation.

| # | Stage | Content | Done when |
|---|---|---|---|
| 0 | **PR #12 completion (done)** | A.2 Power guards for undo/redo, reconcile, wake steps and deferred destruction. A.3 rest revision 2: category policy, instruments never rest, wake on transport, parameter and state. Report §4 and §6 corrected. Fixture checks: MIDI editor on an Off row, Save/reopen while Off, Power undo. | See report §9. |
| 1 | **Prototype (measure, not ship)** | Helper flag in the DAL exe. Shared section, event pair and the ownership and deadline rules of D.3. A test-only `IsolatedInsertInstance` that runs one or N real AmpliTube 4 instances, and DAL Mono Delay for bit identity. Editor smoke test only. Driven from a focused test harness, and from the app behind a diagnostic command-line flag only. No recovery. | The acceptance table below. |
| 2 | Control plane and lifetime | `IsolatedHostConnection`, create, prepare, state, preset, destroy, exit-without-destroy, `IsolatedPluginPolicy`. The factory switch in `instantiateLiveSlot` and the add path. | Project with AmpliTube saves and reloads byte-identical state in and out of isolation. Project close and app exit never call AmpliTube's destructor. |
| 3 | Audio path in the engine | Same-block requests, segment batching, adjacent-slot batching, bounded wait, lost and late states, pause flag for C.3. Read-ahead adoption excludes isolated rows. | Section F audio checks pass. |
| 4 | Off, crash and hang recovery | C.3–C.5, `PluginRecoveryCoordinator`, UI toast and banner, command blocking, Retry. | Section F crash checks pass. |
| 5 | Editor and read-ahead | Helper-owned editor windows. Isolated rows allowed into read-ahead after measurement. | Section F editor and read-ahead checks pass. |
| 6 | Off-only decoded audio | `AudioClip` keeps metadata and a handle to resident PCM. A material store with reference counts from **On** rows only. The waveform pyramid is keyed by material, not by `AudioClip*`, and is kept. Wake re-decodes before the row comes On. Undo snapshots hold handles, not PCM. | Memory drops by the material of Off-only rows. Shared material is never released while an On row uses it. Waveforms stay visible. |

**Stage 1 scope.** Helper process with AmpliTube 4 loaded, authorised and processing; the shared
section and events; the deadline wait with ownership as in D.3; a test-only stand-in. It also
includes a **minimal editor smoke test**: on a test command the helper opens AmpliTube's editor as
a helper-owned top-level window and closes it again. Editor integration in DAL's UI (menus, focus,
state capture on close) stays in stage 5. Recovery stays in stage 4.

**Acceptance for stage 1.** Always measured **together for DAL and helper** and compared with
in-process AmpliTube under the same conditions: same machine, same temp copy of TSE_pt2 with its
10 AmpliTube 4 instances, same device, same worker count.

| Measure | Conditions | Accept when |
|---|---|---|
| Added block latency | impulse through one isolated and one in-process AmpliTube on two rows | the two outputs line up to the sample (the goal of D.1, verified) |
| Deadline misses and device overruns | playing 10 min and stopped 10 min; buffer sizes 64, 128, 256 and 512 at 48 kHz | zero at 128, 256 and 512; at 64 reported, decision with the user |
| Callback time | same | mean and max per buffer size; max stays below 50 % of the block budget at 128 and above |
| Added time per isolated slot | same | median and 99.9th percentile reported per buffer size; 99.9th ≤ 10 % of the block budget at 128 and above |
| First sound block | Play from silence with input on an AmpliTube row | first non-silent output block equals in-process |
| CPU | process CPU of DAL + helper (sum), playing and stopped, with and without spinning | reported against in-process; the added total and the spinning share are stated, no threshold invented before the numbers exist |
| Memory | private bytes of DAL + helper (sum) | within 150 MB of in-process for 10 instances |
| Thread cost | helper threads, idle wakeups per second | reported |
| Parallelism | 10 isolated rows over 7 workers and over 1 worker | per-block wall time grows by less than 10 × one round trip (shows overlap) |
| Memory reclaim | end the helper with 10 instances | helper gone, no AmpliTube destructor called, DAL unchanged |
| Runs at all | AmpliTube 4 in the helper | loads, authorises, processes; its editor opens and closes in the smoke test |

One render worker on this machine is **not a simulation of Conny's laptop**; it only shows the
critical path with fewer workers. If AmpliTube refuses to run or authorise in a second process,
stage 1 stops there. If the misses, callback or per-slot criteria fail, the numbers are reported
and the fallback for AmpliTube Off is report option 2 (keep loaded, released, no processing) for
the verified AmpliTube identity, which needs the user's decision.

---

## F. Verification

Automated in a focused test executable unless marked manual. "Real AmpliTube" means the installed
plug-in on this machine. Projects are temp copies only.

| Area | Check |
|---|---|
| Forced crash during destroy | Test-only helper command "crash on next destroy" (null write). Off one of 10 isolated instances: DAL lives, project unchanged, the 9 desired-On instances are restored with byte-identical state, the Off one is not loaded, Play blocked during restore and allowed after. |
| Forced crash during playback | "crash in processing thread on block k": other rows' output continues bit-identical to a run without isolation, isolated stages are silent from block k, no callback exceeds its deadline, restore runs after Stop. |
| Forced hang | "spin forever on next request" and "hang in getState": deadline misses counted, no callback over budget, the destructive step is cancelled with state intact, the "not responding" UI appears, Restart restores. |
| Crash during restore | Helper crashes on the first create: one attempt only, failed state, Retry works, no loop (counted helper starts = 2). |
| State kept | Active/Bypass, Pre/Post, slot order and state bytes after: Off/On, Save/reopen, Duplicate, undo/redo, crash restore. |
| Only On restored | Rows A On, B Off, C On; crash: helper holds exactly A and C. |
| Repeated cycles | 50 Off/On cycles of a row with 2 AmpliTube instances, then 10 project switches between two projects: no leaked helper, no leaked instance count, helper ends when the last instance goes. |
| Memory reclaimed | All AmpliTube rows Off: helper private bytes 0 (process ended). DAL private bytes back to the "all Off" level of the report §5 E within 10 %. |
| First block and parallelism | First block after Play bit-identical to in-process for DAL Mono Delay in the helper. With N isolated rows in N lanes, wall time per block grows by less than N × single-slot round trip (shows overlap). |
| Read-ahead and offline | Read-ahead excluded (stage 3) then included (stage 5). No row processed twice (sequence counters). Offline export identical to realtime for the deterministic plug-in. Export fails cleanly on a forced hang. |
| Shutdown | App exit and project switch with 10 AmpliTube instances, 20 times: no crash dump, no AmpliTube destructor (helper log). |
| Real AmpliTube, editor closed and open (manual) | Listening: same tone as in-process. Open the editor, change a parameter, close: state captured. With the editor open, force a crash: DAL lives, editor gone, restore uses the last capture, message says what could be lost. |
| Rest and isolation together | Isolated stages that never had input rest and send no requests. Counter of requests while stopped and never played = 0. |

---

## G. What can be finished in PR #12 and what waits for isolation

**Can be finished in PR #12, no isolation needed:**

* Stage 0, done: Power guards (A.2), rest revision 2 with its wake triggers (A.3), corrected
  report wording (A.5), the exact-base test comparison (A.4), and the fixture checks.
* The proxy change stays as is: missing mode reads Manual, explicit choices are written, and Off
  rows never render.

**Must wait for isolation (or for a decision on the interim):**

* Destroying AmpliTube 4 instances on Off and on insert or track delete. Until stages 2–4 exist,
  PR #12 still destroys them in-process with the measured crash risk (report §3.4). PR #12 should
  not go into a tester build before either isolation lands or the user picks an interim (report
  options 2 or 3).
* Shutdown and project switch without AmpliTube's destructor (C.4).

**Independent, separate PR after #12:** stage 6, Off-only decoded audio (under 83 MB in TSE_pt2,
report §5).

**Not proposed:** a general out-of-process plug-in host, sharing one plug-in instance between
rows, pipelined isolation (adds a block of offset), and any rest rule that cuts a tail without the
plug-in's own report.

### Primary sources

* JUCE 8.0.4: `juce_VST3PluginFormat.cpp` (tail, state and editor threading),
  `juce_ConnectedChildProcess.cpp`, `juce_InterprocessConnection.cpp`,
  `juce_Files_windows.cpp` (named pipe, file mapping), `extras/AudioPluginHost` (out-of-process
  scanner pattern).
* Steinberg VST 3 SDK: `IAudioProcessor::getTailSamples` (`kNoTail` = 0, `kInfiniteTail`), and
  `ProcessData` silence flags.
* Microsoft Win32: `SetParent` Remarks (cross-process DPI-awareness reset), `AttachThreadInput`,
  `CreateFileMapping`, `WaitForMultipleObjects`, `AvSetMmThreadCharacteristics`,
  `AllowSetForegroundWindow`.
