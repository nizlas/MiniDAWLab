# Plan: full Off unloading and one shared out-of-process host for AmpliTube 4 (2026-11-05)

Status: **plan only.** Nothing in this document is implemented except what section A lists as
current state. Base: branch `claude/resource-saving` at `4ad4784` (draft PR #12), which is
`main` `8472feb` plus four commits (proxy opt-in, Off lifecycle, automatic rest, report). Result
report for those commits: `docs/RESOURCE_SAVING_2026-11-05.md` ("the report" below).

Each statement is marked by where it comes from:

* **[code]** read in this repository at `4ad4784`, or in JUCE 8.0.4 as built here
  (`build/ninja-release/_deps/juce-src`).
* **[measured]** run on this machine (Core Ultra 9 275HX, Windows 11, RME Babyface Pro ASIO,
  48 kHz / 512).
* **[to prove]** an assumption the prototype or a test must confirm before anything depends on it.

**Short answer.**

* Off already unloads and frees the row's plug-ins (report §3). What is missing for "full" Off:
  decoded audio of Off-only rows, a Power guard on undo/redo, and a crash-safe way to destroy
  AmpliTube 4.
* AmpliTube 4 moves to **one helper process per DAL session** that owns every AmpliTube 4
  instance. DAL keeps one small stand-in per insert slot that looks to the rest of DAL like an
  ordinary `juce::AudioPluginInstance`. Commands and state go over JUCE's existing child-process
  IPC. Audio goes through shared memory, processed **in the same block**: no added block latency,
  and AmpliTube rows do not move against other rows.
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
| MIDI editor | `MidiEditorPresenter::auditionHostForTrack` (`src/app/MidiEditorPresenter.cpp:174`) | Opens without a loaded instrument. Audition goes to the host, which discards it while Off, so audition is silent. **Not tried by hand [to prove].** |
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

Needed completion (stage 0):

1. `reconcileNow`'s unload branch waits while `transportBusy` (a row that is Off is not processed
   anyway, so waiting costs only memory).
2. Wake jobs created by a snapshot restore wait for Stop before loading anything.
3. Undo/redo whose restore changes any row's Off flag is refused while playing, with the same
   message the Power button gives. This is narrower than blocking all undo during playback.

With these, every path that can destroy or create a plug-in because of Off runs only while
stopped. Deleting an insert or a track is already behind the same predicate.

### A.3 Automatic rest: what is safe and what is missing [code]

Implemented (`src/plugins/InsertChainRest.h`, report §4):

* **Finite reported tail.** Rest after 2 × tail + latency + 1 block of silent input and output. A
  plug-in heard after its own tail ran out marks its stage unreliable for the session. Safe for
  plug-ins that report honestly. The second tail is the check.
* **Never had input.** Rest after latency + 2 blocks of silent output.
* **Infinite or zero-reported tail** (Pro-R; AmpliTube, VB3-II, HALion, Groove Agent, DAL Tuner):
  keeps running after the first input. This is why "after Stop" stays at 1.5 ms instead of
  0.75 ms [measured, report §5].

Review of the "never had input" rule found three gaps where a resting stage is not woken:

| Gap | Code | Effect | Fix (stage 0) |
|---|---|---|---|
| Transport start, locate or cycle wrap | `Params::generation` is bumped only by `updateRestParams` (signature: instance, epoch, bypass, tail, latency, kind, editor open) and by the instrument's `refreshRestParameters` | A plug-in that makes sound on its own when the transport starts (a transport-synced generator, an instrument arpeggiator or pattern without MIDI) stays asleep, because nothing feeds it input. | Bump the generation of every stage that has never had input on Play, locate and cycle wrap. The stage re-qualifies after latency + 2 blocks. Cost: about 2 blocks of processing per start. |
| Parameter change without an open editor | The editor-open check covers editor edits. Changes from the generic parameter window are covered (`paramsWindows_`). Changes from DAL code (`setValueNotifyingHost` at `MainAppWindow.cpp:7388`) and from the plug-in itself are not. | A parameter that turns on a self-sounding mode is not heard until the next input. | Attach a `juce::AudioProcessorListener` per live instance. `audioProcessorParameterChanged` and `audioProcessorChanged` (latency, tail or program change) bump the stage generation with one atomic increment. This is safe from any thread. |
| Preset or state load into the same instance | The instrument signature uses the owner pointer, which does not change on `setStateInformation`. For inserts, `instanceEpoch` is set only when an instance is placed (`PluginInsertHost.cpp:601`). Applying a preset (`setPreset` via `getExtensions`, `:887`) or restoring state in place keeps it unchanged. | A preset with a drone does not start until input. | Bump the generation after every `setStateInformation` / `setPreset` DAL performs. |

Remaining work for heavy chains after Stop, with no arbitrary cutting of long echoes:

* **Zero-reported tails (AmpliTube, instruments).** VST3 `kNoTail` (0) is the default and does
  not reliably mean "no tail". JUCE's wrapper does not expose the plug-in's output silence flags:
  `getTailLengthSeconds` reads `getTailSamples` (`juce_VST3PluginFormat.cpp:3199`), and input
  silence flags are cleared [code]. Possible next step, **not a timeout**: an opt-in observation
  that a stage's output stayed exactly zero for a full `latency + block` after the input went
  silent, repeated on several stops, is still not proof. **No rule is proposed here.** It stays
  "keeps running", and the remaining cost is reported.
* **Infinite tails (Pro-R).** Nothing to do safely. Rest only when the user switches the row
  Off.
* **Instruments.** An instrument with no held notes still reports 0. It keeps running after its
  first note. A rule based on the voice state would need the plug-in to report it, which VST3
  does not standardise.
* **Where the remaining cost runs.** The stopped callback still runs the non-resting stages
  serially (1.5–1.8 ms). Dispatching them through the render pool while stopped is a valid,
  separate optimisation. It is not proposed in this plan.

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

The last good state must be fresh when something destructive happens. It is captured
[control channel, helper message thread]:

* At every Save, Save As and autosave (DAL already calls `getStateInformation` here).
* **Before any destructive helper operation**: for **every** isolated instance in the helper,
  not only the one being destroyed, because a crash takes them all.
* When an isolated editor closes, and after parameter activity has been quiet for one message-loop
  turn (helper reports "dirty" from `audioProcessorParameterChanged`, DAL fetches state when idle).
* At the plug-in undo recorder's existing capture points (`UndoRedoCoordinator::onPluginUndoRecord`).

### C.3 Exclusive access in the helper

For a capture to count, no processing may be in flight on that instance:

1. DAL marks the isolated slots of the helper "paused". Their lanes output silence for those
   stages and send no request. This is a published flag read per block, like dormancy.
2. DAL waits for its own drain (callback exit, render-pool idle for those rows, read-ahead paused
   and acknowledged). These are the existing mechanisms (`waitForAudioCallbackExit`,
   `pauseReadAheadWorkerAfterChainPublish`).
3. The helper confirms per instance that `inFlight == 0` and its processing thread is parked.
   **No acknowledgement means no capture and no destructive step.** The operation is cancelled
   and the instances are resumed unchanged (same rule as `DormancyResult::NotDrained`).
4. Capture, then resume or continue with the destructive step.

Power changes happen while stopped (A.2), so pausing all isolated stages for the capture
interrupts nothing audible except a tail after Stop.

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

### D.1 Same-block processing gives zero added latency

The lane that processes a row, at the isolated slot's position in the chain:

1. writes the stage input (2 channels × n samples, float) into the slot's shared input buffer, plus
   a small header: sequence number, n, `PositionInfo` per segment, sample rate generation;
2. signals the slot's request event;
3. waits for the done event (bounded, D.3);
4. reads the output buffer into the chain's buffer and continues with the next slot.

The audio comes back in the same block, so DAL adds **no samples of delay**. AmpliTube's own
reported latency (7 samples [measured, report §4.1]) is unchanged: DAL has no project PDC and does
not compensate it today, in-process or isolated. **AmpliTube rows do not move against other rows**
compared with today.

Rejected alternative: **pipelined processing** (send block k, return block k−1). It removes the
wait but delays AmpliTube rows by one block (512 samples = 10.7 ms here) against all other rows.
Without project PDC that is an audible shift. Not accepted.

### D.2 Cost for the callback

* Per isolated slot per processed block: two buffer copies (2 × 512 × 4 B = 4 KB each way), two
  event operations, and two cross-process thread wakes. The wakes dominate. On Windows a wake is
  usually microseconds, but its tail depends on power state and scheduling **[to prove; this is
  the prototype's main number]**.
* Mitigation: the lane spins on the shared "done" sequence for a short, measured period before
  it sleeps on the event. The helper thread does the same on "request". The spin period is a
  tuning constant chosen from the prototype's distribution, not a correctness timeout.
* No allocation on either side per block. Buffers are sized at prepare for the maximum block
  size and reused. No pipe and no message queue on the audio path.
* Adjacent isolated slots in the same chain and stage (AmpliTube → AmpliTube) are sent as **one
  request**: the helper runs them in order on one thread. All segments of a callback block
  (cycle wrap, sub-block segmentation) are sent in one request with per-segment `PositionInfo`.
  So the number of round trips is one per contiguous isolated run per lane per block, never one
  per segment.
* A separate round trip per instance across **different rows** cannot be merged without delaying
  rows, because each row's input is ready only when its earlier chain stages finish. Those round
  trips run concurrently in different lanes instead of serially (D.4).

### D.3 Bounded waiting; a crash or hang during playback

* The audio-path wait is `WaitForMultipleObjects({done event, helper process handle}, deadline)`.
  The deadline is derived from the block: the time left in this callback's budget minus a measured
  margin for the rest of the graph. Read-ahead uses its own segment deadline. It is a real-time
  bound, not a guess at plug-in behaviour.
* **Deadline missed:** the stage outputs silence for that block, the late result is dropped by
  sequence number, a counter increments, and the next request is not sent until the helper has
  caught up. Silence is used rather than dry pass-through, because unprocessed DI guitar at full
  level is worse than a gap.
* **Process handle signalled (crash during playback):** every isolated stage goes "lost" at once,
  silent. DAL and all other plug-ins keep playing. The transport is not stopped, but a banner
  says "AmpliTube 4 host stopped". Restore (C.5) **starts when the transport stops**, because
  loading plug-ins while playing conflicts with the Power rule. The user can also press Stop at
  once.
* **Hang without crash:** detected by a control-channel liveness request that does not return,
  and by repeated missed deadlines with no progress counter movement in shared memory. Audio is
  already protected by the deadline. DAL does not kill the helper on its own while it might still
  hold newer state. It shows "AmpliTube 4 host is not responding" with **Restart host** (uses the
  last captured state, says what may be lost) and **Wait**. A hang during a capture counts as a
  failed capture, so the destructive step is cancelled (C.3).
* The callback never waits without a bound. The message thread never waits on the helper without
  a bound either: every control request has a limit per kind (create and load longest), after
  which it is reported, never assumed done.

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
| Offline export | `setNonRealtime(true)` is forwarded. The audio-path deadline is replaced by "wait for done or process exit", and a stalled helper fails the export with a message instead of writing silence. An export is bit-identical to in-process only if AmpliTube itself is deterministic **[to prove]**. Identity is tested with a deterministic plug-in (DAL Mono Delay) forced into the helper in a test build. |
| State access | Only on the helper's message thread, via control requests, under C.3. Preset get/set goes the same way. |
| Editor | Opened on request in the helper as a **top-level window owned by the helper**, titled with the DAL track and slot. Not reparented into DAL's window. Microsoft documents that a cross-process `SetParent` forces a DPI-awareness reset of the child window's process (`SetParent`, Remarks). A cross-process parent/child pair is also widely reported to share input processing, so a hung helper could stall DAL's UI. That second point is not in the API reference and is **[to prove]** only if reparenting is ever reconsidered. DAL passes `AllowSetForegroundWindow` so the helper can bring the editor to front. Open/close state goes back to DAL, and an open isolated editor keeps the stage awake (rest rule). The editor dies with the helper and is not reopened automatically after a restore. Behaviour with AmpliTube's own windows (licensing dialogs, resize, high DPI) is **[to prove]**. |

---

## E. Stages

Each stage is one or a few commits on its own branch, starting from PR #12. Stages 0 and 6 do not
depend on isolation.

| # | Stage | Content | Done when |
|---|---|---|---|
| 0 | **PR #12 completion** | A.2 Power guards for undo/redo and reconcile. A.3 wake on transport start, parameter change and state load. Report §6 wording corrected (A.5). Manual checks: MIDI editor on an Off row, Save/reopen while Off, undo of Power. | Focused tests for each guard and wake. Selftests, InsertPersistence and InstrumentParallel green. Base comparison (A.4) unchanged. |
| 1 | **Prototype (measure, not ship)** | Helper flag in the DAL exe. Shared section + event pair. A test-only `IsolatedInsertInstance` that runs one or N real AmpliTube 4 instances, and DAL Mono Delay for bit identity. Driven from a focused test harness, and from the app behind a diagnostic command-line flag only. No recovery, no editor. | Numbers for the stop criteria below. |
| 2 | Control plane and lifetime | `IsolatedHostConnection`, create, prepare, state, preset, destroy, exit-without-destroy, `IsolatedPluginPolicy`. The factory switch in `instantiateLiveSlot` and the add path. | Project with AmpliTube saves and reloads byte-identical state in and out of isolation. Project close and app exit never call AmpliTube's destructor. |
| 3 | Audio path in the engine | Same-block requests, segment batching, adjacent-slot batching, bounded wait, lost and late states, pause flag for C.3. Read-ahead adoption excludes isolated rows. | Section F audio checks pass. |
| 4 | Off, crash and hang recovery | C.3–C.5, `PluginRecoveryCoordinator`, UI toast and banner, command blocking, Retry. | Section F crash checks pass. |
| 5 | Editor and read-ahead | Helper-owned editor windows. Isolated rows allowed into read-ahead after measurement. | Section F editor and read-ahead checks pass. |
| 6 | Off-only decoded audio | `AudioClip` keeps metadata and a handle to resident PCM. A material store with reference counts from **On** rows only. The waveform pyramid is keyed by material, not by `AudioClip*`, and is kept. Wake re-decodes before the row comes On. Undo snapshots hold handles, not PCM. | Memory drops by the material of Off-only rows. Shared material is never released while an On row uses it. Waveforms stay visible. |

**Stop criteria for stage 1** (on this machine, 48 kHz / 512, 10 AmpliTube 4 instances on 10 rows,
TSE_pt2 temp copy, 10 minutes playing plus 10 minutes stopped, and again with one render worker):

* Added callback time per isolated slot: median ≤ 50 µs, 99.9th percentile ≤ 300 µs. Callback
  maximum stays below 50 % of the block budget. **Zero** deadline misses and zero device
  overruns.
* Output of DAL Mono Delay through the helper is bit-identical to in-process. AmpliTube output
  differs from in-process by no more than AmpliTube's own run-to-run difference.
* Helper memory per AmpliTube instance is within 10 % of the in-process growth per instance,
  plus a fixed helper baseline of at most 150 MB.
* Ending the helper with 10 instances frees their memory (private bytes of the helper go to
  zero, DAL unchanged), with no AmpliTube destructor called.
* AmpliTube runs in the helper at all: authorisation, presets and the editor in a separate
  process. If AmpliTube refuses to run in a second process, stop.

If the criteria fail: report the numbers. The fallback for AmpliTube Off is then report option 2
(keep loaded, released, no processing) for the verified AmpliTube identity. That needs a decision
from the user, because it is identity-based special handling.

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

* Stage 0 in full: Power guards (A.2), the three rest wake fixes (A.3), corrected message-thread
  wording (A.5), this exact-base test comparison (A.4), and the manual Off checks.
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
