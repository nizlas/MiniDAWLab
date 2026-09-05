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

> ## `PASS (measured) — checkpoint fresh-capture + generation-counter mechanism confirmed`

The diagnostic harness is implemented, compiled, and its pure logic is verified by 17 new
deterministic selftest checks (**Measured**, §16 below: 194 checks total, 0 failures). All
repository-level SPIKE-01 questions (call chains, editor-close behavior, notification
infrastructure, Save/autosave agreement, dirty/undo semantics, threading constraints) are answered
with **Verified** evidence, and the runtime questions are now answered with **Measured** evidence
from the local run of 2026-09-05 (three app sessions, 167 timed captures, 230 parameter
notifications, two plugins — appendix §21):

* **A (cost):** raw `getStateInformation` on VB3-II costs **≈0.47 ms median / 1.13 ms max**
  (Debug build, ~10.4 KB blob) — negligible at checkpoint frequency. An accidental but valuable
  second plugin, Groove Agent SE (~148 KB blob), costs ≈4.6 ms median / 6.5 ms max — still
  affordable per checkpoint, clearly unjustifiable for continuous polling. **100 % of captures
  ran on the message thread**; no audio/UI anomalies were reported.
* **B (byte stability):** VB3-II is byte-stable at rest in every §9.2 case — repeated capture,
  editor close/reopen, parameter-returned-to-original (exact baseline hash restored), after
  transport activity, and **across save → full app restart → project reload** (hash
  `875dc964caa6…` identical before save and after reload). It is **not** byte-stable while
  playback actively drives it with MIDI/CC. **Groove Agent SE is not byte-stable even idle**
  (10 consecutive untouched captures → 10 distinct hashes, monotonically growing size). Byte
  equality is therefore usable only as a *positive* "unchanged" proof; inequality proves
  nothing (§7).
* **C (notifications):** VB3-II delivers `audioProcessorParameterChanged` for GUI drawbar moves
  (dense ~3–7 ms streams with correct index/name/value) — **all on the message thread**. Zero
  gesture callbacks and zero `audioProcessorChanged` callbacks were observed from either plugin.
  Listeners are usable as cheap hints, nothing more — the canonical assumption is confirmed.
* **E (non-parameter state):** a VB3-II preset change produced no `programChanged` /
  `nonParameterStateChanged` callback; it *was* visible as a 146-event full-parameter burst and
  as a changed blob on fresh capture. Groove Agent SE's idle state churn produced zero
  notifications — non-parameter state changes can be completely silent. Conservative
  generation-bump hints remain mandatory.
* **F (agreement):** the F1 checkpoint measured **identical hashes** for the raw capture and the
  production Save path (both 10 393 bytes; 0.56 vs 0.55 ms).

The recommended mechanism (§17) — authoritative fresh capture at checkpoints, listeners and
editor transitions only as generation-counter hints, hash equality only as a positive
short-circuit — is confirmed with **high confidence** for the measured plugins.

**PID-001:** the §9.2 evidence gate is now satisfied for a first VB3-II-class instrument.
Resolving PID-001 and applying the §19 steering amendment remain a **human review decision**;
this branch does not modify the canonical document. Known residual gaps: E2 (MIDI-learn) was not
performed, and generalization beyond the two measured plugins is unsupported (§18).

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
* Measurement environment (**Measured**, 2026-09-05 21:53–22:35 CEST): Niclas's Windows 11
  studio machine (win32 10.0.26200), **Debug** build (`windows-ninja-debug`) from this branch,
  app version 0.2.0, real project with 5 instrument runtimes. Plugins measured:
  * **VB3-II VST3 2.3.1** (`C:\Program Files\Common Files\VST3\VB3-II.vst3`) — the intended
    target; 157 captures.
  * **Groove Agent SE VST3 5.2.20** (Steinberg) — measured accidentally in phase B3 of the
    second app session (the track selector defaulted to a Groove Agent track after restart) and
    retained as a valuable second data point; 10 captures.
* Audio device/sample rate were not recorded (the operator-note step was skipped) — see §18.
* Debug-build caveat stands: Release captures should be faster; even the Debug numbers are far
  below any concerning threshold for checkpoint-frequency capture (Inference).

## 6. Capture-cost measurements (requirement A)

**Measured (Debug build, 2026-09-05; per-phase tables in §21).** 167 timed captures, 167/167 on
the message thread.

| Plugin | Blob size | n | min | median | p95 | max |
|---|---|---|---|---|---|---|
| VB3-II 2.3.1 | 10 393–10 455 B | 157 | 0.43 ms | ≈0.47 ms | ≈0.8 ms | 1.13 ms |
| Groove Agent SE 5.2.20 | 148 591–148 791 B | 10 | 4.41 ms | 4.62 ms | 6.47 ms | 6.47 ms |

* Cost scales roughly with blob size (~14× bytes ⇒ ~10× time). Both are trivially affordable at
  checkpoint frequency (Save, autosave, render enqueue, editor close). The Groove Agent figure
  (≈5 ms of message-thread time per capture) confirms that **continuous polling would be
  unjustifiable** for large-state plugins — supporting the no-polling design in §17.
* Editor open vs closed: no measurable cost difference (medians 0.46–0.50 ms in both states).
* Transport playing vs stopped: no measurable cost difference (30 captures while playing:
  0.43–0.98 ms).
* The production-Save-path capture (F1) cost the same as raw (0.55 vs 0.56 ms); Base64 encoding
  is negligible at this blob size.
* Operator-reported audio/UI impact: no anomalies reported for the measured run; the operator
  verbally reported "no audio anomalies" during the earlier (data-lost) first attempt. A formal
  A3 operator note was not recorded (§18).
* No universal acceptable-duration threshold is asserted (task rule); §17 assesses consequences.

## 7. Byte-stability results (requirement B)

**Measured (VB3-II + bonus Groove Agent SE; hash inventory in §21).** The pre-committed
interpretation rules apply: byte-identical ⇒ unchanged; byte-different is **never**, by itself,
evidence of audible change. Raw blobs were never persisted or printed.

**VB3-II is byte-stable at rest, in every §9.2 stability case:**

* **A4 (repeated unchanged):** 10 + 10 captures ~10 s apart — one hash.
* **B2 (editor closed → reopened, nothing touched):** 20 captures across the cycle — one hash.
  Merely opening/closing the editor does not change serialized bytes (closes §9's runtime
  question).
* **B4 (parameter moved away, then returned to original):** 20 captures — restored the **exact
  pre-change baseline hash** (`a58813…`). A round-tripped GUI gesture is byte-reversible.
* **B5 (after transport activity, stopped):** 11 captures — one hash, identical to the pre-play
  baseline; playback transients did not permanently perturb serialized state.
* **B3 (save → full app restart → project reload):** 10 captures after reload — one hash,
  `875dc964caa6…` (10 393 B), **byte-identical to the F1 hash captured at save time in the
  previous app session.** The persist/restore round trip is byte-stable for VB3-II.

**VB3-II is not byte-stable during active playback:** one 10-capture burst taken while the
project was actively driving the plugin (transport playing; the project sends CC11 swell data
and `Swell Pedal` is an exposed parameter) produced 4 distinct hashes with sizes 10 452–10 453 B
against the 10 433 B resting baseline. A separate 30-capture playing burst without concurrent CC
traffic was fully stable, and captures returned to the resting baseline hash after stop
(Measured; the CC11 attribution is Inference). Consequence: **hash comparisons are only
meaningful between captures taken at quiescent checkpoints**, never mid-performance.

**Groove Agent SE is not byte-stable at all, even idle:** 10 consecutive captures (editor
closed, transport stopped, zero interaction, ~10 ms apart) produced **10 distinct hashes** with
monotonically growing size (148 591 → 148 791 B, +17…+37 B per capture) — the plugin embeds
some counter/clock in its serialized state. This is the worst case §9.2 anticipated, observed in
practice.

**Conclusion (drives requirement G):** byte-hash equality **may** serve as a positive
short-circuit ("equal bytes ⇒ unchanged ⇒ proxy still valid") and does work for VB3-II-class
plugins at quiescent checkpoints; byte-hash inequality **must not** be interpreted as change of
audible state and, for Groove-Agent-class plugins, occurs on every capture. The general
mechanism must therefore be the §17 generation counter with conservative re-rendering, with hash
equality only as an opportunistic optimization.

## 8. Parameter-notification coverage (requirement C)

**Verified today:** the repository contains **zero** `juce::AudioProcessorListener` registrations,
zero `audioProcessorParameterChanged` / `audioProcessorChanged` handlers, zero parameter
attachments — for instruments *and* inserts (exhaustive search; insert "parameter undo" is an
opaque-blob diff at editor open/close, not a listener). Today, DAL is structurally blind to live
plugin-GUI parameter changes.

**Measured (VB3-II 2.3.1 VST3, `juce::AudioProcessorListener` attached):**

* GUI drawbar drags deliver dense `audioProcessorParameterChanged` streams: two gestures on
  `Drawbar Upper A 16'` (index 28) produced 84 events at ~3–7 ms spacing with correct index,
  name, and normalized value.
* **230 of 230 observed events arrived on the message thread.**
* **Zero gesture callbacks** (`…GestureBegin/End`) were observed for any interaction.
* **Zero `audioProcessorChanged` callbacks** (no `programChanged`, `nonParameterStateChanged`,
  or `parameterInfoChanged`) were observed from either plugin — including at a preset change.
* A VB3-II preset change instead surfaced as a **full-parameter burst**: 146 `paramChanged`
  events (indices 0–146, index 56 absent) within ~300 ms.
* Groove Agent SE with the listener attached and no GUI interaction: 0 events — its idle state
  churn (§7) is invisible to listeners, confirming listeners cannot be authoritative.

Conclusion: for VB3-II, listeners are excellent *hints* — low latency, message-thread delivery,
no thread-safety complications observed — but provide no gesture bracketing and no non-parameter
signal. Canonical §9.2's "hints, never authority" stance is now measured, not assumed.

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
* Whether merely opening/closing the editor changes serialized bytes without a sound change:
  **Measured — it does not** (VB3-II, phase B2: 20 captures across a close/reopen cycle, one
  hash; B3 additionally matched an editor-open save-time hash from a capture with the editor
  closed after restart).
* Plugin destruction/unload (`unloadInstrument`, editor closed first, then atomic clear + drain +
  `releaseResources`) is a final safe capture opportunity on the message thread **before** the
  atomic clear (Verified structure; **Proposed** as a future hook — not implemented).
* No permanent editor-close policy is implemented (task rule).

## 10. Non-parameter-state findings (requirement E)

**Measured (E1); E2 not performed.**

* **E1 (preset/program change in VB3-II's GUI):** no `audioProcessorChanged` callback of any
  kind arrived. The change **was** observable two ways: a 146-event full-parameter notification
  burst (§8), and a changed blob on the next fresh capture (new hash `875dc9…`, size
  10 433 → 10 393 B). Preset changes are thus detectable via hints for this plugin — but only
  because VB3-II republishes every parameter on preset load; a plugin that neither notifies nor
  republishes would stay invisible until the next checkpoint, so the conservative
  editor-open/close generation bump in §17 remains mandatory.
* **E2 (MIDI-learn / mode switch):** not performed — the E2 capture is hash-identical to E1 and
  no operator note describes an E2 action. E2 stays **Open** for VB3-II and all other plugins
  (§18).
* **Bonus negative evidence:** Groove Agent SE's idle state churn (§7) is itself non-parameter
  state changing with **zero** notifications — direct proof that non-parameter state changes can
  be completely silent.

## 11. Save/autosave/conceptual-enqueue agreement (requirement F)

* Save and autosave **share one code path** and both capture fresh live state (Verified, §3.4–3.5).
  There is no divergent "autosave reuses last blob" behavior to reconcile.
* Explicit Save is permitted during playback; autosave is blocked only by recording/count-in and
  modal dialogs (Verified). So capture-during-playback is already a production situation today —
  measuring it (A3) validates existing behavior as much as the future enqueue.
* The panel's **F1 snapshot checkpoint** performs a raw capture and a production-Save-path capture
  back to back and compares hashes — the conceptual render-enqueue equivalent required by the
  task. Agreement result: **Measured — MATCH** (2026-09-05 22:17:49: identical SHA-256
  `875dc9…`, identical size 10 393 B, 0.56 ms raw vs 0.55 ms Save path, both message thread,
  editor open, transport stopped).
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
* Capture thread per sample is recorded in the report; **Measured: 167 of 167 captures and 230
  of 230 listener events on the message thread — 100 %, as required by PI-020 rule 5.**

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
  by construction). Runtime cross-check: the operator did not record the Ctrl+Z observation
  (§18); no undo- or dirty-related anomaly was reported across 167 captures, and the structural
  guarantee is unaffected (capture performs no undo pushes or dirty marking by construction).
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
| A | `getStateInformation` cost / message-thread impact | call chain + thread guards (Verified) | 167 timed captures, 2 plugins, 100 % message thread (§6) | **Answered (Measured)** |
| B | Byte stability of unchanged state | none possible statically | A4, B2–B5 + B3 restart round trip; Groove Agent counterexample (§7) | **Answered (Measured, 2 plugins)** |
| C | Parameter-notification coverage | no listeners exist today (Verified) | 230 events: dense message-thread `paramChanged`; zero gesture/`audioProcessorChanged` (§8) | **Answered (Measured, VB3-II)** |
| D | Editor open/close behavior | close captures nothing; safe hook exists; insert precedent (Verified) | B2: open/close changes no bytes (§9) | **Answered (Verified + Measured)** |
| E | Non-parameter state | `ChangeDetails.nonParameterStateChanged` observable (Verified API) | E1 measured (silent callback-wise, visible via burst + blob); E2 not performed (§10) | **E1 answered (Measured); E2 Open** |
| F | Save/autosave/enqueue agreement | one shared fresh-capture path (Verified) | F1 checkpoint: hashes MATCH (§11) | **Answered (Verified + Measured)** |
| G | Fallback for non-byte-stable blobs | design evaluated §17 | Groove Agent SE proves the fallback is required in general; VB3-II shows equality short-circuit works (§7) | **Answered — mechanism Proposed with measured gating data** |
| H | Dirty/undo semantics | GUI edits never dirty; undo strips blobs; capture mutates nothing (Verified) | no anomalies observed; formal Ctrl+Z note skipped (§13) | **Answered (Verified)** |

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
* Local measurement run executed (Measured, 2026-09-05): three app sessions with
  `--spike01-state-capture`, operator-driven phase matrix, 167 captures + 230 notifications;
  sanitized reports committed under `docs/audits/spike01-measurements/` and condensed in §21.
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
   (Measured: ≈0.5 ms for VB3-II but ≈5 ms for Groove Agent SE — Debug), risks plugin-side
   effects, and the task forbids polling in the product path. The Groove Agent numbers close the
   revisit clause: polling is not viable as a general mechanism.
3. **Editor-close capture only.** Insufficient alone: changes while the editor stays open (or via
   MIDI-learn with the editor closed) would never be seen until Save (Verified gap pattern).
4. **Checkpoint-based fresh capture + cheap hints + generation counter** (the canonical
   hypothesis). **Proposed and now measurement-confirmed** — not selected merely because it was
   the prior preference: §3–§13 evidence establishes that fresh capture at checkpoints is the
   only path that exists and works today, and options 1–3 are each independently insufficient.
   The measurements answered the two open sub-questions: *hints* do shorten detection delay to
   milliseconds where the plugin cooperates (VB3-II, §8), and *hashing* can cheaply prove
   "unchanged" only as a positive equality check on byte-stable plugins at quiescent checkpoints
   (§7) — never in general (Groove Agent SE).

### Recommended mechanism (Proposed; confidence: **high** — A/B/C/E1/F measured on VB3-II, and
the byte-instability worst case measured on Groove Agent SE)

Answers to the required recommendation questions:

* **What is authoritative?** The latest successful **fresh capture** (`getStateInformation` on
  the message thread), performed at defined checkpoints. Never a listener event, never a cached
  blob, never a hash by itself.
* **What is only a cheap change hint?** Parameter callbacks (Measured: they arrive densely and
  on the message thread for VB3-II GUI edits and preset changes; gesture and
  `audioProcessorChanged` callbacks never arrived and must not be relied on) and
  instrument-editor open→close transitions. Hints only bump a **monotonic change-generation
  counter** on the Primary; they never carry state.
* **When must fresh capture occur?** At explicit Save and autosave (already true today —
  Verified); at render enqueue (future, per PI-020 rule 1); and at proxy-staleness evaluation
  when `capturedGeneration < currentGeneration`.
* **While the editor is open:** no continuous capture. Hints accumulate; Auto-mode staleness
  evaluation applies the §18.1 debounce. Measured: VB3-II *does* notify GUI edits, so hints work
  there; but because notification coverage is plugin-specific and non-parameter changes can be
  silent (§10), the editor-open period should still conservatively bump the generation at editor
  open (Proposed conservative default, now supported by the Groove Agent silence evidence).
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
  **Measured: this fallback is required in general** — Groove Agent SE produces a new hash on
  every idle capture (§7). For byte-stable plugins like VB3-II, hash equality at quiescent
  checkpoints is a valid positive short-circuit ("equal ⇒ unchanged"); hash inequality is never
  a change verdict, only "cannot prove unchanged".
* **Save and autosave:** unchanged behavior — both already capture fresh (Verified). The shared
  future contract is "one capture function, called at every checkpoint", which they already
  satisfy.
* **What becomes dirty/undoable:** capture/hash: nothing (rule 6). The user's plugin edit should
  dirty the project once a hint mechanism exists (fixing the Verified gap in §13); it must not
  create musical undo steps (blob-stripping stays).
* **Maximum bounded detection delay:** without hints — until the next checkpoint (Save/enqueue);
  with hints — the §18.1 debounce window (Measured: VB3-II hint latency is milliseconds; the
  binding delay is therefore the chosen debounce policy, not plugin behavior). A hard product
  number remains a steering decision, not a spike result.
* **Thread ownership:** capture, hashing, hint bookkeeping, generation counter: message thread.
  Audio thread: never involved. Render workers: receive an immutable copied blob only.
* **Plugin-specific evidence:** A/B/C/E are measured for VB3-II 2.3.1, plus B (negative) and C
  (silent) data points for Groove Agent SE 5.2.20. Both ends of the spectrum are now observed —
  byte-stable-with-notifications and byte-unstable-without-notifications — and the mechanism is
  safe under both. Broader generalization stays unsupported (§18).

## 18. Known limitations and unsupported claims

* **E2 (MIDI-learn / mode switch) was not performed** — hash-identical to E1, no operator note.
  Open for all plugins.
* **Operator notes were not recorded**: no formal A3 audio-impact note (only a verbal "no audio
  anomalies" from the first, data-lost attempt), no Ctrl+Z observation note, no audio
  device/sample-rate note. No anomaly of any kind was reported, but these attestations are
  informal.
* **Phase labels in session 1 were partially misapplied** (the A1 group contains rows with the
  editor open and with transport playing). No data was lost: every sample records its *actual*
  editor/transport/thread flags, and the analysis in §6–§7 classifies by those flags, not by the
  phase label. Unattributed cross-burst hash transitions (e.g. the A2 group's distinct hash)
  are excluded from conclusions; only within-burst stability and explicitly paired comparisons
  (B3-vs-F1, B4-vs-baseline) are used.
* **The third sanitized report mixes two plugins under phase B3** (the app session continued
  from the Groove Agent mistake; the report format does not print per-row plugin identity). The
  session log (`spike01-capture-log.txt`) records `plugin="…"` per sample and was used to
  disentangle them; §21 presents the split tables. Noted as a harness improvement for any future
  spike (include plugin identity per sample row).
* Audible equivalence is attested by the operator, never inferred from bytes (task rule).
* Runtime findings cover exactly two plugins (VB3-II 2.3.1, Groove Agent SE 5.2.20);
  generalization to other VST3 instruments is explicitly unsupported.
* Debug-build timings overstate production cost; a Release re-run is only warranted if numbers
  ever look borderline (they are ~3 orders of magnitude below checkpoint budgets).
* The CC11/swell attribution for VB3-II's playback-time byte instability is Inference (the
  project sends CC11 and `Swell Pedal` is an exposed parameter); the instability itself is
  Measured.
* The harness cannot observe changes the plugin neither notifies nor serializes; such state is
  untestable from the host by definition (documented limit).
* `spike01LiveInstanceForDiagnostics` intentionally trusts the operator not to unload/replace the
  instrument mid-action; it is not production-hardened (diagnostic scaffolding only).

## 19. Exact proposed steering amendment (for separate human review — NOT applied)

> **Amendment to §9.2 (after the SPIKE-01 requirement list), proposed by SPIKE-01:**
>
> *SPIKE-01 findings (2026-09-05, branch `cursor/spike-01-plugin-state-capture`; repository
> evidence + local measurements: 167 message-thread captures, 230 notifications, VB3-II 2.3.1
> and Groove Agent SE 5.2.20, Debug build; evidence report
> `docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md`):*
> 1. *DAL contains no plugin parameter/processor listeners anywhere (instruments or inserts);
>    live plugin-GUI edits are structurally invisible today and do not dirty the project. The
>    §13 dirty gap means tweak-only sessions may never autosave.*
> 2. *Save and autosave share one fresh-capture path
>    (`buildExperimentalInstrumentProjectBlock` → `getCurrentInstrumentStateBase64`); §9.2 rule 4
>    is Verified for autosave and now Measured at runtime (F1 raw-vs-Save hash MATCH).*
> 3. *The instrument editor-close handler (`editorWindowClosing`, message thread, staleness-
>    guarded) is a safe hint hook; the insert host already implements an editor-close state diff
>    as precedent. Measured: editor open/close alone changes no serialized bytes on VB3-II.*
> 4. *Capture cost is checkpoint-affordable and message-thread-clean (VB3-II ≈0.5 ms / 10 KB;
>    Groove Agent SE ≈5 ms / 148 KB; 100 % message thread, Debug build). Continuous polling
>    stays forbidden.*
> 5. *VB3-II state blobs are byte-stable at rest in all §9.2 cases including
>    save → restart → reload, and revert to baseline bytes when a parameter returns to its
>    original value; they are not stable while playback drives the plugin. Groove Agent SE blobs
>    are never byte-stable (idle counter/clock churn, zero notifications). Therefore: hash
>    equality at quiescent checkpoints is a valid positive "unchanged" proof; hash inequality
>    proves nothing and MUST take the conservative stale path (re-render over silent staleness).*
> 6. *VST3 parameter notifications (VB3-II) arrive densely and on the message thread for GUI
>    edits, and preset changes surface as a full-parameter burst — but no gesture and no
>    `audioProcessorChanged` callbacks were ever observed. Listeners are confirmed as hints
>    only; non-parameter changes can be completely silent.*
> 7. *The authoritative mechanism is measurement-confirmed: fresh capture at checkpoints +
>    monotonic generation counter bumped by hints (notifications, editor open/close) + hash
>    equality only as an opportunistic short-circuit. Capture failure ⇒ keep previous blob,
>    mark proxy stale/unknown.*
>
> **Status change (subject to human review):** PID-001 may be marked **Resolved (VB3-II-class
> evidence)** citing this report, with explicit residual Opens: E2 (MIDI-learn) and
> generalization beyond the two measured plugins.

## 20. Completion status and remaining local actions

### 20.1 Status

* Harness: complete, compiled, selftested (Measured).
* Repository questions (D, F, H + all call chains): answered (Verified).
* Runtime questions (A, B, C, E1, F-confirmation, G-gating): **answered (Measured,
  2026-09-05)** — see §6–§11 and appendix §21. E2 remains Open (§18).
* PID-001: resolution **recommended** (VB3-II-class evidence) — human review decides (§19).

### 20.2 Local procedure, as executed (Windows PowerShell, from the spike worktree)

This is the procedure that was executed on 2026-09-05 (kept for reproducibility; deviations that
occurred are listed in §18). The spike worktree `C:\Users\nicla\development\MiniDAWLab-spike01`
contains the built Debug app. If starting fresh instead: `git fetch origin` and check out
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

Done: the measurement appendix is §21, the raw sanitized reports are committed under
`docs/audits/spike01-measurements/`, and §1/§6–§13/§15/§17 have been updated with the Measured
findings. The PR leaves draft after human review of this report and the §19 amendment.

---

## 21. Measurement appendix (2026-09-05, Debug build)

Raw sanitized reports (committed, generated by the panel, hash/size/timing only):

* `docs/audits/spike01-measurements/2026-09-05-session1-vb3ii-main-run.md` — 147 captures,
  230 notifications (VB3-II).
* `docs/audits/spike01-measurements/2026-09-05-session2-groove-agent-b3.md` — 10 captures
  (Groove Agent SE, wrong-track accident; the report header's "Plugin: Groove Agent SE" is
  correct for these rows).
* `docs/audits/spike01-measurements/2026-09-05-session3-vb3ii-b3-after-reload.md` — cumulative
  20-row B3 table mixing both plugins (see §18); the 10 rows of 10 393 B / hash `875dc9…` are
  the VB3-II B3 measurement.

Full per-action trace: `%APPDATA%\MiniDAWLab\spike01-capture-log.txt` on the operator machine
(records `plugin="…"` per sample; not committed — it contains local absolute paths, no state
bytes).

### 21.1 Per-phase results, disentangled by plugin (from per-sample flags)

| Phase | Plugin | Actual condition (recorded flags) | n | median ms | max ms | bytes | distinct hashes |
|---|---|---|---|---|---|---|---|
| A1 | VB3-II | editor open+closed, stopped (30) / **playing burst (10)** | 41* | 0.48 | 0.97 | 10 433–10 453 | 5* |
| A2 | VB3-II | editor open, stopped | 10 | 0.47 | 0.77 | 10 455 | 1 |
| A3 | VB3-II | editor open, playing | 30 | 0.46 | 0.98 | 10 433 | 1 |
| A4 | VB3-II | repeated unchanged, ~10 s apart | 10 | 0.47 | 0.99 | 10 433 | 1 |
| A5 | VB3-II | immediately after GUI drawbar change | 1 | 0.56 | 0.56 | 10 453 | 1 (new: `33a537…`) |
| B2 | VB3-II | across editor close → reopen | 20 | 0.50 | 0.97 | 10 433 | 1 (= baseline `a58813…`) |
| B4 | VB3-II | drawbar returned to original | 20 | 0.46 | 1.01 | 10 433 | 1 (= baseline `a58813…`) |
| B5 | VB3-II | after transport activity, stopped | 11 | 0.50 | 0.84 | 10 433 | 1 (= baseline `a58813…`) |
| E1 | VB3-II | after preset change | 1 | 0.54 | 0.54 | 10 393 | 1 (new: `875dc9…`) |
| E2 | VB3-II | (no action performed — §18) | 1 | 0.56 | 0.56 | 10 393 | 1 (= E1) |
| F1 | VB3-II | raw + production Save path, back to back | 1+1 | 0.55–0.56 | 0.56 | 10 393 | **1 — MATCH** |
| B3 | Groove Agent SE | idle, editor closed, stopped (accident) | 10 | 4.62 | 6.47 | 148 591–148 791 | **10 — every capture differs** |
| B3 | VB3-II | after save → app restart → project reload | 10 | 0.46 | 1.13 | 10 393 | **1 (= F1 save-time hash `875dc9…`)** |

\* The A1 label was applied across mixed conditions (§18): 31 stopped-state captures were all
hash `a58813…` (stable, editor open and closed alike); the 10-capture burst with
transport=playing during active MIDI/CC produced 4 distinct transient hashes (10 452–10 453 B).
Classification uses the per-sample recorded flags, not the label.

### 21.2 Hash timeline (VB3-II, 12-hex prefixes)

* `a58813…` (10 433 B) — resting baseline; identical across A1-stopped, A3, A4, B2, B4, B5.
* `eb1c91…`/`f36254…`/`f329ab…`/`10d39d…` (10 452–10 453 B) — transient hashes during the
  playing/MIDI burst only; state returned to `a58813…` after stop.
* `5aa0db…` (10 455 B) — A2 group (editor newly opened; transition unattributed, excluded from
  conclusions per §18).
* `33a537…` (10 453 B) — after the A5 drawbar change; reverted to `a58813…` when the drawbar
  was returned (B4).
* `875dc9…` (10 393 B) — after the E1 preset change; confirmed identical at F1 (raw and Save
  path) **and** after full app restart + project reload (B3).

### 21.3 Notification summary (all on message thread)

| Event group | Kind | Count | Detail |
|---|---|---|---|
| Drawbar drag out (22:15:38) | `paramChanged` | ~57 | index 28 `Drawbar Upper A 16'`, 0.01 → 0.76, ~3–7 ms spacing |
| Drawbar drag back (22:16:06) | `paramChanged` | ~27 | 0.75 → 0.00 |
| Preset change (22:17:12–13) | `paramChanged` | 146 | full burst, indices 0–146 (56 absent), ~300 ms |
| Gesture begin/end | — | **0** | never observed |
| `audioProcessorChanged` (any flag) | — | **0** | never observed, incl. at preset change |
| Groove Agent SE (listener attached, idle) | — | 0 | state churn (§7) invisible to listener |

Threading totals: captures 167/167 message thread; notifications 230/230 message thread.
