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

> ## `PARTIAL PASS (SPIKE-01B-M final) — capture layer and byte-stable identity path now fully Measured; volatile-class currency remains hint-based by fundamental observability limit; E2 remains Open`

**SPIKE-01B-M final note (2026-09-06, corrected; full measurement pass in §28):** Groove Agent
SE's churn is present from the very first post-restore capture and grows with **capture count,
not elapsed time** (~+20–30 bytes per past `getStateInformation` call — the serializer observably
perturbs its own next output); in both measured sessions a k=2 probe sufficed to classify it
volatile (M1, §28.2).
For VB3-II the M2 story was **corrected after a schema-inspection error** (§28.0): an earlier
automated run drew MIDI-only source tracks from the wrong JSON path and wrongly concluded the
project had no MIDI and that transport/MIDI does not perturb VB3-II. That claim is **retracted**.
The corrected, delivery-proven run (M2V, §28.3) installs a capture sink at VB3-II's process
boundary and proves the exact destination instance receives 32 note-ons, 32 note-offs and 368
CC11 events across three channels (ch1 authored + ch2/ch3 routed from two MIDI-only tracks); with
delivery proven, dense sampling shows the serialized blob **does vary transiently during active
playback** (9/36 snapshots differed from the at-rest baseline) and, in this session, returned to
the exact authored baseline by the first post-stop capture (no settling window observable at the
~100 ms capture granularity). **Playback capture is therefore unsafe for identity** — confirming
the conservative direction on a proven-delivery basis, not the retracted "unchanged" claim. For
the *render-state* role: background rendering may run concurrently with playback (isolated
instance), but a snapshot beginning a project-start render SHOULD be captured at a
host-observable-quiescence boundary — capture during active MIDI/CC delivery is not yet proven
to provide clean initial conditions, and no clone/restore/render equivalence test was performed
for a playback-time blob (P1D validation obligation, §22.1/§28.7).
M2P (§28.3B) is retained only as a parameter round-trip result and is **not** a substitute for
MIDI/CC testing. PID-001: capture layer Resolved; identity layer remains **Open pending human
review of §23-E/§26**, with the §28.7 corrections applied. M3/E2 (MIDI-learn) remains Open. The
fundamental observability limit (§28.5) stands: DAL can observe *host-observable quiescence*
only (§28.6), never internal plugin state.

**SPIKE-01B corrective note (2026-09-05, supersedes the original PASS verdict below where they
conflict; full analysis in §22–§27):** the measured results stand unchanged and validate the
checkpoint fresh-capture strategy **for byte-stable plugins like VB3-II only**. The original
conclusion was internally incomplete for volatile serializers (Groove-Agent-class): raw state
bytes cannot participate in any validity identity for such plugins (every proxy would be
permanently stale, §23-A), while a bare generation counter cannot by itself rule out unnotified
sound-relevant changes (bounded false-current risk, §23-B). PID-001 therefore must remain
**Open** until the split identity contract in §26 is human-reviewed. Canonical steering revision
4 (merged as PR #3 on the original PASS verdict) marks PID-001 Resolved and is now known to be
premature; the corrective amendment is **Proposed** in §26 and has NOT been applied.

The original SPIKE-01 verdict text follows, preserved for the measured evidence it summarizes:

> `PASS (measured) — checkpoint fresh-capture + generation-counter mechanism confirmed` *(superseded as stated above)*

The diagnostic harness is implemented, compiled, and its pure logic is verified by 17 new
deterministic selftest checks (**Measured**, §16 below: 194 checks at the time of §16; 202 after
the SPIKE-01B-M `spike01b:` delivery-counter tests, 0 failures). All
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

The mechanism sketched in §17 — authoritative fresh capture at checkpoints, listeners and editor
transitions only as generation-counter hints, hash equality only as a positive short-circuit —
survives SPIKE-01B **for the capture layer**, but §17's identity story was underspecified:
SPIKE-01B splits it into three distinct layers (authoritative render state / semantic validity
identity / volatile runtime bytes, §22) and evaluates five identity policies against both failure
modes (§23). Only the hybrid contract (§23-E) covers both **false-current** (silently rendering
stale sound) and **permanent-stale** (a volatile plugin that can never publish a valid proxy).

**PID-001: remains Open.** The §9.2 evidence gate is satisfied for the *capture* questions
(A/B/C/E1/F measured), but the *identity contract* — what token proves a completed render is
still current, for plugins whose bytes never repeat — was not part of the original gate and is a
blocking finding (§22, SPIKE01B-F1). Known residual gaps in addition: E2 (MIDI-learn) was not
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
| G | Fallback for non-byte-stable blobs | design evaluated §17; **re-evaluated in depth by SPIKE-01B (§22–§23)** | Groove Agent SE proves the fallback is required in general; VB3-II shows equality short-circuit works (§7) | **Re-opened by SPIKE-01B — the fallback needs the full split identity contract (§26), not just a counter; blocking for PID-001** |
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

### Recommended mechanism (Proposed — **partially superseded by SPIKE-01B, §22–§26**: the
capture layer below stands; the identity/staleness layer is refined into the three-layer
contract and Policy E, and "confidence: high" is withdrawn for the identity layer)

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

## 19. Original proposed steering amendment (SUPERSEDED — see §26)

> **SPIKE-01B note:** the amendment below was approved by human review on 2026-09-05 and merged
> to `main` as steering revision 4 (PR #3) **before** the SPIKE-01B analysis. Its point 7 and its
> status change ("PID-001 may be marked Resolved") are now known to be premature: they do not
> resolve the volatile-state identity contradiction (§22). The corrective amendment is §26.
> Points 1–6 (the measured findings) remain accurate.

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
* Runtime questions (A, B, C, E1, F-confirmation): **answered (Measured, 2026-09-05)** — see
  §6–§11 and appendix §21. E2 remains Open (§18).
* G (fallback/identity): **re-opened by SPIKE-01B** — the identity contract is a blocking
  finding (§22); resolution path is §26 + the follow-up measurements in §27.
* PID-001: **Open.** Steering revision 4 (which marked it Resolved) requires the corrective
  amendment in §26 — a human review decision, not applied by SPIKE-01B.

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
findings. *(Historical note: the original PR #2 was merged on the PASS verdict; SPIKE-01B
subsequently revised the verdict to PARTIAL PASS — see §22–§27.)*

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

---

# SPIKE-01B — Corrective addendum: the plugin-state identity contract

*Added 2026-09-05 (local, uncommitted pending human review). Scope: architecture validation and
documentation only — no product code, no schema, no renderer, no steering-document change. The
same Verified / Measured / Inference / Open / Proposed classification applies.*

## 22. The blocking finding: one blob cannot serve three roles (SPIKE01B-F1)

SPIKE-01 measured four facts (§6–§10, §21): VB3-II blobs are byte-stable at quiescent
checkpoints; Groove Agent SE blobs differ on every idle capture with growing size and zero
notifications; blobs may change while transport feeds MIDI/CC; listeners are hints, not a record.

The original §17 conclusion implicitly let one artifact — the captured blob (and its hash) — play
three different roles. SPIKE-01B separates them, because the measurements prove no single
artifact can safely serve all three:

1. **Authoritative render state** — the exact bytes captured on the message thread at render
   enqueue and restored via `setStateInformation` into the isolated render instance. Volatility
   does not threaten *identity* here (bytes are never compared in this role), and the capture
   *mechanics* are fully validated (cost §6, Save-path agreement §11) for both plugin classes.
   **What is NOT yet validated (corrected, SPIKE-01B-M):** no clone/restore/reset/render
   equivalence test has been performed for a blob captured during playback; M2V (§28.3) shows
   such a blob may contain transient MIDI/CC-dependent performance state. Whether the isolated
   render instance's lifecycle (restore → prepare → reset/flush as supported → deterministic
   MIDI/CC chase → render from project start) removes those transient initial conditions is a
   **P1D validation obligation**, not a measured fact. See §22.1 for the corrected capture-
   boundary rule.

2. **Semantic validity identity** — the token compared to answer "is this proxy/completed job
   still current?" This is where the contradiction lives:
   * If the token is the raw blob hash (§23-A), a volatile serializer makes **every** proxy
     instantly stale and **every** completed render obsolete at publication — the
     **permanent-stale** failure mode. Measured basis: Groove Agent SE, 10/10 distinct idle
     hashes (§7).
   * If the token is only a host-side generation counter (§23-B), a sound-relevant change that
     produces no notification and no observed lifecycle event is never detected — the
     **false-current** failure mode, which canonical §9.2/PI-020 rule G forbids more strictly
     than the first.
   A safe contract must bound *both* modes at once; §23-E is the only evaluated policy that does.

3. **Volatile/runtime state** — bytes inside the blob that change without any user-visible or
   sound-authoring action: Groove Agent's idle counter/clock churn (Measured), VB3-II's
   transient performance values during active MIDI/CC (Measured; settle back after stop). These
   bytes are **not identifiable from the host side**: JUCE 8.0.4 serializes a VST3 plugin as an
   opaque two-stream XML container (`IComponent` + `IEditController` states,
   `juce_VST3PluginFormat.cpp:3279–3297`, Verified in the vendored JUCE source) with no semantic
   view into either stream. Per the task rule and this evidence, **no generic normalization is
   possible**; volatile bytes can only be *routed around* (role 2 must not depend on them), never
   filtered out.

### 22.1 The active-playback / quiescence problem (explicit definition)

Measured (§7): captures taken while the transport actively drives the instrument with MIDI/CC
differ between adjacent captures (VB3-II: 4 distinct hashes in one 10-capture burst; sizes
10 452–10 453 B vs the 10 433 B resting baseline), then settle back to the resting hash after
stop. Two distinct consequences:

* **For role 1 (render state; corrected by SPIKE-01B-M):** capture during playback is
  *mechanically possible* — `getStateInformation` returns a blob in any transport state, and
  explicit Save already does this in production (§11). But what the measurements prove stops
  there: M2V (§28.3) shows a playback-time blob may contain transient MIDI/CC-dependent
  performance state, and **no clone/restore/reset/render equivalence test was performed for a
  playback-time blob** — it is *not* proven that such a blob provides clean initial conditions
  for a project-start proxy render. The Save precedent does not transfer by itself: Save's blob
  is restored into the *same* instance at load, which is a different contract from seeding an
  isolated render instance. Corrected rules:
  * **Background rendering MAY run concurrently with normal DAL playback** — it uses an
    isolated plugin instance; the transport never needs to stop (or stay stopped) for the
    render itself.
  * **Snapshot capture and background rendering are separate operations.** A snapshot used to
    begin a complete render from project start SHOULD normally be captured at a
    host-observable-quiescence boundary (§28.5). Capturing while the destination is actively
    receiving MIDI/CC is not yet proven to provide clean initial conditions.
  * **P1D must validate the isolated-instance lifecycle:** restore state → prepare →
    reset/flush as supported → deterministic MIDI/CC chase → render from project start. If that
    lifecycle cannot remove performance-transient initial state for a plugin, snapshot capture
    must be deferred until an eligible boundary, or the plugin receives an explicit
    compatibility limitation.
* **For role 2 (validity identity):** hashes of playback-time captures are **meaningless as
  identity evidence** even for byte-stable plugins (Measured). Any byte-equality evidence MUST
  be taken at a quiescent capture boundary — originally defined here as "message thread;
  transport stopped; no parameter/processor notification for the §18.1 debounce window", and
  **corrected by §28.5/§28.7 to `host-observable quiescence`** (no host-sent MIDI/CC + full
  notification silence; transport state is not itself a criterion, and the boundary makes no
  claim about internal plugin quiescence). Captures outside that boundary never contribute
  identity truth.

### 22.2 What DAL/JUCE/plugin APIs actually provide (Verified, JUCE 8.0.4 vendored source)

Investigated for each required capability:

| Capability | Evidence | Verdict |
|---|---|---|
| Distinguish semantic vs volatile bytes | VST3 blob = opaque `IComponent`+`IEditController` streams (`juce_VST3PluginFormat.cpp:3279–3297, 3473+`); no JUCE API exposes structure | **Not possible** from the host; plugin state stays opaque |
| Finer-grained "program-only" state | `AudioProcessor::getCurrentProgramStateInformation` exists but the VST3 wrapper does not override it — default forwards to full `getStateInformation` (`juce_AudioProcessor.cpp:921–924`) | **No help** for VST3 hosting |
| Plugin-announced non-parameter changes | VST3 `IComponentHandler2::setDirty(true)` → JUCE `updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true))` → `audioProcessorChanged` (`juce_VST3PluginFormat.cpp:3961–3967`) | **API path exists and is wired**; Measured: neither tested plugin ever used it (0 events, §8). A hint channel, coverage plugin-dependent |
| Other lifecycle signals | `restartComponent` flags (`kParamValuesChanged`, `kMidiCCAssignmentChanged`, `kLatencyChanged`, …) are handled on the message thread (`juce_VST3PluginFormat.cpp:3950–4002`) and surface as listener callbacks | Additional hint sources; same coverage caveat |
| Safe quiescent boundary | Transport intent + notification silence are both observable host-side (Verified: transport flag recorded per sample; notifications timestamped) | **Definable** (§22.1) without plugin cooperation |
| Compare job vs current state without byte compare | Host-side monotonic generation/revision per Primary (does not exist yet — Proposed §23-B/E); persisted save-pairing restores identity at load *by construction* (`setStateInformation(saved blob)` makes live state = saved state) | **Possible** without touching volatile bytes |

## 23. Policy analysis

Evaluated against both failure modes. "False-stale" = unnecessary re-render (canonically
acceptable, §9.2 rule G); "false-current" = stale sound presented as current (forbidden).

### 23-A. Raw-blob identity (bytes fingerprinted exactly)

* **Correctness guarantee:** equal bytes ⇒ identical restored state ⇒ identical rendered sound
  (assuming deterministic rendering). Sound; but only the *positive* direction exists.
* **False-stale risk:** unbounded for volatile serializers — Groove Agent SE re-fingerprints
  differently on every capture (Measured §7): every proxy is stale the moment it is published.
  Also fires for byte-stable plugins if any capture happens outside quiescence (Measured §22.1).
* **False-current risk:** none (inequality always treated as changed).
* **During playback:** always inequality ⇒ constant false-stale (Measured, VB3-II burst).
* **After Save/load:** VB3-II: works (round trip byte-identical, §7 B3). Volatile plugins: fresh
  capture after load differs from the saved blob ⇒ stale after every load, forever.
* **Publication race:** completed job compares captured hash vs fresh hash — volatile plugins
  fail every time ⇒ **render starvation loop** (enqueue → render → obsolete → enqueue …).
* **Compatibility consequences:** correct for the VB3-II class only; structurally broken for the
  Groove-Agent class (which includes a Steinberg first-party product — not an exotic outlier).
* **Implementation complexity:** lowest (hash + compare).
* **Required tests:** per-plugin idle-churn probe test; save/reload hash round-trip test;
  playback-capture exclusion test.
* **Verdict: rejected as the sole identity.** Usable only as *positive* evidence (see E).

### 23-B. Host revision identity (blob renders; host-managed revision decides currency)

* **Mechanism:** monotonic per-Primary revision, bumped by every observed change source:
  parameter/processor notifications (incl. `nonParameterStateChanged`/`setDirty` when plugins
  send it, §22.2), editor open and close, preset/program operations initiated through DAL,
  plugin replace/reload, project load, state restore, MIDI-mapping changes. A render job stores
  the revision at capture; currency = "revision unchanged since capture".
* **Correctness guarantee:** currency means *no observed change* — exact for every change that
  produces a notification or an observable lifecycle event.
* **False-stale risk:** low-moderate: hints fire only on real interactions; an editor-open bump
  with no actual edit forces one unnecessary re-render (recoverable via E's equality rescue).
* **False-current risk:** **nonzero and unbounded in principle**: a sound-relevant change with no
  notification, made while no lifecycle event is observed (e.g. a plugin applying an external
  config/preset file change, or GUI edits on a plugin that notifies nothing — not observed in
  the tested plugins, but §8 proves coverage is plugin-specific). This is the policy's blind
  spot; unhandled, it violates rule G.
* **During playback:** safe — the revision derives from events, never from bytes.
* **After Save/load:** safe *by construction*: load restores the exact saved blob into the
  instance, so the live state equals the state the proxy was rendered from if the proxy was
  Current at save; persisted pairing (proxy generation ↔ saved state) re-establishes validity
  without any byte comparison of a fresh capture. (Requires persisting the pairing — P1B
  schema concern, out of scope here.)
* **Publication race:** compare stored revision vs current revision at publication — volatile-
  safe, no starvation. A bump during render correctly marks the job obsolete.
* **Compatibility consequences:** works identically for both measured plugin classes.
* **Implementation complexity:** moderate — one counter per Primary + bump plumbing at each
  hint/lifecycle source + persisted pairing.
* **Required tests:** revision bookkeeping unit tests; obsolete-at-publication test; load-pairing
  test; regression: no bump from capture itself (rule 6).
* **Verdict: necessary core, insufficient alone** (blind spot must be bounded by C/D).

### 23-C. Conservative lifecycle invalidation

* **Mechanism:** every observable lifecycle event conservatively invalidates (or forces
  recapture + revision bump): editor open (not just close), preset browser use, plugin
  replace, project load without a persisted pairing, sample-rate/config changes, unload/reload.
* **Correctness guarantee:** eliminates false-current for every change the user makes *through
  an observable surface*. With the editor-open bump, the entire editor-open period counts as
  "possibly changed" regardless of notification coverage.
* **False-stale risk:** highest of all policies — every editor peek without an edit re-renders
  (bounded by E's equality rescue for stable plugins; unbounded for volatile ones).
* **False-current risk (remaining blind spots):** changes with *no* observable surface:
  editor-closed plugin-internal changes (external file watchers, inter-instance sync, hardware
  controllers wired directly to the plugin via its own MIDI input outside DAL's routing).
  DAL-routed MIDI is **not** a blind spot: DAL sends it, so DAL can count it (Verified: routing
  goes through DAL's own merge path, §8.1 of the steering document); whether routed MIDI should
  bump the revision is a steering choice (it marks performance state, usually not authoring).
* **During playback / Save-load / publication / compatibility:** as B (it feeds B's counter).
* **Implementation complexity:** low-moderate (hooks exist: editor open/close handlers Verified
  §9; load/replace paths Verified §3).
* **Required tests:** each lifecycle source bumps exactly once; editor-open→close with no edit
  triggers re-render unless rescued by equality.
* **Verdict: required companion to B**; blind spots must be *named and accepted* (D), not
  silently ignored.

### 23-D. Explicit unsupported/limited classification

* **Mechanism:** classify each plugin instance's state behavior at runtime and surface it.
  **Detection (Measured basis):** at instrument load/restore, on the message thread at a
  quiescent moment, take k (2–3) captures a few ms apart (§6: 1–10 ms total even for a 148 KB
  blob). All equal ⇒ class **byte-stable** (positive-evidence features enabled). Any difference
  ⇒ class **volatile** (equality features disabled). Cache per plugin identity+version;
  re-probe per session. Misclassification risk is **asymmetric, not universally harmless**
  (corrected by SPIKE-01B-M, §28.6): false-volatile (a stable plugin classified volatile) loses
  the equality rescue — unnecessary re-rendering or reduced availability — but never strengthens
  a currency claim; false-stable (a volatile plugin classified stable) may temporarily expose
  the hybrid policy's bounded false-current blind spot until detected. Repeated at-rest byte
  inequality may safely demote stable → volatile; classification must **never** promote
  volatile → stable without a new qualifying probe.
* **User-visible semantics (Proposed):** volatile-class Primaries still render proxies (role 1
  is unaffected), but proxy currency is presented as **hint-based** ("Current (assumed — this
  plugin cannot confirm unchanged state)") rather than byte-verified; byte inequality is never
  shown to the user as "you changed the sound". A stricter P1 variant — Auto-mode proxies
  disabled for volatile plugins, Manual render only — is a steering choice; both variants must
  never claim byte-verified currency.
* **Correctness guarantee:** honesty — the system never asserts stronger validity than the
  plugin's behavior supports.
* **False-stale / false-current:** inherits B+C for volatile plugins; adds no new risk.
* **Playback / Save-load / publication:** as B/C.
* **Compatibility consequences:** every plugin gets *some* proxy support; none gets false
  guarantees.
* **Implementation complexity:** low (k-capture probe + a flag + UI string).
* **Required tests:** probe classification test against a synthetic volatile stub; UI state
  test.
* **Verdict: required transparency layer** for the volatile class.

### 23-E. Hybrid contract (Proposed — the recommended resolution of SPIKE01B-F1)

Combine the validated pieces, each in the only role it is fit for:

1. **Render state (role 1; corrected by SPIKE-01B-M):** a fresh message-thread capture at
   enqueue. A snapshot that begins a complete render from project start SHOULD normally be
   captured at a host-observable-quiescence boundary (§22.1/§28.5): capture while the
   destination is actively receiving MIDI/CC is not yet proven to provide clean initial
   conditions, pending P1D's isolated-instance lifecycle validation (restore → prepare →
   reset/flush as supported → deterministic MIDI/CC chase → render from start). Background
   rendering itself may run concurrently with playback on the isolated instance; the transport
   never needs to stop for the render.
2. **Validity identity (role 2):** the host revision (B) + conservative lifecycle bumps (C) +
   persisted save-pairing for load. Never raw bytes.
3. **Positive byte evidence (stable class only):** at quiescent boundaries (§22.1), if a fresh
   capture's hash equals the job's captured hash, the revision bump that triggered re-evaluation
   is *rescued* — the proxy is proven still current and the re-render is cancelled. Equality is
   the only byte verdict that exists; inequality is silent (no user-facing meaning).
4. **Classification (D):** k-capture probe selects stable vs volatile; volatile plugins run
   without feature 3 and with hint-based currency presentation.
5. **Publication:** revision compare only; feature-3 equality may additionally rescue a
   stable-class job whose revision bumped mid-render.

* **Correctness guarantee:** false-current bounded by C's named blind spots (explicitly
  accepted, user-visible via D); permanent-stale impossible (no byte compare on the critical
  path; volatile plugins publish via revision compare like everyone else).
* **False-stale risk:** bounded — worst case one unnecessary re-render per editor-open cycle for
  volatile plugins; rescued by equality for stable plugins.
* **False-current risk:** only C's blind-spot residue; strictly smaller than B alone.
* **During playback:** background rendering explicitly allowed (isolated instance; the
  transport need not stop or stay stopped); *snapshot capture* for a project-start render is
  deferred to a host-observable-quiescence boundary unless P1D validates playback-time capture
  for the plugin (§22.1); identity untouched (no byte evidence collected).
* **After Save/load:** validity restored by pairing, both classes.
* **Publication race:** revision compare, starvation-free; equality rescue is an optimization,
  never a requirement.
* **Compatibility consequences:** full support for stable class; honest, slightly more
  re-render-prone support for volatile class.
* **Implementation complexity:** the sum of B+C+D plus the rescue rule — each piece individually
  small; the composition is the cost.
* **Required tests:** all of B/C/D's tests plus: equality-rescue happy path; volatile plugin
  never rescued; playback captures never enter identity records; end-to-end no-starvation test
  with a synthetic always-differing stub.

## 24. (Reserved — merged into §22.2 to keep the API evidence beside the finding.)

## 25. Diagnostic-code review (all SPIKE-01 code now on `main`)

Reviewed files: `src/diagnostics/Spike01StateCapturePanel.h/.cpp`, `Spike01Sha256.h`,
`Spike01ReportFormat.h`, the two accessors in `src/plugins/ExperimentalInstrumentHost.h/.cpp`,
the flag hook in `src/Main.cpp:275–286`, the forwarding chain
(`MainWindow`, `TransportControlsShortcutTarget.h`, `MainAppWindow.cpp`), the CMake registration,
and the selftest block in `tests/selftest/MiniDAWSelftestsMain.cpp`.

* **Inertness without the flag: Verified.** The only product-path reference is the
  `commandLine.contains("--spike01-state-capture")` branch in `Main.cpp` (Verified by search:
  no other call sites construct the panel). Without the flag: no panel, no session-log file, no
  listener, no captures. The two host accessors compile into the product but have no callers
  outside the panel. `MainAppWindow` holds a null `unique_ptr` and one extra include.
* **Live-instance raw pointer (`spike01LiveInstanceForDiagnostics` → `attachedInstance_`):
  bounded diagnostic risk, zero product risk.** The panel stores the raw
  `juce::AudioPluginInstance*` while the listener is attached. Detach re-validates through the
  host's `asyncAliveGuard()` + a fresh host resolve + pointer-identity comparison (no deref)
  before calling `removeListener` (`Spike01StateCapturePanel.cpp:417–438`, Verified). Residual
  hazard: if the operator unloads/replaces the instrument *while attached*, the instance is
  destroyed with the listener still registered; the stale pointer is never dereferenced
  afterwards (identity compare only), but callbacks arriving during teardown are theoretically
  possible. Same-thread execution (all lifecycle on the message thread) makes this a
  narrow-window, operator-triggered condition — acceptable for flag-gated scaffolding, expressly
  not production-hardened (already documented in §18).
* **Listener side effects:** `addListener` mutates the live instance's listener list — a real
  (if tiny) interaction with a production object. Only reachable via the panel. No evidence of
  cost or behavior change while attached (Measured: A/B phases ran with listener attached with
  no anomalies).
* **Session log:** written only when the panel exists; sanitized (sizes/hashes/metadata);
  location `%APPDATA%\MiniDAWLab\spike01-capture-log.txt`. No product interaction.
* **Selftest additions:** pure-function checks; no product risk.

**Disposition recommendation (Proposed; do not execute in SPIKE-01B):**

1. **Retain through the SPIKE-01B follow-up measurements** (§27 needs the panel for the
   volatile-restore probe and the quiescence-settling probe) and through SPIKE-02 planning
   (parts — SHA-256 helper, report format — are directly reusable).
2. Then remove in one dedicated cleanup commit: delete `src/diagnostics/Spike01*.*`, the two
   `spike01*ForDiagnostics` accessors, the `Main.cpp` flag branch, the
   `startSpike01StateCaptureProbe` / `invokeStartSpike01StateCaptureProbeFromStartup` forwarding
   chain and the panel member in `MainAppWindow`, the CMake source line, and the
   `spike01:` selftest block (or move `Spike01Sha256.h`/`Spike01ReportFormat.h` under a shared
   diagnostics-support name if SPIKE-02 adopts them).
3. If retention extends past SPIKE-02: move compilation behind a CMake option
   (e.g. `MINIDAW_ENABLE_SPIKE_DIAGNOSTICS`, default OFF for packaging presets) so release
   binaries cannot contain the scaffolding even inert.

## 26. Corrective proposed steering amendment (Proposed — NOT applied; supersedes §19)

> **Corrective amendment to §9.2 and PID-001 (steering revision 5, proposed by SPIKE-01B):**
>
> 1. *Revision 4's status change is narrowed: SPIKE-01's measured findings (§9.2 findings block
>    points 1–6) stand; point 7's mechanism confirmation and the "PID-001 Resolved" status are
>    withdrawn. **PID-001's capture layer is Resolved; its identity layer is Open** pending
>    review of the split identity contract below.*
> 2. *Identity contract (three roles): (a) the authoritative render state is a fresh
>    message-thread capture at enqueue; a snapshot that begins a complete render from project
>    start SHOULD normally be captured at a host-observable-quiescence boundary, because capture
>    while the destination is actively receiving MIDI/CC is not yet proven to provide clean
>    initial conditions — P1D must validate the isolated-instance lifecycle (restore → prepare →
>    reset/flush as supported → deterministic MIDI/CC chase → render from project start), and a
>    plugin for which that lifecycle cannot remove performance-transient initial state gets
>    deferred capture or an explicit compatibility limitation. Background rendering may run
>    concurrently with normal playback on the isolated instance — the transport never needs to
>    stop for the render; (b) semantic validity identity
>    is a host-managed monotonic revision per Primary — bumped by parameter/processor
>    notifications, editor open and close, preset operations, plugin replace, load/restore —
>    plus a persisted proxy↔saved-state pairing that restores validity at load by construction;
>    (c) raw state bytes NEVER serve as validity identity. Byte-hash equality taken at a
>    quiescent boundary (transport stopped, notification-silent for the debounce window) is
>    admissible only as positive "still current" evidence for plugins classified byte-stable;
>    byte inequality has no semantic meaning anywhere.*
> 3. *Plugin classification: at load/restore, a k-capture quiescent probe classifies each
>    instance byte-stable or volatile. Misclassification risk is asymmetric, not universally
>    harmless: false-volatile loses the equality rescue (unnecessary re-rendering or reduced
>    availability) but never strengthens a currency claim; false-stable may temporarily expose
>    the bounded false-current blind spot. Repeated at-rest inequality may safely demote
>    stable → volatile; promotion volatile → stable requires a new qualifying probe. Volatile
>    plugins render proxies normally but present hint-based currency; they are never blocked
>    from publication by byte comparison.*
> 4. *Publication/obsolete checks compare revisions, never bytes (equality may rescue
>    stable-class jobs). This bounds both failure modes: false-current is limited to the named
>    lifecycle blind spots (accepted, user-visible), permanent-stale is structurally impossible.*
> 5. *Residual Opens carried forward: E2 (MIDI-learn), plugins beyond the two measured, and the
>    steering choice between hint-based Auto proxies vs Manual-only for volatile plugins in P1.*
>
> **Status change (subject to human review):** PID-001 → **Open (identity layer) / Resolved
> (capture layer)**; steering document header notes revision 5 correcting revision 4.

## 27. Smallest next slice and completion status

*(2026-09-05 late evening: SPIKE-01B-M has since been executed — see §28. The "no new code"
constraint below was consciously waived by the operator, who requested unattended automation;
the deviation and its scope are documented in §28.1 and §28.8.)*

**Recommended smallest next step — SPIKE-01B-M, a ~15-minute follow-up measurement with the
existing panel (no new code):**

1. *Volatile restore probe:* load a project containing Groove Agent SE; immediately at load
   (quiescent), capture ×3. Question: does the churn exist from the first post-restore capture
   (expected yes)? This pins the k-capture classification probe's reliability at the exact
   moment it would run in production.
2. *Quiescence settling probe:* VB3-II — play with active CC, stop, capture ×3 immediately and
   ×3 after ~2 s. Question: how quickly does the state settle to the resting hash (bounds the
   quiescent-boundary debounce)?
3. *(Optional)* E2 attempt: one MIDI-learn or mode switch in VB3-II with listener attached.

After SPIKE-01B-M (or a decision to skip it): human review of §23-E + §26; apply steering
revision 5; then the first implementation slice for the mechanism is the **revision counter +
lifecycle bump plumbing** (policy B+C core) as a P1B/P1C prerequisite — small, product-visible
only through proxy code that does not exist yet.

**SPIKE-01B completion report:**

* Files inspected: the two steering/audit documents, all three measurement reports, the seven
  diagnostic/product files listed in §25, the vendored JUCE 8.0.4 sources
  (`juce_VST3PluginFormat.cpp`, `juce_AudioProcessor.h/.cpp`, `juce_VST3Common.h`).
* Files changed: **this document only** (verdict, §15-G, §17 banner, §19 banner, §20.1, new
  §22–§27).
* Tests/builds run: none — no code was changed; the 194-check selftest result from SPIKE-01
  (§16) is unaffected.
* Production behavior modified: **no.**
* Git state: changes are local and uncommitted on `main` (per task instruction: no commit, push,
  or PR until the findings are reviewed). The steering document (revision 4) is intentionally
  untouched; §26 carries the corrective amendment as Proposed.

---

# SPIKE-01B-M — Final targeted measurement pass (2026-09-05; corrected 2026-09-06)

## 28. SPIKE-01B-M: unattended measurements M1/M2/M3

### 28.0 Correction record: schema-inspection error and its cause (2026-09-06)

An earlier pass of this section (dated 2026-09-05, now rewritten below) asserted that
`TSE_pt2.dalproj` "contains no arranged MIDI clips at all." **That claim is false and is fully
retracted.** Root cause: the project was inspected with an ad-hoc PowerShell `ConvertFrom-Json`
query that looked for clips/notes under each entry of the top-level `tracks` array. In DAL's real
on-disk schema (`src/io/ProjectFile.cpp`), instrument-lane musical content lives in a **separate
top-level array, `experimentalInstrumentTracks`**, keyed by `trackId`; the `tracks` array carries
only routing/identity for the classic lanes. Notes are stored as `timelineNotes` (tick-based:
`midiNote`/`velocity`/`offVelocity`/`channel`/`startTick`/`durationTicks`), and continuous
controllers as `ccPoints` (`startTick`/`controller`/`value`/`channel`/`interp`). Querying the
wrong array returned empty note/CC lists, and a second bug (PowerShell counting a scalar/null as
one element) masked the emptiness. The corrected inspection was cross-checked against DAL's
runtime scheduling code (§28.0.2), not left to the ad-hoc query.

Everything the earlier pass concluded *from that false premise* is invalidated: specifically the
claim that transport/scheduled MIDI does not perturb VB3-II state (the old §28.3 "control
finding") and the framing of session 5 (`M2`) as a valid control. See §28.3 (rewritten) and the
INVALID banner on `2026-09-05-session5-m2-vb3ii-transport-only.md`.

#### 28.0.1 Verified project MIDI topology (real schema, cross-checked)

Destination instrument: **trackId 7, name "Organ", instrument VB3-II** (`instrumentKind`
generic VST3; `output` → master trackId 2). Three channels of MIDI converge on this one instance:

| Source | `tracks` kind | `midiChannel` (output remap) | routing | clip content (ticks @ tpq 960, 180 bpm) |
|---|---|---|---|---|
| trackId 7 "Organ" (own lane) | instrument | 1 | self | 8 `timelineNotes` ch1, ticks 960–11520; 3 `ccPoints` **CC11** ch1, `interp=linear` (expression ramp) |
| trackId 8 "Organ Lower" | **midi** | 2 | `midiTo: 7` | 3 `timelineNotes` ch2, ticks 5760–10560 |
| trackId 9 "Organ pedal" | **midi** | 3 | `midiTo: 7` | 5 `timelineNotes` ch3, ticks 960–11520 |

So: **16 notes/cycle across three effective channels (1, 2, 3)** plus a linear **CC11** expression
ramp on channel 1, with channels 2 and 3 routed in from two MIDI-only source tracks via
`midiTo`. On the JSON `midiChannel` field: it is each track's **output-channel remap**
(`Track::midiOutputChannel`, `ProjectFile.cpp:169–174/1670–1675`; absent = `Any`/preserve each
note's own channel, `1…16` = force that channel on send — remap logic baked at
`InstrumentTrackController.cpp:2334`). Here it is set to 1/2/3, so the effective delivered
channel is forced to 1/2/3 (and coincides with the stored note channels). The **effective**
channels 1/2/3 are what matters and are proven delivered by the M2V counters (§28.3); the
mechanism is a forced remap, not "preserve native." (Cross-check by
[Map MIDI scheduling to instrument hosts](54a06019-b203-4f39-bf97-ee40d3b1d98a) and re-verified in
`ProjectFile.cpp`.) Tick range 960–11520 = quarter-notes 1–12 ≈ **0.33 s – 4.0 s**; clip
`lengthSamples` 240000 (5 s at 48 kHz). The project's saved cycle is
`0 … rightLocatorSamples 288000` (6 s), `cycleEnabled=true`; saved `playheadSamples` 249600
(5.2 s). Playing from 0 with the cycle armed therefore encounters all clips; the corrected run
seeks to sample 0 explicitly via `Transport::requestSeek(0)` (the Stop button also seeks to
sample 0, not the left locator — immaterial here since the left locator is 0).

#### 28.0.2 Cross-check against DAL runtime scheduling (Verified)

The single many-to-one MIDI choke point is
`ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs`
(`src/plugins/ExperimentalInstrumentHost.cpp:3488`, audio thread). It merges UI-enqueued MIDI and
transport MIDI (`rtBlockMidi_`, filled per event by `audioThread_addMidiEventForCurrentBlock`,
line 1704 — the message already carries its channel; no remap) into one `juce::MidiBuffer blockMidi`,
then calls `inst.processBlock(view, blockMidi)` at line 3575 on `owner->inst`. The same
`activeOwner_->inst` is what `spike01LiveInstanceForDiagnostics()` returns (line 2087/2101), so a
sink observing this boundary observes exactly the buffer the diagnosed instance receives. A
pre-existing test seam — `MidiDeliveryCaptureSink` + `installMidiDeliveryCaptureSinkForTests`
(header lines 153–166) — is invoked at line 3531, *before* the plugin's `processBlock`, once per
block; that is the mechanism §28.3 uses to prove delivery.

### 28.1 Deviation record and exact procedure (Measured)

The task specification said to document panel limitations rather than extend the panel. The
operator explicitly requested unattended automation instead ("Jag hinner inte lägga så mycket
testningstid. Kan du automatisera det?"), which was executed as a conscious, recorded deviation.
The extension is confined to the same flag-gated scaffolding chain reviewed in §25:

* `Spike01StateCapturePanel.h/.cpp`: a `juce::Timer`-driven auto-run state machine
  (`--spike01-auto=<plan>`, plans `M1X`, `M1Y`, `M2`, `M2O`, `M2P`, `M2V`), track selection by
  label substring, custom-phase setting, a parameter wiggle/revert helper
  (`setValueNotifyingHost` — the same notify-host path the UI uses), a **MIDI-delivery capture
  sink** (`spike01::MidiDeliveryCounters` in `Spike01MidiDeliveryCounters.h` — RT-safe atomics,
  raw-byte parsing, selftest-covered — bound to the pre-existing
  `ExperimentalInstrumentHost::MidiDeliveryCaptureSink` interface by a thin `MidiSinkAdapter` in
  the panel), and four optional callbacks
  (`startTransport`/`stopTransport`/`seekTransport`/`readCycleWrapCount`).
* `MainAppWindow.cpp`: the four transport callbacks, wired to
  `TransportPlayPauseStopController::togglePlayPauseFromUi()` / `stopOrSeekFromStopButton()` and
  `Transport::requestSeek` / `readCycleWrapCountForUi` — the exact code paths the transport
  strip uses.
* `Main.cpp`, `MainWindow.h/.cpp`, `TransportControlsShortcutTarget.h`: the `autoPlanId`
  string threaded through the existing forwarding chain.

The sink is installed only during an M2V run, cleared in the plan's final step, and cleared
defensively on abort and in `~Content()` so the audio thread never holds a dangling pointer.
Nothing runs without both `--spike01-state-capture` and `--spike01-auto=`; the operator-driven
panel is unchanged in behavior. Product behavior without the flags: unchanged.

#### 28.1.1 Diagnostic-code review record (2026-09-06, pre-PR)

* **Inactive without flags:** the panel exists only when `--spike01-state-capture` is on the
  command line (`Main.cpp:280`), and the auto-run state machine additionally requires
  `--spike01-auto=<plan>`. Without both, no timer, no sink, no transport callbacks run.
* **No product behavior introduced:** all new logic lives in the flag-gated panel; the
  `MainAppWindow` callbacks are bound only when the panel is created and call the same transport
  entry points the UI strip uses. The host-side sink hook (`midiCaptureSink_`, atomic
  pointer checked once per block, `ExperimentalInstrumentHost.cpp:3501/3531`) is pre-existing
  test infrastructure.
* **Audio-thread safety:** `spike01::MidiDeliveryCounters::countBlock` classifies events from
  the raw `meta.data` bytes — no `juce::MidiMessage` construction, hence no allocation even for
  SysEx; fixed-size relaxed atomics; no locks, no file I/O, no state serialization. All 72 blob
  captures of session 9 executed on the message thread (sanitized report, Threading section).
* **Bounded storage:** the counter set is a fixed struct (8 scalars + 16-slot histogram); no
  growth with run length.
* **No invalid plugin pointer can be retained:** the sink holds no plugin pointer at all.
  `m2vInstanceAtInstall_` is a `const void*` recorded for the log and never dereferenced;
  instance identity is established structurally (§28.0.2: sink boundary and
  `spike01LiveInstanceForDiagnostics()` resolve the same `activeOwner_->inst`), not by
  dereferencing a diagnostic pointer on the audio thread.
* **Lifecycle:** sink cleared in the plan's final step, on abort, and in the panel destructor;
  the host clears it independently on teardown.
* **Clean removability:** the whole chain (auto-run machinery, counters header, adapter,
  selftest, callbacks, `autoPlanId` threading) is enumerated in §28.8 for a single cleanup
  commit; the pre-existing sink seam stays.

Unattended sessions were launched against the operator's real project (`TSE_pt2.dalproj`, 7
instrument tracks — passed on the command line through the existing Explorer-open path) on the
Debug build, each in a fresh process, each auto-writing a sanitized report on completion:

| Session | Plan | Window | Status | Purpose |
|---|---|---|---|---|
| 4 | `M1X` | 09-05 23:34 | valid | Groove Agent SE, **untouched-first**: captures at 60 s, 120 s |
| 5 | `M2` | 09-05 23:37 | **INVALID** (§28.0) | VB3-II trackId 4; no MIDI delivery; wrongly read as "transport doesn't perturb" — retracted |
| 6 | `M1Y` | 09-05 23:38 | valid | Groove Agent SE, **capture-heavy**: bursts at ≈0/30/60/120 s |
| 7 | `M2P` | 09-05 23:44 | valid (param only) | VB3-II parameter wiggle (Volume 0.5040→0.2540→0.5040); **not** a MIDI/CC test |
| 8 | `M2O` | 09-06 00:08 | preliminary | VB3-II "Organ" trackId 7, real MIDI, **no delivery counters** — superseded by session 9 |
| 9 | `M2V` | 09-06 00:21 | **valid, delivery-proven** | VB3-II "Organ" trackId 7; sink proves note/CC delivery; dense play sampling + post-stop cadence |

Timer-scheduled capture points hit their targets within ≤10 ms (per-sample ISO timestamps in
the sanitized session log). Sanitized outputs: `docs/audits/spike01-measurements/`
`2026-09-05-session4…7-*.md` and `2026-09-06-session8…9-*.md` plus the incremental session log.

### 28.2 M1 — Groove Agent SE volatility probe (Measured)

First-of-burst blob size against elapsed time and prior capture count:

| Session | Burst | Elapsed since load | Prior captures | First-capture bytes |
|---|---|---|---|---|
| M1X | 60 s | ~60 s | 0 | 148 561 |
| M1Y | 0 s | ~0 s | 0 | 148 601 |
| M1X | 120 s | ~120 s | 10 | 148 788 |
| M1Y | 30 s | ~30 s | 10 | 148 821 |
| M1Y | 60 s | ~60 s | 20 | 149 100 |
| M1Y | 120 s | ~120 s | 30 | 149 267 |

* **Churn exists from the very first post-restore capture** in both sessions: all 10 hashes in
  every burst are distinct, and consecutive captures 3–15 ms apart already differ. A **k=2
  probe classified Groove Agent SE volatile in every measured burst**; no tested settling
  interval changed this (60 s and 120 s untouched behaved the same as 0 s).
* **Growth correlates with capture count, not elapsed time.** With prior-capture count held
  equal, first-of-burst sizes match within ~40 bytes regardless of whether 0, 30, 60 or 120
  idle seconds passed; each additional past capture adds ~20–30 bytes. Untouched idle time
  contributes little (+15–25 bytes per 30–60 s window at most, same order as one capture).
* **`getStateInformation` observably perturbs the plugin's own next serialized output**
  (self-perturbation / observation effect). Consequences: (a) capture is not idempotent for
  volatile plugins — even a Save changes the next byte image, so bytes can never be compared
  across saves either; (b) the side effect is harmless **to the proposed validity-identity
  algorithm** (volatile bytes are excluded from identity), but **sonic equivalence of the
  successively growing blobs was not measured** — this result must not be generalized into an
  audible-equivalence claim, and the number of diagnostic/production captures should therefore
  remain minimal; (c) no capture-frequency policy can make a volatile plugin byte-comparable.

### 28.3 M2 — corrected, delivery-proven VB3-II playback probe (M2V, Measured)

The corrected run drives the **real arranged MIDI** of §28.0.1 into VB3-II (trackId 7 "Organ"),
seeks the playhead to 0, plays with the cycle armed, samples the serialized blob densely during
playback (every ~250 ms for ~9 s, past one cycle wrap) and again after stop
(+0/100/250/500/1000/2000 ms, then two late bursts), with the delivery sink installed at the
process boundary throughout.

**Delivery proof (sink counters, session 9 — the decisive numbers).** The destination instance
pointer recorded at sink-install (`spike01LiveInstanceForDiagnostics()` on trackId 7) is the
instance whose `processBlock` the counted buffer feeds (§28.0.2), and the boundary block counter
advanced 187 → 2447 during the run. Merged-buffer contents actually delivered:

| Metric | Value |
|---|---|
| Note-On / Note-Off | **32 / 32** (balanced) |
| CC total / of which CC11 | **384 / 368** |
| Channel histogram | **ch1=401, ch2=13, ch3=21, ch4–16=1 each** — histogram total **448** |
| Blocks with MIDI / total boundary blocks | 327 / 2210 |
| First / last event (abs samples since install) | 88 000 / 555 360; cycle wraps = 1 |
| other (non-note/non-CC) | 0 |

**Counter reconciliation — all 448 counted messages accounted for.** The by-type totals and the
channel histogram are two complete views of the same event set and agree exactly:

```
by type:     32 noteOn + 32 noteOff + 384 CC + 0 other                    = 448
by channel:  401 (ch1) + 13 (ch2) + 21 (ch3) + 13×1 (ch4…ch16)           = 448

ch1     = 16 noteOn + 16 noteOff + 368 CC11 + 1 CC123                     = 401
ch2     =  6 noteOn +  6 noteOff            + 1 CC123                     =  13
ch3     = 10 noteOn + 10 noteOff            + 1 CC123                     =  21
ch4–16  = 1 CC123 each                                                    =  13

CC      = 368 CC11 (expression ramp, ch1) + 16 CC123 (one per channel)    = 384
notes   = (8 + 3 + 5) arranged notes × 2 laps (cycleWraps = 1)            = 32 on / 32 off
```

The 16 non-CC11 controller events are **proven, not inferred**: on transport stop,
`InstrumentTrackController::audioThread_flushTransportMidi`
(`src/instruments/InstrumentTrackController.cpp:2538–2541`) sends
`juce::MidiMessage::allNotesOff(c)` — **CC 123**, a 0xBn controller event — on every channel
c = 1…16. Those land in the CC total and put exactly one event on each of ch1–ch16, which is why
ch1 shows 401 (not 400) and ch4–16 show 1 each despite carrying no arranged music. A summary that
adds only ch1+ch2+ch3 gets 435 and appears 13 short; the "missing" 13 are precisely the ch4–16
all-notes-off flush. No non-channel messages were delivered (`other = 0`), and the histogram
total equals the type total, so no event escaped channel attribution.

After the run, the counter implementation was hardened without changing classification
semantics: extracted to `src/diagnostics/Spike01MidiDeliveryCounters.h`
(`spike01::MidiDeliveryCounters`, raw-byte status parsing — no `juce::MidiMessage` construction,
so provably allocation-free on the audio thread even for SysEx), given an explicit `channelless`
counter so the identity `noteOn + noteOff + cc + other == Σ channelHist + channelless` is
structural, and covered by a deterministic selftest (`testSpike01MidiDeliveryCounters`,
including the CC123/velocity-0/SysEx edge cases). For the ≤3-byte channel messages of session 9
the counting is byte-for-byte equivalent to the code that produced the numbers above.

This proves scheduled events reached the exact VB3-II instance from **all three sources**: the
Organ lane (ch1 notes + the CC11 expression ramp), "Organ Lower" (ch2, routed via `midiTo`), and
"Organ pedal" (ch3, routed via `midiTo`). CC11 delivery is proven (368 events). Merely observing
that transport was running is *not* what is claimed here — the buffer handed to the plugin was
counted.

**State effect under proven delivery (this is the corrected finding).** With delivery proven and
the blob sampled densely during playback:

* **During playback the serialized blob varies transiently.** Of 36 dense play-phase captures,
  **27 equalled the at-rest authored baseline `875dc964caa6…` (10 393 bytes) and 9 differed**
  (10 413 / 10 415 bytes, distinct hashes such as `dad4f30b4a51…`, `40ee6a58fae1…`,
  `09ba00347f3a…`). VB3-II therefore **does fold active performance state into
  `getStateInformation` while notes/expression are live** — playback perturbs the observable
  byte image. (The earlier automated run at trackId 4 saw one constant hash only because no MIDI
  reached it — §28.0; that non-result is retracted, not evidence of stability.)
* **On stop it returned to the exact authored baseline by the first post-stop capture** in this
  session. All 6 post-stop captures (+0 … +2000 ms) and all 20 late captures are
  `875dc964caa6…` — the same hash as the pre-play baseline and as the original SPIKE-01 baseline
  (§1). No settling window is observable at the ~100 ms capture granularity of this run; a
  slower-settling plugin (or a slower VB3-II code path not exercised here) would need its own
  measurement, which is why the §26/§28.7 debounce is sized from evidence per plugin rather than
  assumed zero.
* **Notifications fired concurrently:** 156–368 `paramChanged` events during the run (the CC11
  expression stream surfaced as parameter-change notifications). So this is explicitly **not** a
  notification-silent case: host-observable activity and blob variation coincided, and the blob
  returned to baseline once the host stopped sending events.

**Consequences.** Capturing plugin state **during playback is unsafe for identity** — the blob
does not equal the authored baseline while performance state is live, so a during-playback
capture could be mistaken for a distinct authored state. This confirms the conservative direction
of the original §7 caveat ("not byte-stable while playback actively drives it") on a
**proven-delivery** basis and **retracts** the earlier automated claim that transport/MIDI leaves
VB3-II unchanged. The safe capture boundary is host-observable quiescence (§28.6): transport
stopped **and** no host-sent MIDI/CC **and** notification-silent for a debounce window.

### 28.3B M2P — parameter round-trip (session 7; retained, scope-limited)

M2P remains a *separate* result and is **not** presented as a substitute for MIDI/CC playback
testing. It measured only a deterministic automatable parameter wiggle through the UI notify-host
path on trackId 4 VB3-II:

* Baseline ×10 `f19a23677e…`; perturbed (Volume 0.5040→0.2540) `482a3aa086…` (identical across the
  two measured passes — in this session the byte image behaved as a deterministic function of the
  parameter value); revert to 0.5040 returned to exactly `f19a23677e…` by the first capture ≤5 ms
  later; all post-revert captures identical. Exactly 4 `paramChanged` events, correct
  index/name/value, all on the message thread.

This shows that for VB3-II, in the measured sessions, an *authored-parameter* change was
deterministic and fully reversible in the blob, and that its restore→serialize round trip was
byte-deterministic across OS processes (session-7 baseline matched an earlier session on
identical semantic state). It says **nothing** about how
received MIDI/CC affects state — that is what M2V (§28.3) measures. Parameter-wiggle behavior does
**not** predict MIDI/CC behavior; indeed the two differ (a reverted parameter returns to baseline
with the transport stopped, whereas live MIDI perturbs the blob only while playing).

### 28.4 M3 — MIDI-learn / non-parameter control attempt (Not performed; E2 stays Open)

MIDI-learn requires interacting with the plugin's native editor UI (right-click context
gestures inside the vendor GUI) plus hardware CC input — not automatable through any existing
DAL surface without simulating input into the plugin's own window, which was judged unsafe and
out of scope. **E2 remains Open**, unchanged from §10/§18. M2V now proves the *received-MIDI/CC*
path end to end, and M2P proves the *authored-parameter* path; the still-unmeasured E2 class is
specifically **silent non-parameter authored state** (no note, no CC, no parameter notification,
no dirty flag) — exactly the fundamental observability limit of §28.5.

### 28.5 The fundamental observability limit and `host-observable quiescence`

Define **`host-observable quiescence`** = the host has sent no MIDI/CC to the instance, received
no parameter-change notifications, and received no dirty-state / non-parameter-state
notifications, for a debounce window. This is the *only* kind of quiescence DAL can establish.
It is **not** the same as the plugin being internally at rest: a plugin may continue changing
its own state (release tails, LFOs, internal counters, deferred housekeeping) while the host
observes complete silence.

If a plugin changes sound-relevant authored state internally and (a) changes no observable
parameter, (b) does not set VST3 dirty state, and (c) gives no other notification, **DAL cannot
detect that change with certainty without interpreting opaque plugin state.** SPIKE-01B-M makes
the limit concrete on both sides: Groove Agent SE shows silent byte churn with zero
notifications (bytes change with no semantic event), and its self-perturbation shows the
converse (the observation itself manufactures byte differences), so byte inequality can never be
promoted to a semantic signal for the volatile class. VB3-II shows a third shape: proven MIDI/CC
delivery perturbs the blob *while playing* and it returns to baseline on stop (§28.3) — but DAL
learns "safe to trust the blob" only from **host-observable quiescence**, never from any direct
read of the plugin's internal activity. This limit is a property of the plugin API surface, not
of any DAL policy, and no conservative-invalidation scheme removes it; conservative bumps only
*narrow* the accepted blind spots (§23-C/§23-E). **We do not claim notification silence proves
quiescence, and we do not claim DAL can observe internal plugin activity.**

### 28.6 Effect on the required interpretation points and policies A–E

* **k-capture probe safety/reliability:** validated for classification. k=2 sufficed for
  GA-class volatility; the VB3-II bursts show no false-volatile flakiness *at rest*. Probe
  self-perturbation is harmless to the proposed validity-identity algorithm (volatile bytes are
  excluded from identity, §28.2), but its sonic equivalence was not measured, so capture counts
  stay minimal. Residual: a plugin volatile only *outside* the probe window would be
  misclassified stable — detectable later as repeated at-rest inequality; §28.7 adds an optional
  demotion rule.
* **Misclassification risk direction (asymmetric — neither direction universally harmless):**
  false-volatile loses the equality rescue and may cause unnecessary re-rendering or reduced
  availability, but never strengthens a currency claim. False-stable may temporarily expose the
  hybrid policy's bounded false-current blind spot — the same bound as §23-C's blind spots,
  since equality is only ever *positive* evidence. Repeated at-rest inequality may safely
  demote stable → volatile; promotion volatile → stable requires a new qualifying probe.
* **Playback capture (corrected):** a `getStateInformation` call is mechanically possible in
  any transport state, but a **during-playback capture must never be admitted as byte identity
  evidence** — M2V proves the blob differs from the authored baseline while MIDI/CC is live
  (§28.3). For the *render-state* role, a snapshot beginning a project-start render SHOULD be
  captured at a host-observable-quiescence boundary until P1D validates the isolated-instance
  lifecycle (§22.1); no clone/restore/reset/render equivalence test was performed for a
  playback-time blob. Background rendering itself remains allowed during playback (isolated
  instance). Byte *equality* evidence is admissible only at a **host-observable quiescence**
  boundary, not merely "transport stopped."
* **Quiescence definition (corrected):** must be expressed as host-observable quiescence
  (§28.5), i.e. no host-sent MIDI/CC **and** notification silence for the debounce window —
  transport state alone is neither necessary nor sufficient. The debounce is a generic host-side
  window; it bounds only what the host can see and makes **no** claim about internal settling.
  The earlier "≤5 ms settling" observation belongs to the M2P *parameter round-trip* (§28.3B)
  and must not be generalized to MIDI/CC or to internal plugin state.
* **Policies:** A (raw-blob identity) is further condemned — GA can never satisfy it, and by
  self-perturbation even a Save invalidates it. B/C bounds unchanged. D's probe is now Measured
  rather than Proposed-reliable. **E (hybrid) survives and is still the recommended contract**;
  its rule-G guarantee holds *only within the explicitly accepted blind spots* of §28.5, and
  only if byte evidence is gated on host-observable quiescence and never collected during
  playback. Volatile plugins must not be presented as proven-Current in P1 (hint-based
  presentation stays mandatory; the P1 Auto-vs-Manual steering choice remains a residual Open).

### 28.7 Required corrections to the proposed revision 5 amendment (§26)

1. **Quiescent boundary (amendment point 2c):** replace "transport stopped, notification-silent
   for the debounce window" with "**`host-observable quiescence`: the host has sent no MIDI/CC to
   the instance, and received no parameter/dirty/non-parameter-state notifications, for the
   debounce window. Transport state is not itself a criterion. Host-observable quiescence does
   NOT imply the plugin is internally at rest — it bounds only what the host can see.**" Byte
   equality taken at such a boundary is admissible as positive "still current" evidence for
   byte-stable-classified plugins only; a capture taken while MIDI/CC is being delivered is never
   admissible as identity evidence (Measured: M2V, §28.3).
2. **Render-state snapshot boundary (amendment point 2a):** the amendment must state that
   snapshot capture and background rendering are separate operations; that background rendering
   may run concurrently with normal playback (isolated instance — the transport never needs to
   stop for the render); that a snapshot beginning a complete render from project start SHOULD
   normally be captured at a host-observable-quiescence boundary, because capture during active
   MIDI/CC delivery to the destination is not yet proven to provide clean initial conditions
   (no clone/restore/reset/render equivalence test was performed for a playback-time blob); and
   that P1D must validate the isolated-instance lifecycle (restore → prepare → reset/flush as
   supported → deterministic MIDI/CC chase → render from project start), deferring capture or
   declaring an explicit compatibility limitation for plugins where the lifecycle cannot remove
   performance-transient initial state. Save-time capture remains a distinct operation: Save may
   capture during playback as today, but that precedent does not by itself prove suitability as
   the initial state of a project-start proxy render.
3. **Probe note (amendment point 3):** add "the classification probe itself may perturb a
   volatile plugin's subsequent bytes (Measured on Groove Agent SE); this is harmless to the
   validity-identity algorithm because bytes never serve as identity for the volatile class —
   but sonic equivalence of the successively growing blobs was not measured, so the result must
   not be generalized into an audible-equivalence claim and capture counts (diagnostic or
   production) should remain minimal. Optionally, repeated at-rest byte inequality on a
   stable-classified instance may demote it to volatile (a safe direction); promotion
   volatile → stable requires a new qualifying probe."
4. **Debounce sizing:** the debounce window is a host-side safety margin, not a measured
   internal-settling time. The ≤5 ms figure applies only to the M2P parameter round-trip and
   must not be cited for MIDI/CC or internal state; 250 ms is a reasonable default pending
   broader plugin coverage.
5. **New explicit non-claims (must appear in the amendment):** the steering text must state that
   DAL (a) cannot observe internal plugin quiescence, (b) does not treat notification silence as
   proof of quiescence, and (c) distinguishes host-observable quiescence from unknowable internal
   plugin activity. Currency for any plugin is therefore "current as far as the host can observe,"
   never an absolute guarantee.
6. Otherwise the §26 text stands. **PID-001 status recommendation unchanged: capture layer
   Resolved, identity layer Open until §23-E/§26 (with these corrections) passes human review.**
   SPIKE-01B-M removes the last *measurement* obstacles for the byte-stable and volatile classes;
   what remains is a steering decision (accepting the blind spots and the volatile-class
   presentation), plus the named residual Opens (E2; plugin coverage).

### 28.8 Diagnostic scaffolding cleanup recommendation (updated)

§25's disposition stands, extended: the auto-run machinery (`buildAutoPlan`,
`timerCallback`-state machine, perturb/revert helpers, `Spike01MidiDeliveryCounters.h` with the
panel's `MidiSinkAdapter`, the `testSpike01MidiDeliveryCounters` selftest, the four
transport callbacks and the `autoPlanId` threading) joins the same single cleanup commit. The
`MidiDeliveryCaptureSink` seam it uses is pre-existing product-test infrastructure and stays.
Retention until SPIKE-02 planning concludes is still recommended because the SHA-256 / report /
auto-run / delivery-counter pieces are directly reusable for a future regression probe. The
M3/E2 limitation (native-editor interaction not automatable) is a documented panel limitation,
not a reason to extend further.

### 28.9 Verdict impact

The verdict **remains PARTIAL PASS**. The correction does not lower it: it *removes an
over-claim* (the retracted "transport/MIDI leaves VB3-II unchanged") and *replaces it with a
stronger, delivery-proven finding* (playback perturbs the blob; capture during playback is
unsafe for identity; byte-equality evidence is admissible only at host-observable quiescence,
and a project-start render snapshot should be captured there pending P1D validation, §22.1/§28.7
point 2). The §9.2 capture-layer gate is
unaffected. What changed inside the PARTIAL PASS: the byte-stable class's safe-capture rule is
now correctly conditioned on host-observable quiescence rather than "transport stopped," and the
identity layer's Open status is unchanged pending human review of §23-E/§26 with the §28.7
corrections. E2 remains a named residual Open.

### 28.10 SPIKE-01B-M completion report (corrected)

* **Schema-inspection error:** documented in §28.0 (queried `tracks` instead of the top-level
  `experimentalInstrumentTracks`; a scalar/null counted as one element masked it). Cause and fix
  recorded; the false "no MIDI clips" claim retracted everywhere (§1, §28.0, §28.3, session-5
  banner).
* **Invalidated earlier interpretation:** session 5 (`M2`, trackId 4, no delivery proof) marked
  INVALID/Inconclusive; its raw measurements preserved as evidence only, not used for any
  conclusion.
* **Verified project MIDI topology:** §28.0.1 — destination trackId 7 "Organ" (VB3-II); own-lane
  ch1 (8 notes + CC11 ramp) plus ch2 (3 notes, from MIDI-only "Organ Lower", `midiTo:7`) and ch3
  (5 notes, from MIDI-only "Organ pedal", `midiTo:7`); tick range 960–11520 (~0.33–4.0 s);
  cycle 0–288000 samples. Cross-checked against DAL's runtime choke point (§28.0.2).
* **Corrected M2 procedure and results:** §28.3 (M2V) — seek to 0, cycle-armed playback, dense
  in-play sampling + post-stop cadence, delivery sink installed throughout.
* **Exact proof MIDI/CC reached VB3-II:** sink counters on the destination instance (trackId 7,
  pointer recorded at install): **Note-On 32, Note-Off 32, CC 384 (CC11 368), channels
  ch1=401/ch2=13/ch3=21/ch4–16=1 each**, blocks-with-MIDI 327, boundary blocks 187→2447,
  first/last event 88000/555360 samples, 1 cycle wrap. All 448 messages reconcile exactly
  (by-type total = histogram total; equation in §28.3 — the 16 non-CC11 controllers are the
  proven CC123 all-notes-off stop flush, one per channel). Same-instance confirmation via
  §28.0.2 (sink boundary and `spike01LiveInstanceForDiagnostics()` resolve the identical
  `activeOwner_->inst`).
* **Verdict change:** none (remains PARTIAL PASS); rationale in §28.9.
* **Files changed:** this document (§1 note, §27 pointer, §28 incl. new §28.0/§28.3/§28.3B/
  §28.9/§28.10 and corrected §28.5–§28.7);
  `src/diagnostics/Spike01StateCapturePanel.h/.cpp`,
  `src/diagnostics/Spike01MidiDeliveryCounters.h` (extracted, hardened counter),
  `tests/selftest/MiniDAWSelftestsMain.cpp` (focused counter selftests), `src/app/MainAppWindow.cpp`,
  `src/app/MainWindow.h/.cpp`, `src/app/TransportControlsShortcutTarget.h`, `src/Main.cpp`
  (auto-run mode + delivery sink); six sanitized measurement outputs under
  `docs/audits/spike01-measurements/` (sessions 4–9). The canonical steering document and the
  operator's version/release changes (`CMakeLists.txt`, `installer/MiniDAWLab.iss`) are untouched.
* **Tests/builds run:** four Debug rebuilds via `scripts/build-windows.ps1` (all OK);
  `MiniDAWSelftests` rebuilt and run — final state **202 checks, 0 failures** (194 from §16 plus
  8 new `spike01b:` delivery-counter checks, including the reconciliation identity).
* **Measurement sessions:** six unattended sessions (M1X, M2, M1Y, M2P, M2O, M2V) plus one
  invalidated (M2); all COMPLETE; zero operator interaction after launch.
* **Remaining open questions:** E2 (silent non-parameter authored state); plugin coverage beyond
  VB3-II and Groove Agent SE; the P1 presentation choice for volatile plugins; human review +
  application of steering revision 5 with the §28.7 corrections.
* **Git state:** the SPIKE-01B/01B-M changes (this report, the diagnostic/automation code, the
  delivery-counter header + selftests, the sanitized measurement outputs) are committed on the
  dedicated review branch `spike/spike-01b-measured` and opened as a **draft PR against `main`
  — DO NOT MERGE BEFORE HUMAN REVIEW**. Nothing is merged. The operator's unrelated
  version/release changes (`CMakeLists.txt`, `installer/MiniDAWLab.iss`) and the canonical
  steering document remain uncommitted/untouched.
