# SPIKE-02 — Isolated render-instance lifecycle, offline throughput, playback contention, latency and tail policy

**Date:** 2026-09-06
**Status:** Measurement report — evidence for PID-004/PID-005 lock review and P1D unblocking
**Steering references:** `docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md` (revision 5 Canonical) §9.4, §14, §15, §21 PID-004/PID-005, §22 P0/P1A/P1D; tests T-09, T-10, T-15, T-16, T-29, T-31
**Prior evidence:** `docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md` (SPIKE-01/01B/01B-M)

---

## 1. Executive verdict: **PASS**

All seven automated measurement plans (S2A–S2G) completed unattended and produced the
evidence this spike was scoped to collect:

- The isolated render-instance lifecycle (capture → create → restore → prepare on the
  message thread; `processBlock` exclusively on one dedicated low-priority worker;
  release → destroy back on the message thread) works for a real VST3 instrument
  (VB3-II) and a deterministic synthetic instrument, with thread/instance identity
  logged at every step (§3).
- Offline throughput is more than sufficient: **~32× realtime** for VB3-II at 48 kHz in a
  **Debug** build, essentially independent of block size (256/512/1024) and of the
  `setNonRealtime` flag (§4–§5). The speed-neutrality of `setNonRealtime` is a result for
  the measured VB3-II instance, not a general guarantee for other plugins. A real
  ten-minute render including 32-bit-float stereo WAV writing took **18.6 s** (§6).
- Concurrent rendering during live transport playback caused **no measurable degradation**
  of the audio callback (0 overruns, 0 near-overruns, mean budget unchanged at ~4 %)
  even with the render worker running unthrottled at ~34× realtime (§7). Recommended P1
  resource policy: **render unthrottled at low thread priority; recording remains a pause
  condition** (§8).
- The P1D snapshot lifecycle (`restore → prepare → reset → deterministic MIDI/CC chase →
  render from start`) produced a render from a **mid-MIDI/CC-delivery snapshot** that is
  materially equivalent (max 0.41 dB per-second RMS delta) to the render from a
  host-observable-quiescence snapshot; omitting reset/chase measurably worsened the
  match (1.63 dB max delta) (§9). This is evidence for the measured VB3-II scenario —
  not a universal, deterministic or bit-exact equivalence claim for arbitrary plugins;
  P1D validation remains a per-plugin gate.
- Latency preservation verified exactly with a synthetic instrument of known 333-sample
  latency across all block sizes and offline states; VB3-II advertises 0 samples (§10).
- Both tested plugins have idle noise floors far below every candidate silence threshold
  (VB3-II −149 dBFS peak, Groove Agent SE digital silence), so the locked absolute-peak
  tail structure is feasible; every candidate combination completed without
  misclassification (§11). The 72 successful combinations are evidence for the measured
  plugins and sessions, not a universal plugin-compatibility claim — a plugin with a
  higher idle floor still fails at the cap and receives a compatibility limitation.
  Recommended values: **X = −70 dBFS, Y = 1.0 s, Z = 30 s** (§12). **No steering
  amendment is required** (§17).

One architectural hazard was found and matters for P1 implementation: a render harness
that sizes its process buffer to the main stereo pair only will **crash** with multi-bus
instruments (VB3-II reports `totalOut=4`); the buffer must span
`max(2, totalNumInputChannels, totalNumOutputChannels)` channels exactly as the product's
own scratch allocation does (§14, hazard H1).

Scope guard: this spike produced **no product renderer, no schema change, no fingerprint,
no proxy files, no queueing, no playback substitution, no UI, no packaging**, and the
canonical steering document was not modified.

## 2. Tested environment and plugins

| Item | Value |
|---|---|
| OS | Windows 11 Home |
| CPU | Intel Core Ultra 9 275HX |
| DAL build | 0.9.0, **Debug** (ninja-debug, MSVC); throughput numbers are therefore conservative |
| Host path | JUCE VST3 hosting (`AudioPluginFormatManager` + `VST3PluginFormat`), same as product |
| Sample rate / render blocks | 48 000 Hz; 256/512/1024-sample render blocks |
| Live audio device during contention runs | 48 000 Hz, 480-sample callback blocks |
| Test project | The operator's TSE_pt2 project (same project as SPIKE-01B-M; Organ = VB3-II destination, trackId 7; Groove Agent SE on trackId 3). Musical contents untouched. |
| Plugins measured | VB3-II (VST3, `totalOut=4`, mainOut=2); Groove Agent SE (VST3) as second real plugin |
| Synthetic instrument | `spike02::SyntheticLatencyInstrument` — fixed 333-sample reported latency, deterministic impulse at noteOn+latency |

The render workload is a fixed diagnostic organ pattern replicating the verified TSE_pt2
Organ **topology** (SPIKE-01B-M §28.0: channel-1 chords + CC11 expression, channel-2
lower-manual line, channel-3 pedal, tiled 4-second pattern). The exact musical phrase was
deliberately **not** copied from the project (diagnostics carry no musical content); the
pattern keeps the instrument active on all three channels with the same event types and
comparable density (360 MIDI-carrying blocks per rendered minute).

## 3. Lifecycle, thread and instance evidence (S2A)

All lifecycle claims below are backed by per-step log lines carrying thread IDs and
instance pointers (`spike02-render-log.txt`, plan S2A).

- **Instance isolation:** live VB3-II instance and render instance logged as different
  pointers (`identity: … DIFFERENT (required)`); after the render, the live pointer was
  unchanged and the live host still had its instrument.
- **Thread affinity:** `captureStateToSlot`, `createIsolatedPlugin`, state restore,
  `prepareToPlay`, `teardownIsolated` (releaseResources + destroy) all logged
  `(message)`; every `processBlock` ran on a distinct dedicated worker thread ID
  (one per job), started at `juce::Thread::Priority::low`.
- **State transfer:** live state captured on the message thread through the same
  `getStateInformation` path Save uses (10 393 bytes; SHA-256 `875dc9…` — byte-identical
  to the at-rest state measured in SPIKE-01B-M, reconfirming VB3-II's at-rest byte
  stability); restore into the isolated instance took 2–4 ms; instance creation ~220–246
  ms; `prepareToPlay` ~110–136 ms.
- **Live playback independence:** during the 30-s isolated render the transport kept
  playing (cycle wrap count advanced; `transportStillPlaying=true`), the audio callback
  stayed at mean 5.3 % / max 12.6 % of budget with **0 overruns**.
- **Cancellation:** a paced synthetic render cancelled between blocks in **6.8 ms**; the
  VB3-II contention renders cancelled in **0.13 ms / 1.37 ms** (request-to-worker-exit).
- **Clean shutdown:** teardown logs confirm releaseResources+destroy on the message
  thread for every instance; the panel abort path cancels any in-flight worker and the
  controller destructor joins it before teardown (exercised implicitly by design; normal
  completion path exercised in every plan).
- **No editor:** `createEditor` is never called; every teardown logged
  `editorPresentAtTeardown=false`.
- **Latency-position proof (synthetic):** noteOn at sample 48 000 produced the first
  nonzero output sample at exactly **48 333** (= noteOn + 333), confirming the harness
  neither trims nor shifts leading latency.

## 4. Offline / non-realtime behavior

`setNonRealtime` is currently **not used anywhere in product code** (confirmed by search;
the existing mixdown renders live instances behind an offline gate instead). For VB3-II,
`setNonRealtime(true)` vs `false` made **no measurable throughput or output difference**
(≈32–35× realtime both ways; output peak/RMS equivalent). The flag was applied on the
message thread before `prepareToPlay` in every run and is harmless; P1 SHOULD still set
`setNonRealtime(true)` on render instances as a correctness signal for plugins that do
change behavior offline, but must not rely on it for speed.

Output validity was never judged by byte equality (VB3-II is not bit-deterministic):
every run checked duration processed, all-samples-finite, first/last nonzero positions,
peak/RMS levels, and MIDI-carrying block counts. All 9 matrix runs produced valid
non-silent output (peak ≈ −12 dBFS, mean RMS ≈ −30 dBFS, `allFinite=true`).

## 5. Block-size comparison (S2B; VB3-II, 60 s audio per run, Debug build)

| Mode | Block | Realtime factor | Wall ms | Block ms med / p95 / max | Peak dBFS | Valid output |
|---|---:|---:|---:|---|---:|---|
| nonRealtime=false, unpaced | 256 | 33.1 | 1 812 | 0.153 / 0.200 / 1.598 | −12.1 | yes |
| nonRealtime=false, unpaced | 512 | 34.7 | 1 728 | 0.290 / 0.399 / 0.764 | −12.5 | yes |
| nonRealtime=false, unpaced | 1024 | 30.4 | 1 971 | 0.640 / 0.911 / 2.344 | −11.7 | yes |
| nonRealtime=true, unpaced | 256 | 32.1 | 1 872 | 0.149 / 0.220 / 1.016 | −12.0 | yes |
| **nonRealtime=true, unpaced** | **512** | **32.9** | **1 826** | **0.297 / 0.420 / 1.181** | **−12.9** | **yes** |
| nonRealtime=true, unpaced | 1024 | 33.6 | 1 786 | 0.591 / 0.787 / 1.648 | −11.6 | yes |
| realtime-paced | 256 | 1.00 | 60 000 | 0.209 / 0.360 / 3.067 | −12.6 | yes |
| realtime-paced | 512 | 1.00 | 60 000 | 0.446 / 0.679 / 2.185 | −12.7 | yes |
| realtime-paced | 1024 | 1.00 | 60 000 | 0.715 / 1.090 / 5.785 | −11.5 | yes |

Block size is throughput-neutral within noise for VB3-II. **512 remains the P1 candidate**
(no evidence favors another value; per-block cost med 0.30 ms at 512 leaves the largest
scheduling flexibility per unit of bookkeeping). Reported plugin latency was 0 in all
runs and did not change with block size or offline state.

## 6. Actual ten-minute render benchmark (S2C)

Configuration: VB3-II, 512-sample blocks, `setNonRealtime(true)`, unpaced, organ pattern
active for the full duration (3 600 MIDI-carrying blocks), 32-bit-float stereo WAV
written to the system temp directory during the render.

| Plugin | Block | Offline | Audio duration | Wall time | Realtime factor |
|---|---:|---:|---:|---:|---:|
| VB3-II | 512 | true | 600.0 s | 18.63 s | 32.2× |

- Worker CPU time 18.16 s (≈97 % of wall — the job is CPU-bound in the plugin, not I/O).
- WAV-writing contribution: **1.12 s** of the 18.63 s (~6 %), measured around the write
  calls; file size **230 400 104 bytes** (≈220 MB, exactly 600 s × 48 kHz × 2 ch × 4 B +
  header). The artifact was deleted after metrics were computed.
- Added tail time, measured separately by feeding silence after the pattern: with the
  recommended policy (X=−70 dBFS, Y=1 s) the tail completed **0.68 s** after the final
  event (decision at 1.69 s, i.e. ~0.05 s extra wall time at 32×).
- Predicted wall time with the realtime-paced fallback: **≈600 s + ~2 s decision** ≈ 10 min
  (validated by the paced 60-s runs holding exactly 1.00×).

## 7. Playback-contention results (S2D)

Live transport playback of the full project through the live instances, while the
isolated VB3-II render instance processed on the low-priority worker. Audio callback
metrics from the engine's always-on load window (`snapshotAudioCallbackLoadAndReset`),
UI responsiveness from a 50 ms message-thread timer-jitter probe.

| Phase (45 s observation; baseline 30 s) | Callback mean / max budget % | Near-overruns / overruns | Render RT factor | UI jitter mean / p95 / max ms |
|---|---|---|---:|---|
| D1: playback alone | 4.3 / 10.6 | 0 / 0 | — | 51.1 / 57.0 / 98.3 |
| D2: playback + unpaced render | 4.2 / 9.3 | 0 / 0 | **34.3** | 51.1 / 60.3 / 64.2 |
| D3: playback + yielding render (3 ms per 4 blocks) | 4.6 / 10.9 | 0 / 0 | 9.8 | 51.4 / 60.8 / 69.2 |

- Live output remained continuous in all phases (0 overruns/near-overruns; transport
  playing throughout; callback budget statistically unchanged vs baseline).
- The unpaced worker processed 25.7 minutes of audio during its 45-s window while
  consuming ~98 % of one core (44.3 s CPU / 45 s wall) at low priority.
- Cooperative yielding cut throughput 3.5× while buying no measurable callback headroom
  on this machine.

## 8. Recommended P1 resource policy

**Render unthrottled at low thread priority during playback.** Evidence: unpaced
low-priority rendering produced zero callback degradation and zero dropouts while
tripling+ the effective throughput of the yielding variant. Cooperative yielding remains
a trivially available knob (`yieldEveryBlocks`/`yieldMs` in this harness) if slower
machines show contention, but the measured machine gives no reason to pay its cost.
**Recording MUST remain a pause condition** (locked steering §14; not contradicted by any
measurement here). No hard CPU-percentage cap: plugins may create internal threads, so an
in-process percentage cap cannot be guaranteed (steering-aligned; unchanged).

## 9. Snapshot initial-condition result (S2E; P1D lifecycle, T-31)

- Snapshot **Q** captured at host-observable quiescence (transport stopped; 10 393 bytes,
  SHA-256 `875dc9…`).
- Snapshot **M** captured while the destination was **provably receiving MIDI/CC**
  (delivery counters at capture: 15 note-ons, 12 note-offs, 120 CCs — all CC11 — on
  channels 1/2/3; `duringTransportPlaying=true`; 10 413 bytes, different hash — the
  transient-byte behavior documented in SPIKE-01B-M).
- Both snapshots were rendered from "project start" through the identical P1D lifecycle:
  restore → prepare → `reset()` → deterministic chase prefix (per channel: CC64=0,
  CC120, CC121, CC123, then the initial CC11 value) → 30-s organ-pattern render.
- Result: **materially equivalent** — per-second RMS profiles differ by max **0.41 dB**
  (mean 0.18 dB) across all 30 seconds, within run-to-run variation for this
  non-bit-deterministic instrument.
- Control render of M **without** reset/chase: max delta **1.63 dB** (4× worse), i.e. the
  reset/flush/chase stage does real, measurable work and must not be skipped.

Conclusion: for VB3-II, a snapshot captured during active MIDI/CC delivery, passed
through the full P1D lifecycle, yields project-start renders materially equivalent to
quiescent-capture renders. This **validates the P1D lifecycle for this plugin** at
per-second RMS granularity. It does **not** prove sample-exact equivalence, does not
generalize to other plugins without their own validation, and does not weaken the
steering position that project-start snapshots SHOULD normally be captured at
host-observable-quiescence boundaries — it demonstrates the required fallback works when
that boundary is unavailable, for this plugin.

## 10. Latency results and preservation verdict (S2F)

Synthetic instrument with known fixed 333-sample latency, all 6 configs
(256/512/1024 × nonRealtime on/off):

- `getLatencySamples` reported **333 before and after processing in every config** —
  changing block size or offline state cannot silently change the recorded value.
- First nonzero output sample at exactly **48 333** in every config (noteOn at 48 000 +
  333): leading latency preserved; no trimming or shifting; rendered length exactly the
  requested block count (563/282/141 blocks for 3 s).
- The harness metadata (`RenderResult.latencySamplesAtStart/AtEnd`) represents the value
  losslessly — a proxy artifact can carry it.

VB3-II advertised latency: **0 samples** in all 12 prepared configurations (S2B/S2F).
No physical latency was inferred from note attack time (out of scope). Verdict: the
locked P1 policy — **latency preservation, not PDC** — is implementable as specified;
preservation was verified, PDC was neither attempted nor needed.

## 11. Idle-noise-floor and tail measurements (S2G)

Idle floor (5 s, no MIDI, after restore+prepare):

| Plugin | Idle max peak | Idle mean RMS |
|---|---:|---:|
| VB3-II | **−149.3 dBFS** | −184.9 dBFS |
| Groove Agent SE | **digital silence** (−∞; no nonzero sample) | −∞ |

Tail runs: deterministic phrase (VB3-II: chord ch1 + pedal ch3 + CC11; GA: two drum
hits), explicit note-offs + CC64=0 as the final event, then silence-fed processing for
60 s with per-block peak/RMS recording (10.7 ms blocks). Full 36-combination candidate
grid evaluated per plugin (4 thresholds × 3 windows × 3 caps); representative rows:

| Plugin | X (dBFS) | Y (s) | Tail sec | Decision sec | Peak after decision | Rose again | Verdict |
|---|---:|---:|---:|---:|---:|---|---|
| VB3-II | −60 | 1.0 | 0.47 | 1.47 | −85.7 dB | no | completed |
| VB3-II | −70 | 0.5 | 0.90 | 1.40 | −84.6 dB | no | completed |
| VB3-II | **−70** | **1.0** | **0.90** | **1.90** | **−93.6 dB** | **no** | **completed** |
| VB3-II | −70 | 2.0 | 0.90 | 2.90 | −116.6 dB | no | completed |
| VB3-II | −80 | 1.0 | 1.29 | 2.29 | −102.1 dB | no | completed |
| VB3-II | −90 | 1.0 | 1.70 | 2.70 | −113.5 dB | no | completed |
| Groove Agent SE | −70 | 1.0 | 0.94 | 1.94 | −126.3 dB | no | completed |
| Groove Agent SE | −90 | 1.0 | 1.14 | 2.14 | −128.1 dB | no | completed |

Every one of the 72 evaluated combinations (36 × 2 plugins) **completed** within even the
smallest 15-s cap; no combination failed at a cap, no combination misclassified
persistent noise as a tail, and no post-decision material ever rose back above its
threshold. Neither plugin's idle noise floor comes anywhere near the candidate
thresholds, so the locked absolute-peak structure is feasible for both — no evidence
against the steering structure was found (the RMS/relative-detection question does not
need to be opened).

## 12. Recommended tail-policy values

- **X (silence threshold): −70 dBFS absolute per-block peak.** −60 clips audible
  reverb-class tails too eagerly in general; −80/−90 buy nothing for these plugins but
  push closer to plausible idle floors of noisier third-party instruments.
- **Y (required continuous window): 1.0 s.** 0.5 s completed everywhere but is one echo
  gap away from a false decision for delay-based plugins; 2.0 s adds a second of render
  for no measured benefit.
- **Z (maximum tail): 30 s.** Both measured plugins finish in <3 s; 15 s would also have
  passed, but 30 s covers long-reverb instruments without the pathological worst case of
  60 s.
- **Policy when the idle noise floor exceeds X:** the render reaches Z materially
  non-silent → **Failed, never publishes** (locked semantics unchanged), and the plugin
  receives an explicit per-plugin compatibility limitation. Neither tested plugin
  triggers this.
- **Fingerprint/tail-policy version consequence:** X/Y/Z are policy inputs of the
  rendered artifact; they belong in the tail-policy version field so that changing them
  later invalidates proxies through the version bump, not through silent behavioral
  drift. Recommended initial value: tail-policy v1 = (−70 dBFS, 1.0 s, 30 s).

## 13. Realtime-fallback recommendation

The realtime-paced mode held exactly 1.00× in all three block sizes with negligible CPU
(median block cost unchanged). Keep it as the defensive fallback for plugins that
misbehave when processed faster than realtime, selectable per plugin as a compatibility
limitation. Predicted cost: wall time ≈ audio duration + tail decision (for the measured
project: ~10 min for the 10-minute render vs 18.6 s unpaced). It should never be the
default given the measured 32× headroom.

## 14. Architectural hazards and plugin-compatibility limitations

- **H1 — multi-bus process buffers (found the hard way):** the first S2A attempt crashed
  the app inside VB3-II's `processBlock` because the harness passed a 2-channel buffer to
  a plugin with `totalOut=4`. The product's own scratch rule
  (`jmax(2, getTotalNumOutputChannels())`, see `ExperimentalInstrumentHost` scratch
  alloc and the Phase-C drum probe) must be applied to every render instance:
  **the process buffer must span `max(2, totalIn, totalOut)` channels**; the proxy
  artifact then reads only the main stereo pair. The P1 renderer MUST inherit this rule.
- **H2 — `setNonRealtime` is a no-op for speed** on the measured plugins; treat it as a
  correctness signal only (§4).
- **H3 — Debug-build numbers are conservative:** all throughput figures come from an
  unoptimized Debug build; Release will be faster. No conclusion here depends on the
  absolute number, only on the ~32× margin.
- **H4 — VB3-II advertises 0-sample latency**; latency preservation therefore has no
  observable effect for this plugin, and the preservation proof rests on the synthetic
  instrument (§10). Fine for P1; plugins with real latency get it preserved by
  construction.
- **H5 — non-bit-determinism:** VB3-II output varies at sample level between identical
  runs; all equivalence judgments in this spike use duration/finiteness/energy/level
  profiles, and any future proxy-validation tooling must do the same.
- **H6 — Groove Agent SE event path:** GA was measured through generic note-on/off
  excitation (the same note range the product's Phase-C drum probe plays into
  state-copied clones). Its own arranged-content path was not exercised; the GA numbers
  support tail/idle-floor conclusions, not scheduling conclusions.

## 15. Can PID-005 numeric values be locked?

**Yes — evidence supports locking** X=−70 dBFS, Y=1.0 s, Z=30 s (with the Failed-at-cap
semantics and the compatibility-limitation escape hatch already Locked structurally).
Both measured plugins complete under every candidate; the recommendation picks the middle
of the safe region rather than an edge. Lock should happen in the next steering revision
after human review of this report, recording the values as tail-policy v1.

## 16. Is P1D unblocked?

**Yes.** The full P1D lifecycle was executed and validated end-to-end for VB3-II,
including the hard case (snapshot captured during proven MIDI/CC delivery) — §9. The
per-plugin validation obligation stands: P1D remains a per-plugin gate (a plugin whose
mid-delivery snapshots fail the equivalence check gets deferred capture or a
compatibility limitation, per steering §9.4.4), but nothing blocks starting the P1D
implementation slice.

## 17. Proposed steering amendment

**None required.** The locked tail structure (absolute peak threshold, continuous window,
cap, Failed-at-cap) is feasible as specified (§11). The only steering-adjacent action is
the PID-005 numeric lock (§15), which is a value fill-in inside the already-locked
structure, plus optionally recording the H1 buffer rule in the render-instance section
when P1 implementation begins.

## 18. Diagnostic-scaffolding cleanup disposition

New SPIKE-02 scaffolding (all diagnostic-only, inactive without `--spike01-state-capture
--spike01-auto=S2*`, removable cleanly):

- `src/diagnostics/Spike02RenderHarness.h` — harness (delete file).
- `src/diagnostics/Spike02TailAndStats.h` — pure evaluators (delete file; also delete the
  two `testSpike02*` selftests that include it).
- `src/diagnostics/Spike01StateCapturePanel.cpp/.h` — S2 plan builder + wait-probe +
  jitter probe + `Spike01AudioLoadStats` (revert the S2 sections; the SPIKE-01 panel
  machinery is unchanged by removal).
- `src/app/MainAppWindow.cpp` — the `snapshotAudioLoad` callback wiring (delete the
  lambda).
- `build/run-spike02-plan.ps1` — launcher script in the untracked build directory (not
  part of the source tree).

Recommended disposition: keep until PID-005 is locked and the P1D slice is planned (the
harness is the cheapest way to re-measure another plugin), then remove together with the
SPIKE-01 scaffolding per the SPIKE-01 report §28.8 plan. No product path references any
of it; the app builds and behaves identically without the flags.

## 19. Files inspected and changed

Inspected (read-only): `docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md` (§9.4, §14, §15, §21,
§22, T-09/T-10/T-15/T-16/T-29/T-31), `docs/audits/PORTABLE_INSTRUMENTS_ARCHITECTURE_AUDIT.md`
(§6 offline rendering, §7 lifecycle/threading), `docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md`,
`src/app/AudioMixdownExporter.cpp`, `src/engine/PlaybackEngine.{h,cpp}`,
`src/plugins/ExperimentalInstrumentHost.{h,cpp}` (create/state/prepare/drum-probe/scratch
paths), `src/io/MonoWavFileWriter.*`, `src/engine/RecorderService.cpp`,
`src/util/AsyncLifetimeToken.h`, `src/Main.cpp`, `tests/selftest/MiniDAWSelftestsMain.cpp`,
`CMakeLists.txt` (read only — not modified).

Changed (diagnostic scaffolding + this report only):

- `src/diagnostics/Spike02RenderHarness.h` (new)
- `src/diagnostics/Spike02TailAndStats.h` (new)
- `src/diagnostics/Spike01StateCapturePanel.cpp` (S2 plans, wait probe, S2 helpers)
- `src/diagnostics/Spike01StateCapturePanel.h` (`Spike01AudioLoadStats` + `snapshotAudioLoad` callback field)
- `src/app/MainAppWindow.cpp` (wire `snapshotAudioLoad` to `PlaybackEngine::snapshotAudioCallbackLoadAndReset`)
- `tests/selftest/MiniDAWSelftestsMain.cpp` (`testSpike02TimingStats`, `testSpike02TailPolicyEvaluator`)
- `docs/audits/SPIKE_02_ISOLATED_RENDER_TAIL_LATENCY.md` (this report)
- `build/run-spike02-plan.ps1` (untracked launcher; build directory)

Explicitly not touched: `CMakeLists.txt` and `installer/MiniDAWLab.iss` (user's
uncommitted version changes preserved), the canonical steering document, the test
project's musical contents, `src/Main.cpp` (the existing generic `--spike01-auto=<plan>`
comment already covers S2 plan IDs).

## 20. Builds, tests and measurement runs performed

- Debug build of `MiniDAWLab` + `MiniDAWSelftests` (ninja, MSVC): success.
- Selftests: **214 checks, 0 failures** (12 new SPIKE-02 checks covering the timing-stats
  aggregation and the tail evaluator: completion, cap failure, window restart after
  re-rise, zero tail, misclassification detection, grid size, dBFS round-trip).
- Measurement sessions (each a fresh unattended app launch against the TSE_pt2 project;
  logs in `%APPDATA%\MiniDAWLab\spike02-render-log.txt` + `spike01-capture-log.txt`):
  - S2A ×3: first run aborted (wrong project copy under Documents — 1 instrument track;
    corrected to the operator's real project); second run crashed in VB3-II
    `processBlock` (hazard H1, fixed); third run **COMPLETE**.
  - S2B, S2C, S2D, S2E, S2F, S2G: **COMPLETE** on first attempt each.
- Total measurement wall time ≈ 10 minutes (within the 30-minute budget).
- Temporary artifacts: one 220 MB WAV in the system temp directory during S2C, deleted
  after metrics (verified absent); no artifact in project media or project JSON; no raw
  state bytes logged (sizes + SHA-256 only).

## 21. Git status and completion confirmation

`git status` after the spike (working tree, nothing staged):

```
 M CMakeLists.txt                                  (user's version change — untouched)
 M installer/MiniDAWLab.iss                        (user's version change — untouched)
 M src/app/MainAppWindow.cpp                       (spike wiring)
 M src/diagnostics/Spike01StateCapturePanel.cpp    (spike plans)
 M src/diagnostics/Spike01StateCapturePanel.h      (spike callback)
 M tests/selftest/MiniDAWSelftestsMain.cpp         (spike selftests)
?? docs/audits/SPIKE_02_ISOLATED_RENDER_TAIL_LATENCY.md
?? src/diagnostics/Spike02RenderHarness.h
?? src/diagnostics/Spike02TailAndStats.h
```

Confirmed: nothing committed, nothing pushed, no PR opened; no live plugin instance was
processed by the worker; no message-thread plugin lifecycle operation occurred on the
worker; no product renderer/schema/UI was created; the ten-minute figure and playback
contention were actually measured (not extrapolated/inferred); tail values are backed by
the recorded 72-combination grid; latency preservation was not confused with PDC; no raw
WAV/state artifact exists or is staged; `CMakeLists.txt` and `installer/MiniDAWLab.iss`
are byte-identical to their pre-spike uncommitted state.

**Stopped for human review.**
