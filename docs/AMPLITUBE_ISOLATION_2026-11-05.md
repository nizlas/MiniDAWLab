# AmpliTube 4 in a shared helper process (2026-11-05)

Implements the plan in `OFF_UNLOADING_AND_AMPLITUBE_ISOLATION_PLAN_2026-11-05.md` (sections B–D)
for AmpliTube 4. Evidence: `docs/evidence/amplitube-isolation-2026-11-05/`.

## Short answer

* **AmpliTube 4 now runs in one helper process per DAL**, and every other plug-in stays in DAL.
  DAL's audio lanes hand each block to the helper through shared memory and get it back in the
  same block. No block latency is added: the output is bit-identical to in-process at 64, 128 and
  512 samples, checked with a deterministic plug-in.
* **AmpliTube's destructor no longer runs at Off of the last instance, project close or app
  exit.** In those cases the helper is terminated. In the app, Off of every row with all 10
  AmpliTube instances loaded ran 3 times out of 3 without a crash, and no helper was left behind.
  Before, it crashed 4 times out of 10. Off of one AmpliTube row while other AmpliTube rows stay
  On still runs that instance's destructor inside the helper. If that crashes, DAL keeps running,
  and only the On instances are restored, from state captured just before.
* **Memory:** the 10 AmpliTube instances use 2.6 GB private in the helper. All of it is returned
  when the helper ends. DAL plus helper together is 4.74–4.76 GB, against 4.89–4.91 GB with
  everything in-process.
* **Cost:** about 60 µs extra per processed AmpliTube block (round trip). With 9 AmpliTube rows
  processing, the stopped callback mean is 4.4 ms against 3.9 ms in-process, and the max is 8.3
  ms against 7.4 ms. 10 blocks went above 70 % of the 10.7 ms budget; no block overran. DAL and
  helper together use 0.78–0.95 core, against 0.74–0.92 in-process.
* **Not done:** releasing decoded audio of Off-only rows (plan stage 6). Off releases plug-in
  memory, not audio material.

## What is built

| Part | Where | Notes |
|---|---|---|
| Identity | `IsolatedPluginPolicy::shouldIsolate` | VST3 and "AmpliTube 4" and "IK Multimedia" and class unique id `0x62218172` and bundle `AmpliTube 4.vst3`, all required. The name alone does not qualify, and a different id with the same name stays in-process (tested). `--no-plugin-isolation` turns it off. `--isolate-plugin-for-test <identifier>` exists for A/B runs only. |
| Factory | `PluginInsertHost::createInsertInstance` | The only branch. Project load, add insert and wake from Off all use it. Saved projects are unchanged: same descriptor, same state bytes. |
| Stand-in | `IsolatedInsertInstance` (`src/plugins/isolation/IsolatedInsertHosting.*`) | A `juce::AudioPluginInstance`, so the engine, render pool, read-ahead, rest, Off, undo and persistence handle it unchanged. It forwards prepare, release, state, preset (through a VST3 client in `getExtensions`), latency and tail. |
| Helper | `IsolatedHostWorker.cpp`: DAL's exe with the JUCE child-process prefix | Opens no audio device and touches no settings or projects. Runs one processing thread per instance (MMCSS "Pro Audio"), and creation, state and editor on its message thread. When the pipe is lost (DAL closed, ended or crashed) it terminates itself without running destructors. A job object with kill-on-close also ends it if DAL crashes. |
| Audio path | shared section per instance, plus an event pair | DAL writes the input and transport, publishes `requestSeq`, waits, then reads the output. No allocation, lock or pipe on the lane. |
| Deadline | `IsolatedProcessDeadline` | The engine publishes one absolute deadline per callback, at 80 % of the block, and every isolated wait in any lane shares it. Read-ahead sets its own per segment. Offline export waits in 50 ms slices, watching the helper process and its progress counter (30 s with no progress counts as a hang). |
| Missed deadline | stand-in | The stage outputs silence, not the dry signal. The section stays with the helper until it reports that sequence. No new request is sent and the late output is discarded, so a late helper never writes into a reused buffer (tested). |
| Off | `PluginInsertHost::unloadForOff` / `destroyNextOffRetiredInsert` | State is captured via the helper. A failed capture cancels the unload, and the row stays loaded and silent. Before a destroy that runs a destructor in the helper, every On instance is paused, drained and captured; if that fails, the destruction is postponed. If only Off rows remain in the helper, it is terminated instead. |
| Crash | `IsolatedHostConnection` | Detected through the process handle in the lanes' waits and in a watch thread. Stages go silent. Restore runs only while stopped, with "Restoring plugins... k/N" one instance per message-loop turn. Play, record, Power, structural edits, undo/redo, save and open are blocked meanwhile. If no On instance exists, no helper is started. There is one automatic attempt; after that, a failure dialog offers Retry, and state stays saved. |
| Hang | control requests with per-kind limits | A request that is not answered marks the helper unresponsive and offers "Restart" from the last captured state. Audio is protected by the deadline. |
| Editor | `openNativeEditor` → helper top-level window | AmpliTube's real editor runs in the helper, not reparented into DAL. Closing it captures state and makes the same undo step as an in-process editor. |
| Export | `AudioMixdownExporter` | A block the helper cannot produce fails the export with a message, and nothing is written. |

## State capture contract

* `getStateInformation` asks the helper for fresh state on its message thread while the instance
  keeps processing on its own thread. That is the same VST3 rule DAL already relies on
  in-process: `getState` on the UI thread beside `process`. Audio is not paused, so this alone
  proves no more about consistency than in-process does.
* The first capture happens right after creation, so a crash before any other capture restores
  the loaded settings and not AmpliTube's defaults.
* Parameter and editor changes reported by the plug-in mark the instance "changed since capture"
  (a 20 Hz flag in the helper, sent as Dirty). While stopped, the change is captured once it has
  been quiet for 1 s; while playing, capture is deferred until Stop. Save and autosave capture at
  once. Closing the editor captures at once while stopped.
* Exclusive capture (pause, drain, then capture all) happens only before an operation that can
  run a destructor in the helper, and those operations happen only while stopped.
* What a helper crash can lose: changes since the last capture of that instance. That means
  edits during the current playback, or less than 1 s of edits while stopped, or edits in an
  editor that is still open. The recovery message names the instances that had uncaptured
  changes. DAL does not report those changes as saved.

## Tests

| Suite | Result | Covers |
|---|---|---|
| `IsolatedInsertFocusedTests` (new) | 79 / 0 | See the list below. |
| `InsertPersistenceFocusedTests --isolate-delay <exe>` (new mode) | 153 / 0 | The existing Off, Power-while-busy, stepwise On, Save/reopen and dormant-chain suite, run through the real insert host and `TrackPowerCoordinator` with DAL Mono Delay in the helper. |
| `InsertPersistenceFocusedTests` (in-process) | 153 / 0 | |
| `MiniDAWSelftests` 3543 / 0, `InsertAudible` 86 / 0, `Solo` 111 / 0, `InstrumentParallel` 45 / 0, `ExportLevel` 76 / 0, `ArrangementMidiEvent` 87 / 0, `MidiEditorCohort` ok | unchanged | |

`IsolatedInsertFocusedTests` checks:
* bit identity with in-process at 64, 128 and 512, with the impulse on the same sample;
* the first block after create;
* the state round trip, presets through the VST3 client, and four parallel lanes each matching in-process;
* a late reply: the miss is silent, no new request goes out while the section is owned, and the late output is discarded;
* a slow `getState` within its bound, and an offline wait for a slow block;
* a forced crash in a destructor: one new helper restores the two On instances with their states;
* a forced crash while processing: the lane returns within its deadline and the stage is marked lost;
* a forced crash during restore: one attempt, Failed, Retry works, no loop;
* project-close teardown and the last-instance retire end the helper with exit code 0, so the armed destructor crash never runs;
* real AmpliTube 4: identity, loading 3 instances (2.6 s), latency 7, processing at 64–512, and the real editor opening and closing in the helper.

## Measurements (ASIO Fireface, 48 kHz / 512, 7 workers, temp copy of TSE_pt2, same exe)

`--stability-perf-profile <copy> --seconds 20 --warmup 5 --idle --idle-gap 30`. "Isolated" is the
default; "in-process" uses `--no-plugin-isolation`. CPU is sampled for DAL and helper separately
(cumulative CPU time every second) and summed. Each figure is one run.

**Project as saved** (the 10 AmpliTube rows are muted, so AmpliTube processes nothing here):

| Window | Callback mean / max, in-process → isolated | DAL + helper cores | Private DAL + helper |
|---|---|---|---|
| Before first Play | 0.77 / 1.44 → 0.81 / 1.46 | 0.45 → 0.46 | 4.89 → 2.09 + 2.64 = 4.73 GB |
| Playing | 0.57 / 1.23 → 0.54 / 1.12 | 0.48 → 0.50 | same |
| After Stop | 1.51 / 2.28 → 1.64 / 2.40 | 0.50 → 0.52 | same |

**Fixture with the 9 muted AmpliTube rows unmuted** (`TSE_pt2-ampon.dalproj`, temp copy only):

| Window | Callback mean / max, in-process → isolated | >70 % blocks | DAL + helper cores |
|---|---|---|---|
| Before first Play | 3.88 / 5.45 → 4.38 / 6.10 | 0 → 0 | 0.74 → 0.78 |
| Playing | 0.97 / 1.80 → 1.12 / 2.02 | 0 → 0 | 0.88 → 0.94 |
| Right after Stop | 5.50 / 7.41 → 5.99 / 8.31 | 0 → 10 | 0.92 → 0.94 |
| 30 s after Stop | 5.31 / 7.39 → 5.93 / 7.79 | 0 → 7 | 0.88 → 0.95 |

No overrun in any run. Summed per block, the AmpliTube instances cost 2.18 ms isolated against
1.77 ms in-process (7 instances), which is about 60 µs per instance per block.

**Every source row switched Off in the running process (`--all-off`), isolated, 3 runs:** no crash,
helper gone, DAL 0.48–0.50 GB private, 0.06–0.08 core, callback 0.02 ms.

**Direct round trip** (`IsolatedInsertFocusedTests`, no device): DAL Mono Delay has a mean of
3–24 µs and a max of 18–34 µs at 64–512 samples. AmpliTube 4 has a mean of 38–67 µs including its
own processing, and a max of 0.6–1.1 ms. One miss occurred in 5562 requests, on the first block
after a re-prepare.

Not measured: buffer sizes other than 512 with the device. That would mean changing the user's
audio settings, which this task forbids; sizes 64–256 were measured directly without a device.
Also not measured: one render worker, and any other machine. One worker on this machine is not a
simulation of Conny's laptop.

## Not done or limits

* **Decoded audio of Off-only rows is not released** (plan stage 6). Off frees plug-in memory:
  in-process, and for AmpliTube by unloading in the helper or ending it. The decoded PCM of Off
  rows stays resident, about 83 MB as float in TSE_pt2.
* No parameter list is mirrored. DAL's generic parameter window shows nothing for AmpliTube; its
  own editor is the way to edit it.
* A hang shows only through a control request that is not answered. A helper that is alive but
  stuck only on the audio side shows as deadline misses, not as "not responding".
* There is no helper-side spin. DAL lanes spin 50 µs before sleeping. The per-block cost above is
  with that setting; spinning more was not tried.
* The peak-block increase (10 blocks above 70 % with 9 processing AmpliTube rows after Stop) is
  real headroom lost. On a 4-core laptop the margin is smaller, which is not measured.
* No listening test, and no manual editor test in DAL's UI (the editor was opened and closed in
  the helper by the test).
