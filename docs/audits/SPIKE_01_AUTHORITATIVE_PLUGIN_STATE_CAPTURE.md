# SPIKE-01 — Authoritative Plugin-State Capture: Evidence Report

**Spike:** SPIKE-01 (canonical steering §9.2, §22 slice P0/P1A; blocks P1C)
**Canonical authority:** `docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md` (status `Canonical`,
merged to `main` in PR #1, squash commit `3bd5abf746dc6948d96ed3cda5242f187c8d8170`)
**Repository-evidence basis:** `docs/audits/PORTABLE_INSTRUMENTS_ARCHITECTURE_AUDIT.md`
**Branch:** `cursor/spike-01-plugin-state-capture`
**Date:** 2026-09-05
**Classification of statements:** every material statement below is marked
**Verified** (read in the repository at the cited location), **Measured** (produced by running
code), **Inference** (reasoned but not directly observed), **Open** (unknown, requires evidence),
or **Proposed** (a recommendation, not implemented).

---

## 1. Executive verdict

> ## `INCOMPLETE — awaiting local VB3-II measurements`

The diagnostic harness is implemented, compiled, and its pure logic is verified by 17 new
deterministic selftest checks (**Measured**, §16 below: 194 checks total, 0 failures). All
repository-level SPIKE-01 questions (call chains, editor-close behavior, notification
infrastructure, Save/autosave agreement, dirty/undo semantics, threading constraints) are answered
with **Verified** evidence in this report.

The runtime questions — capture cost (A), byte stability (B), notification coverage (C, E) —
require a real VB3-II-class VST3 instrument, a GUI, and an operator. They have **not** been
measured. The exact ~10–15-minute local procedure for Niclas is in §20.2.

**PID-001 must NOT be marked resolved on the basis of this report alone** (Open until the local
measurement run is attached and reviewed).

---

## 2. Scope and governing canonical requirements

In scope (canonical §9.2, quoted requirement list):

* cost and message-thread impact of `getStateInformation` on VB3-II-class plugins;
* whether repeated capture of an unchanged audible state produces byte-stable blobs;
* coverage and reliability of host parameter notifications for plugin-GUI-driven changes;
* editor-open and editor-close behavior (capture opportunities and state visibility);
* behavior for state changes not represented by ordinary automatable parameters;
* Save/autosave/enqueue agreement on "current state";
* a safe fallback if state blobs are not byte-stable for identical audible state;
* conservative invalidation preferring unnecessary re-rendering over silent staleness.

Locked contract constraints honored by the harness (PI-020, §9.2 rules 1–6): all captures on the
message thread only (rule 5); capture/hashing creates no undo entries and no dirty state (rule 6).

Out of scope and **not implemented** (task classification): proxy rendering, fingerprint
production logic, background render queues, schema v20 / proxy metadata, playback-source
substitution, Secondary instruments, final-feature UI, automatic polling or listener architecture
in the product path, any permanent `PrimaryStateAuthority`. The canonical steering document is
**not modified** by this branch; §19 contains the proposed amendment for separate human review.

---

## 3. Current verified state-capture call chains

All line numbers verified on this branch (base `3bd5abf`), 2026-09-05.

### 3.1 The single instrument capture primitive (Verified)

`ExperimentalInstrumentHost::getCurrentInstrumentStateBase64`
(`src/plugins/ExperimentalInstrumentHost.cpp:2055–2085`):

* refuses to run off the message thread (returns empty; lines 2057–2060);
* loads `activeOwner_` (atomic `shared_ptr` holding the one live `juce::AudioPluginInstance`);
* calls `inst->getStateInformation(mb)` directly (line ~2073) and Base64-encodes the bytes.

### 3.2 Who calls it (Verified — the complete list)

Only `InstrumentTrackController::buildExperimentalInstrumentProjectBlock()`
(`src/instruments/InstrumentTrackController.cpp:836`), at two sites:

* line 883 (`kind == "GenericVst3"`, plugin loaded) — with failure reporting
  (`stateCaptureFailure`, lines 880–886);
* line 948 (Groove Agent / HALion kinds, `pluginWasLoadedOnSave`).

Indirect callers of that DTO builder:

| Caller | Location | Keeps state blob? |
|---|---|---|
| Project Save / Save As / autosave | `src/domain/Session.cpp:1755` via `saveProjectToFile` | Yes |
| Track-delete undo capture | `src/app/TrackLanesEditCoordinator.cpp:217` | Yes (restore needs it) |
| Musical undo snapshot | `InstrumentTrackController.cpp:1009–1017` | **No** — immediately stripped by `stripExperimentalInstrumentTrackPluginFieldsForUndo` (`src/io/ProjectFile.cpp:1844–1853`) |

### 3.3 All other `get/setStateInformation` sites for the instrument instance (Verified)

* Restore at load: `setStateInformation` inside `loadInstrumentFromDescription`
  (`ExperimentalInstrumentHost.cpp:2230`), message-thread-guarded (2093–2096), applied **before**
  the instance is published to the audio thread.
* Drum-name Phase C probe (`ExperimentalInstrumentHost.cpp:3121–3321`, message thread): creates a
  **temporary second instance**, copies live state via `liveInst.getStateInformation(state)` →
  `probe->setStateInformation(...)` (3160/3163), runs offline `processBlock`s, then
  `releaseResources` and destroys the function-local instance. Precedent: cloning live state on
  the message thread is already an accepted repository pattern.
* The audio-insert host (`src/plugins/PluginInsertHost.cpp`: 134, 218, 230, 701, 772, 1070, 1105,
  1123) has its own capture sites, including **editor-open baseline and editor-close diff**
  (see §9) — for insert FX only, not instruments.

### 3.4 Save path (Verified)

`Ctrl+S` (`src/app/MainWindow.cpp:211–217`) / File → Save →
`ProjectIoCoordinator::saveProject()` (`src/app/ProjectIoCoordinator.cpp:519–521`) →
`session_.saveProjectToFile(..., ctlLookup, ...)` (:573–583) → `Session.cpp:1740–1756` iterates
instrument controllers → §3.2. **Save always captures fresh live state; no cached blob is used.**
The only cached copy, `pendingPluginStateBase64_` (`InstrumentTrackController.h:468`), is a
load/restore staging field, never a save source (Verified).

### 3.5 Autosave path (Verified)

`ProjectIoCoordinator` is a `juce::Timer` (message thread), tick 60 s
(`autosave_policy::kTickMs`, `ProjectIoCoordinator.cpp:332`), first write 60–120 s after dirty,
escalating intervals 2/5/10/15 min (:345–350). `timerCallback` (:1414–1465) →
`writeAutosaveNow("periodic")` → the **same** `saveProjectToFile` path as Save ⇒ fresh capture.
Autosave is blocked during recording/count-in (`MainAppWindow.cpp` block provider) and by modal
dialogs, **not** during plain playback. Autosave runs only when `isProjectDirty()` (:1417–1422) —
which interacts badly with §13's dirty gap.

---

## 4. Diagnostic design and files involved

Design principle: **operator-driven, message-thread-only, hash-only, removable.** No polling, no
timers, no product-path change; the panel exists only when the hidden `--spike01-state-capture`
command-line flag is present.

| File | Status | Role |
|---|---|---|
| `src/diagnostics/Spike01Sha256.h` | new | Dependency-free SHA-256 (FIPS 180-4) so raw state bytes never leave the process — only `{size, hash, timing}` |
| `src/diagnostics/Spike01ReportFormat.h` | new | Pure data model + min/median/p95/max statistics + sanitized markdown report builder. **Privacy by construction:** the sample type cannot carry blob bytes |
| `src/diagnostics/Spike01StateCapturePanel.h/.cpp` | new | Manual measurement window: phase-labelled timed captures (raw `getStateInformation` and production Save path), `juce::AudioProcessorListener` attach/detach instrumentation, operator notes, sanitized Desktop report |
| `src/plugins/ExperimentalInstrumentHost.h/.cpp` | 2 accessors added | `spike01LiveInstanceForDiagnostics()` (message-thread-guarded, same guard idiom as `getCurrentInstrumentStateBase64`) and `spike01IsNativeEditorOpenForDiagnostics()` — clearly marked SPIKE-01-only |
| `src/Main.cpp` | flag hook | `--spike01-state-capture` → `callAsync` → `MainWindow::startSpike01StateCaptureProbe()` (same pattern as `--stability-*`) |
| `src/app/MainWindow.h/.cpp`, `src/app/TransportControlsShortcutTarget.h`, `src/app/MainAppWindow.cpp` | forwarding + panel ownership | `invokeStartSpike01StateCaptureProbeFromStartup()` builds decoupling callbacks (track list from `SessionSnapshot`, host lookup via `InstrumentRuntimeCoordinator::getInstrumentHostForTrack`, transport state via `transport.readPlaybackIntentForUi()`) |
| `CMakeLists.txt` | 1 source registration | `Spike01StateCapturePanel.cpp` on the app target |
| `tests/selftest/MiniDAWSelftestsMain.cpp` | +17 checks | FIPS SHA-256 vectors, statistics, report sanitization, phase-matrix coverage |

Removal: delete `src/diagnostics/Spike01*.*`, the two host accessors, the flag hook chain, the
CMake line, and the selftest block. Nothing in the product path depends on any of it (Verified by
construction; the only product-path reference is the flag check in `Main.cpp`).

Why an in-app panel instead of an external throwaway probe (Inference, stated in §17 options):
the spike must measure the **production host seam** (same instance, same layout preparation, same
Save path, transport running) — an external JUCE host would measure a different host and could
not answer F (Save agreement) at all.

---

## 5. Test environment and plugin identities

* Harness build environment (Measured): Windows 11, MSVC 2022 x64, Ninja, CMake preset
  `windows-ninja-debug`, JUCE via FetchContent; app + selftests built from this branch,
  exit code 0 (2026-09-05).
* Measurement environment (Open — to be filled by the local run): Niclas's studio machine,
  VB3-II VST3 (identity/version recorded automatically in the sanitized report header from
  `PluginDescription`), plus the active audio device/sample rate as an operator note.
* Note: capture-cost numbers from a **Debug** build overstate production cost (Inference); the
  report should state the build config used. If Debug numbers are borderline, repeat with
  `windows-ninja-release`.

## 6. Capture-cost measurements (requirement A)

**Open — awaiting local VB3-II run.** The harness records, per capture: duration (hi-res ms),
blob size, SHA-256, message-thread flag, editor-open flag, transport flag; per phase it reports
n/min/median/p95/max and size range. Required phases A1 (editor closed, stopped), A2 (editor
open, stopped), A3 (editor open, playing), A4 (repeated unchanged), A5 (immediately after a
plugin-GUI parameter change) are preset in the panel. UI/audio impact is judged by the operator
during A3 and recorded as an operator note. No universal acceptable-duration threshold is
asserted (task rule); consequences are assessed in §17 once numbers exist.

## 7. Byte-stability results (requirement B)

**Open — awaiting local VB3-II run.** The harness computes SHA-256 per capture and reports
distinct-hash counts per phase plus a cross-phase hash inventory. Phases B2 (editor
reopened/closed, no intentional change), B3 (save+reload of unchanged state), B4 (parameter
returned to original value), B5 (after transport activity) map §9.2's stability cases.
Interpretation rules committed in advance: byte-identical ⇒ stable; byte-different is only
"apparently audibly equivalent" if the operator attests no audible change — **byte comparison
alone never proves audible equivalence** (task rule). Raw blobs are never persisted or printed.

## 8. Parameter-notification coverage (requirement C)

**Verified today:** the repository contains **zero** `juce::AudioProcessorListener` registrations,
zero `audioProcessorParameterChanged` / `audioProcessorChanged` handlers, zero parameter
attachments — for instruments *and* inserts (exhaustive search; insert "parameter undo" is an
opaque-blob diff at editor open/close, not a listener). Today, DAL is structurally blind to live
plugin-GUI parameter changes.

**Open (runtime):** whether VB3-II's VST3 wrapper delivers `audioProcessorParameterChanged` /
gesture callbacks / `audioProcessorChanged{programChanged, nonParameterStateChanged}` for GUI
drawbar moves, preset changes, and editor-closed changes — and on which thread. The harness
records kind, parameter index, name (resolved only when the callback arrives on the message
thread), normalized value, thread, and timestamp for every event while attached.

## 9. Editor-open and editor-close findings (requirement D)

* Instrument editor close performs **no state capture today** (Verified):
  `ExperimentalPluginEditorWindow::closeButtonPressed` (`ExperimentalInstrumentHost.cpp:1611–1622`)
  only hides the window and asynchronously calls `editorWindowClosing()` (:1666–1669), which
  logs and resets `editorWindow_`. Canonical §4.5/§9.1 statement re-verified on this branch.
* Contrast: the **insert** host does capture at open and diff at close
  (`PluginInsertHost.cpp:1070`, `:1123`) and pushes a parameter-undo step — an existing in-repo
  precedent for an editor-close capture hook (Verified).
* A safe instrument hook point exists: `editorWindowClosing()` runs on the message thread behind
  an `AsyncCallbackGuard` staleness check, and the live instance is still loaded (`activeOwner_`
  untouched by editor close) — so `getStateInformation` remains available immediately before and
  after close (Verified from code structure; runtime confirmation is phase B2 in the local run).
* Whether merely opening/closing the editor changes serialized bytes without a sound change is
  **Open** (phase B2/A2-vs-A1 hash comparison).
* Plugin destruction/unload (`unloadInstrument`, editor closed first, then atomic clear + drain +
  `releaseResources`) is a final safe capture opportunity on the message thread **before** the
  atomic clear (Verified structure; **Proposed** as a future hook — not implemented).
* No permanent editor-close policy is implemented (task rule).

## 10. Non-parameter-state findings (requirement E)

**Open — plugin-dependent.** The harness observes `audioProcessorChanged` with JUCE
`ChangeDetails` flags (`latencyChanged`, `parameterInfoChanged`, `programChanged`,
`nonParameterStateChanged`) and pairs E1 (preset/program change) and E2 (MIDI-learn/mode switch)
phases with immediate hash captures, so each non-parameter action is classified as:
notified + blob changed / not notified but blob changed (visible only on fresh capture) /
not observable. Documented limit: the spike can only test what VB3-II exposes; MIDI-learn and
routing switches beyond VB3-II's feature set stay Open for other plugins (explicitly recorded in
§18).

## 11. Save/autosave/conceptual-enqueue agreement (requirement F)

* Save and autosave **share one code path** and both capture fresh live state (Verified, §3.4–3.5).
  There is no divergent "autosave reuses last blob" behavior to reconcile.
* Explicit Save is permitted during playback; autosave is blocked only by recording/count-in and
  modal dialogs (Verified). So capture-during-playback is already a production situation today —
  measuring it (A3) validates existing behavior as much as the future enqueue.
* The panel's **F1 snapshot checkpoint** performs a raw capture and a production-Save-path capture
  back to back and compares hashes — the conceptual render-enqueue equivalent required by the
  task. Agreement result: **Open** (expected identical; Inference based on both paths calling the
  same `getStateInformation` on the same instance on the same thread).
* No production enqueue implementation was created (task rule).

## 12. Threading and lifecycle findings

* All capture paths are message-thread-only, enforced by early-return guards
  (`ExperimentalInstrumentHost.cpp:2057`, load :2093, and ~12 further sites — Verified).
  `JUCE_ASSERT_MESSAGE_THREAD` is not used anywhere in the repo; the guard idiom is
  `MessageManager… isThisTheMessageThread()` early-out. The new accessors use the same idiom.
* The audio thread never touches capture: it only reads the published atomic
  `activeOwner_` in `processBlock` (Verified). The harness never makes the live instance
  reachable from any worker thread; listener callbacks may *arrive* on any thread and are only
  recorded (lock-guarded), with plugin calls (name lookup) restricted to message-thread arrivals.
* Lifecycle safety: the panel resolves the host fresh through
  `InstrumentRuntimeCoordinator::getInstrumentHostForTrack` on every action and guards listener
  detach with the host's `asyncAliveGuard()` (repo Stability-Slice-4 pattern, Verified).
* Capture thread per sample is recorded in the report; expected: 100 % message thread (Measured
  once the local run exists).

## 13. Dirty and undo findings (requirement H)

* **Verified gap:** turning a knob in the instrument plugin's own GUI does **not** mark the
  project dirty today. `markProjectDirtyFromEdit` / `instrumentOrPluginEditsSinceClean_`
  (`ProjectIoCoordinator.cpp:1125–1127`, `.h:161`) is set by undo-coordinator callbacks, musical
  edits, track deletes and autosave recovery — never by instrument-editor activity. The editor's
  only activity hook, `messageThreadOnNativeEditorUserActivity()`, is a diagnostics no-op in
  production builds (Verified).
  **Consequence (Inference):** because periodic autosave runs only when dirty, a session where
  the *only* change is live plugin tweaking may never autosave — the tweak is captured only by an
  explicit Save (or a save triggered by some other dirtying edit).
* Musical undo strips all plugin fields (`stripExperimentalInstrumentTrackPluginFieldsForUndo`,
  `ProjectFile.cpp:1844–1853`; equality ignores plugin fields :1902–1942) — so plugin-state noise
  cannot create musical undo steps (Verified, consistent with PI-020/§18.3).
* Diagnostic capture/hashing mutates nothing: `getStateInformation` is a read; the harness
  performs no `setStateInformation`, no session edits, no undo pushes, no dirty marking (Verified
  by construction). Runtime cross-check: run undo (Ctrl+Z) after captures — nothing to undo from
  capture alone (operator step in §20.2).
* A future state authority can therefore satisfy §9.2 rule 6 (capture never dirties/undoes;
  the *user's edit* should dirty per existing rules) — but making the user's plugin-GUI edit
  dirty at all requires the very change-detection mechanism this spike evaluates (Inference).

## 14. Failure and privacy handling

* Capture failure today: `getCurrentInstrumentStateBase64` returns empty on wrong thread /
  no instrument / zero bytes; the Save DTO builder records a failure reason
  (`stateCaptureFailure`, `InstrumentTrackController.cpp:880–886`) (Verified).
* Harness failure handling: no instrument / no live instance ⇒ status message, no sample
  recorded; a zero-byte capture would appear as `bytes=0` with the SHA-256 of empty input —
  visibly anomalous in the report (Verified by construction).
* Privacy: raw state bytes exist only in short-lived stack buffers inside the two capture
  functions; the report format structurally cannot carry them (no byte field exists). The
  selftest proves a planted "license-like" secret in a fake blob cannot appear in the rendered
  report while its hash does (Measured). The plugin *binary path* (`fileOrIdentifier`) is
  recorded as plugin identity; it identifies the installed plugin, not user data (Inference:
  acceptable; flagged here for reviewer awareness).

## 15. Evidence matrix — the eight §9.2 requirements

| # | §9.2 requirement | Repository evidence | Runtime evidence | Status |
|---|---|---|---|---|
| A | `getStateInformation` cost / message-thread impact | call chain + thread guards (Verified) | phases A1–A5 | **Open — local run** |
| B | Byte stability of unchanged state | none possible statically | phases A4, B2–B5 | **Open — local run** |
| C | Parameter-notification coverage | no listeners exist today (Verified) | listener instrumentation | **Open — local run** |
| D | Editor open/close behavior | close captures nothing; safe hook exists; insert precedent (Verified) | B2 + operator steps | **Partially answered (Verified); runtime part Open** |
| E | Non-parameter state | `ChangeDetails.nonParameterStateChanged` observable (Verified API) | phases E1–E2 | **Open — local run** |
| F | Save/autosave/enqueue agreement | one shared fresh-capture path (Verified) | F1 checkpoint hash comparison | **Largely answered (Verified); confirmation Open** |
| G | Fallback for non-byte-stable blobs | design evaluated §17 (Proposed) | depends on B results | **Proposed — gated on B** |
| H | Dirty/undo semantics | GUI edits never dirty; undo strips blobs; capture mutates nothing (Verified) | undo cross-check step | **Answered (Verified)** |

## 16. Checks run (Measured)

* Full Debug build of `MiniDAWLab` + `MiniDAWSelftests` in the isolated spike worktree:
  configure + build exit 0 (MSVC 2022 x64 / Ninja / `windows-ninja-debug`).
* `MiniDAWSelftests.exe`: **194 checks, 0 failures**, including the 17 new `spike01:` checks
  (FIPS 180-4 SHA-256 vectors incl. multi-block and incremental equivalence; min/median/p95/max
  statistics including nearest-rank p95; report sanitization — planted secret never appears,
  full hash does; phase-matrix coverage A1–A5, B2–B5, E1, E2, F1).
* Normal startup unchanged without the flag: the only product-path reference is one
  `commandLine.contains("--spike01-state-capture")` branch in `Main.cpp` (Verified by diff
  inspection); no timers, listeners, or captures exist unless the panel is opened and used.
* Not run (out of spike scope, per task): release certification, stability matrix, endurance
  tests.

## 17. Options considered and recommended mechanism

### Options

1. **Parameter listeners as authoritative state.** Rejected regardless of measurement outcome
   (Inference from JUCE semantics + canonical warning): listeners cannot represent non-parameter
   state (presets, MIDI-learn, internal blobs), and §9.2 explicitly forbids assuming a listener
   sees all changes. At best listeners are hints.
2. **Periodic background polling of `getStateInformation`.** Rejected: canonical PI-020 requires
   message-thread capture; polling burns message-thread time proportional to capture cost
   (unknown until A), risks plugin-side effects, and the task forbids polling in the product
   path. Could only be revisited if A shows sub-millisecond captures *and* hints prove unusable.
3. **Editor-close capture only.** Insufficient alone: changes while the editor stays open (or via
   MIDI-learn with the editor closed) would never be seen until Save (Verified gap pattern).
4. **Checkpoint-based fresh capture + cheap hints + generation counter** (the canonical
   hypothesis). **Proposed** — pending confirmation by measurements; not selected merely because
   it was the prior preference: §3–§13 evidence already establishes that fresh capture at
   checkpoints is the only path that exists and works today, and options 1–3 are each
   independently insufficient. What the measurements decide is whether *hints* (listeners,
   editor-close) can shorten staleness-detection delay, and whether *hashing* can cheaply prove
   "unchanged" (byte stability).

### Recommended mechanism (Proposed; confidence: medium pending A/B/C measurements)

Answers to the required recommendation questions:

* **What is authoritative?** The latest successful **fresh capture** (`getStateInformation` on
  the message thread), performed at defined checkpoints. Never a listener event, never a cached
  blob, never a hash by itself.
* **What is only a cheap change hint?** Parameter/gesture/`audioProcessorChanged` callbacks (if
  C shows they arrive) and instrument-editor open→close transitions. Hints only bump a
  **monotonic change-generation counter** on the Primary; they never carry state.
* **When must fresh capture occur?** At explicit Save and autosave (already true today —
  Verified); at render enqueue (future, per PI-020 rule 1); and at proxy-staleness evaluation
  when `capturedGeneration < currentGeneration`.
* **While the editor is open:** no continuous capture. Hints accumulate; Auto-mode staleness
  evaluation applies the §18.1 debounce. If C shows zero notifications from VB3-II, the editor-
  open period itself must conservatively count as "possibly changed" (generation bumped at
  editor open — Proposed conservative default).
* **When the editor closes:** bump the generation (cheap hint; insert-host precedent exists) and
  optionally schedule one fresh capture on the message thread. Not implemented in this spike.
* **Changes without parameter callbacks:** detected conservatively — any editor-open/close cycle
  and any program/`nonParameterStateChanged` notification bumps the generation; when in doubt the
  proxy is stale (canonical G rule: prefer unnecessary re-render over silent staleness).
* **If capture fails:** the previous authoritative blob stays, the proxy/fingerprint is marked
  stale/unknown (never "current"), the failure is logged like today's `stateCaptureFailure`.
* **If unchanged blobs are not byte-stable:** hashing cannot prove "unchanged"; equality
  optimizations are disabled and staleness relies purely on the generation counter + mandatory
  fresh capture at Save/enqueue. Blobs are never normalized or reinterpreted (task rule).
* **Save and autosave:** unchanged behavior — both already capture fresh (Verified). The shared
  future contract is "one capture function, called at every checkpoint", which they already
  satisfy.
* **What becomes dirty/undoable:** capture/hash: nothing (rule 6). The user's plugin edit should
  dirty the project once a hint mechanism exists (fixing the Verified gap in §13); it must not
  create musical undo steps (blob-stripping stays).
* **Maximum bounded detection delay:** without hints — until the next checkpoint (Save/enqueue);
  with hints — the §18.1 debounce window. A hard number is deliberately not asserted before C
  measurements (Open).
* **Thread ownership:** capture, hashing, hint bookkeeping, generation counter: message thread.
  Audio thread: never involved. Render workers: receive an immutable copied blob only.
* **Plugin-specific evidence:** everything in A/B/C/E is VB3-II-specific until repeated with a
  second instrument; the mechanism above is deliberately safe even under the worst measured
  outcome (no callbacks, unstable bytes).

## 18. Known limitations and unsupported claims

* No real-plugin numbers exist yet; §6, §7, §8, §10 are Open (this is why the verdict is
  INCOMPLETE).
* Audible equivalence is attested by the operator, never inferred from bytes (task rule).
* All runtime findings will be VB3-II-specific; generalization to other VST3 instruments is
  explicitly unsupported until a second plugin is measured.
* Debug-build timings overstate production cost; a Release re-run is recommended if numbers are
  borderline.
* The harness cannot observe changes the plugin neither notifies nor serializes; such state is
  untestable from the host by definition (documented limit).
* `spike01LiveInstanceForDiagnostics` intentionally trusts the operator not to unload/replace the
  instrument mid-action; it is not production-hardened (diagnostic scaffolding only).

## 19. Exact proposed steering amendment (for separate human review — NOT applied)

> **Amendment to §9.2 (after the SPIKE-01 requirement list), proposed by SPIKE-01:**
>
> *SPIKE-01 repository findings (2026-09-05, branch `cursor/spike-01-plugin-state-capture`):*
> 1. *DAL contains no plugin parameter/processor listeners anywhere (instruments or inserts);
>    live plugin-GUI edits are structurally invisible today and do not dirty the project. The
>    §13 dirty gap means tweak-only sessions may never autosave.*
> 2. *Save and autosave already share one fresh-capture path
>    (`buildExperimentalInstrumentProjectBlock` → `getCurrentInstrumentStateBase64`); §9.2 rule 4's
>    "Save MUST capture fresh state" is Verified for autosave as well.*
> 3. *The instrument editor-close handler (`editorWindowClosing`, message thread, staleness-
>    guarded) is a safe hint hook; the insert host already implements an editor-close state diff
>    as precedent.*
> 4. *The authoritative mechanism remains as hypothesized — fresh capture at checkpoints +
>    generation counter + hints — now supported by repository evidence for D/F/H; final selection
>    stays gated on the VB3-II measurements for A/B/C/E (PID-001 remains Open until then).*
>
> **Status change:** none. PID-001 stays Open / evidence-gated until the local measurement
> appendix is attached to this report and reviewed.

## 20. Completion status and remaining local actions

### 20.1 Status

* Harness: complete, compiled, selftested (Measured).
* Repository questions (D, F, H + all call chains): answered (Verified).
* Runtime questions (A, B, C, E, F-confirmation, G-gating): **awaiting Niclas's local VB3-II
  run** (~10–15 min once built).
* PID-001: **not resolved**.

### 20.2 Exact local procedure (Windows PowerShell, from the spike worktree)

The spike worktree `C:\Users\nicla\development\MiniDAWLab-spike01` already contains a built
Debug app. If starting fresh instead: `git fetch origin` and check out
`cursor/spike-01-plugin-state-capture`, then step 1 builds everything needed.

```powershell
# 1. Build only what the diagnostic requires (skip if the existing build is current)
cd C:\Users\nicla\development\MiniDAWLab-spike01
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1   # Debug preset

# 2. Launch the SPIKE-01 diagnostic
.\build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe --spike01-state-capture
```

3. **Load VB3-II:** create/select an Instrument track and load VB3-II exactly as usual. In the
   SPIKE-01 window, click **Refresh tracks** and select the VB3-II track.
4. **A1** (editor closed, transport stopped): phase `A1` → **Capture x10 (raw)**.
5. **A2:** open the VB3-II editor (normal Instrument button). Phase `A2` → **Capture x10**.
6. **A3:** press Play (any small project/loop). Phase `A3` → **Capture x10**. Note in the note
   field whether audio glitched or the UI stuttered → **Add note**. Stop playback (that stop is
   phase B5's precondition).
7. **B5:** phase `B5` → **Capture x10**.
8. **A4** (repeated unchanged): do not touch anything. Phase `A4` → **Capture x10**, wait ~10 s,
   **Capture x10** again.
9. **Listener + A5:** click **Attach parameter listener**. Move one drawbar in VB3-II's GUI and
   release it. Phase `A5` → **Capture x1**. Check the listener line: did notifications arrive?
   Add a note describing what you moved.
10. **B4:** move the same drawbar back to its exact original position. Phase `B4` →
    **Capture x10**.
11. **B2:** close the VB3-II editor (its close button) **without changing anything**, phase `B2`
    → **Capture x10**; reopen the editor, **Capture x10** again (same phase).
12. **E1:** change a preset/program inside VB3-II. Phase `E1` → **Capture x1**. Note whether the
    listener line showed `programChanged`/`nonParameterStateChanged` or nothing.
13. **E2** (if VB3-II offers MIDI-learn or a mode switch): perform one such change. Phase `E2` →
    **Capture x1** + a note describing the action. If not available, add a note "E2 not
    supported by plugin".
14. **F1:** click **F1 snapshot checkpoint (raw + Save path)** — the status line reports whether
    the raw and production-Save hashes MATCH.
15. **B3:** save the project (Ctrl+S), close and restart the app **with the same flag**
    (step 2), reopen the project, Refresh tracks, phase `B3` → **Capture x10**.
16. **H cross-check:** press Ctrl+Z once — confirm nothing unexpected is undone by captures
    alone; note the result.
17. Click **Detach parameter listener**, then **Write sanitized report (Desktop)**.

**Report location:** `Desktop\SPIKE01_STATE_CAPTURE_REPORT_<timestamp>.md`.
**Privacy check before sharing (step 7 of the task):** open the file and confirm it contains
only tables of timings/sizes/hex hashes and notification metadata — no base64 blocks, no state
dumps. (The format cannot contain them by construction, but verify visually.)

### 20.3 After the run

Attach the sanitized report (or its tables) to this document as a measurement appendix on this
branch; then §1's verdict, §6–§8, §10, §15 and §17's confidence are updated and the PR leaves
draft after human review.
