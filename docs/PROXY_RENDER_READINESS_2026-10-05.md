# Proxy render readiness and publication identity (1.1.14)

Investigation and correction of the proxy problem noted in 1.1.13 (commit `ae45902`): the same
render identity produced a 49.3 s proxy in a Debug build and a 39.0 s proxy in a Release build, and
the second publication then failed with "generation-name collision with a non-identical existing
file". Everything below was measured on copies (`%TEMP%\dal-tse-copy\…`, `%TEMP%\dal-proxy-probe\…`);
the user's real projects were only read. Evidence: `docs/evidence/proxy-render-readiness-2026-10-05/`
(per-render reports with per-second profiles, identity files, artifact manifest with SHA-256).

## 1. Reproduction with frozen inputs

Tool: `--stability-proxy-render-probe <project> <trackId> <outDir> [--repeat N] [--wait-after-prepare ms]
[--publish] [--state-blob <file>] [--no-readiness] [--realtime-indication]` (new, diagnostics). It renders
one destination repeatedly through the production engine seam (`AppProxyRenderEngine::captureRequest` →
`prepare` → worker `render`), keeps every artifact, writes a content analysis
(`src/diagnostics/ProxyRenderProbeAnalysis.h`: first sample above −60 dBFS, last sample above the
−70 dBFS tail threshold, music / tail energy, per-second peak + RMS) and documents the frozen identity.

Groove Agent SE row ("Track 3", TrackId 3) of the TSE copy:

| Frozen input | Value |
|---|---|
| Fingerprint (render identity, schema 2) | `sha256:7bd0dd458c69b1ecfbb21e092848df2aff5c2d48048eee66f628595ac7529ae4` |
| Canonical snapshot bytes (5589 bytes) | SHA-256 `7bd0dd45…29ae4` — identical in every run, Debug and Release |
| Plug-in | Groove Agent SE 5.2.20 (Steinberg, VST3), `C:\Program Files\Common Files\VST3\Steinberg\Groove Agent SE.vst3` |
| Plug-in state blob | **not byte-stable**: 148 559–148 588 bytes, a different SHA-256 at every capture (Debug `3d801dca…`, `da0f5529…`; Release `c7afd687…`, `1905ff59…`, `3a823830…`) — the blob contains volatile bytes; the fingerprint deliberately excludes blob bytes (hybrid identity: revision 83 + pairing), so the identity was identical anyway. `--state-blob` lets both builds render one dumped blob; the results below did not depend on which blob was used. |
| State revision | 83 |
| Render rate / block | 48 000 Hz / 512; timeline reference rate 48 000; note-off gate 100 ms |
| Tail policy v1 | −70 dBFS absolute per-block peak, 1.0 s window, 30 s cap; policies latency v1 / tail v1 / render v1 / format v1 |
| Content | 1 destination clip, 166 notes (channel 10), no routed sources; last relevant event = 1 825 333 samples (38.028 s) |

Measured renders (same project copy, same identity):

| Run | Build | Status | Length | Tail | First audible | Music RMS | Audible s in [0, 38.0) | SHA-256 |
|---|---|---|---|---|---|---|---|---|
| debug-a render 1 | Debug | Succeeded | 49.333 s | 11.307 s | **16.407 s** | −20.6 dB | 23 / 39 | `b585053d…` |
| debug-a render 2 | Debug | Succeeded | 49.333 s | 11.307 s | **10.402 s** | −19.8 dB | 29 / 39 | `58ef64f5…` |
| release-a renders 1–3 | Release | Succeeded | **39.029 s** | 1.003 s | **never** (peak −200 / −225 dBFS) | −251 dB | **0 / 39** | `1650f940…`, `f95580a8…`, `f95580a8…` |
| release-wait3000 renders 1–2 (3 s wait after prepare, message loop running) | Release | Succeeded | 49.333 s | 11.307 s | **4.002 s** (= first note) | −19.5 dB | 35 / 39 | `e9fb4170…` ×2 |

Render speed: Release 637–891× realtime while silent (44–61 ms wall for 39 s), 80× when the kit
sounds; Debug 73–77×. Preparation (create + restore + prepare) 140–190 ms; prepare→first block
70–90 ms in both builds.

## 2. What differs

* **The Release renders contain no instrument sound at all**: digital silence from the first to the
  last sample, 39.029 s = span end (38.028 s) + the 1.003 s silence window. The "39.0 s" length is
  the tail detector doing its job on a silent instrument.
* **The Debug renders are incomplete too**: the first notes (from 4.0 s) are missing; sound appears
  at 10–16 s of material and the two Debug renders differ from each other. The 11.3 s tail is a
  real cymbal decay (tail peak −3.5 dBFS, last sample above −70 dBFS at 48.3 s).
* With a 3 s wait after prepare, both Release renders are **complete and byte-identical** —
  Groove Agent SE is deterministic once its content is loaded. The difference between the builds
  is therefore not sample-level variation and not the tail detector; it is **how much of the kit had
  streamed in by the time the render reached each note**, which depends on wall-clock time, and the
  Release build simply rendered everything before anything had loaded.
* The plug-in's state bytes grow on every `getStateInformation` call during loading (148 547 →
  148 757 bytes over 3.4 s) and keep changing afterwards; they are **not** a readiness signal.
  JUCE's VST3 host exposes no `IProgress`, so the plug-in's own loading progress is not observable.

Root cause (category 1 of the task — an incorrect / incomplete render result): **the isolated
render instance restores its state synchronously but Groove Agent SE loads its samples
asynchronously; the render started immediately after `prepareToPlay` and rendered silence for notes
whose samples had not arrived. Format / length validation accepted the silent asset and the
scheduler published it as Current.**

Impact beyond the test copy (read-only check): the user's saved project
`C:\Users\nicla\Music\TSE_pt2_260827\TSE_pt2.dalproj` (saved 2026-10-03) references the GA generation
`0eb66f3d…` whose file (`InstrumentProxies\track_3_sha256_0eb66f3d….wav`, 14 987 368 bytes, rendered
2026-09-15) is **byte-identical to the silent Release renders** (`f95580a8…`). Under the 1.1.13
comparability rule that generation is Current, so on a machine without Groove Agent that track
would play 39 s of silence from a proxy reported as current. The autosave of 2026-10-04 references
`7bd0dd45…` (18 944 104 bytes, 49.3 s, rendered by the user's own 1.1.12 session). Nothing in the real
project was modified; see §6 for what to do.

## 3. Corrections

### 3.1 Readiness verification before the render (`ProxyRenderExecutor.h`, steering §14.2 step 4b)

`verifyInstrumentReadiness` runs on the render worker between prepare and the block loop:

1. The content's distinct (channel, note) pairs — `ProxyOfflineSequencer::collectDistinctNoteOns`,
   at most 48, in order of first occurrence — are the stimulus. The content's initial controller
   state (reset prefix + first value of every CC / pitch-bend stream) is sent first, in a block of
   its own.
2. One pass plays each note for 0.25 s and judges whether it answered: AC-coupled block peak above
   −90 dBFS and at least 12 dB above the AC residual measured right before the note (the previous
   note is allowed to settle under the tail threshold first, bounded by 2 s). Each note is released
   with a plain Note Off, nothing else.
3. Passes repeat 250 ms apart (the worker sleeps in cancellable slices; the message thread keeps
   running) until **no new note has started answering for three consecutive passes** — the
   instrument's response has settled. Bounds: 20 s with no answer at all, 60 s overall; both are
   caps on waiting, never correctness inputs. An instrument that answers at once is verified in four
   passes.
4. The instrument then settles under the tail threshold for a full silence window (bounded 12 s);
   All Sound Off / All Notes Off / sustain off are sent only if it does not settle. The render's own
   reset prefix follows in its first block.

The outcome is recorded in `ProxyRenderResult::readiness` (verified, passes, stimulus / sounding
notes, wall-clock, blocks, flush residual, per-pass trace) and logged at publication; it is never
fingerprinted or persisted. Measured: GA SE verified after 5 passes, 11/11 notes, 1.2–1.8 s; the
render is then complete from its first note in both builds (Debug and Release renders byte-identical,
`00e1c27c…`).

Why not a sleep: a fixed delay is either too short on a slower disk or wasted on every instrument
that is ready at once; the response-based criterion adapts to both and is bounded.

### 3.2 No silent "Current" (`ProxyRenderExecutor.h`, §14.2 step 8)

A render whose scheduled notes produced nothing above the tail threshold anywhere fails with the new
reason `NoAudibleOutput` ("the instrument produced no audible output for its N scheduled notes
(readiness: …) — render incomplete, not published"). The previous generation and its metadata are
retained, the destination derives Failed for the current identity, Retry stays available. The
explicit silent generation (§15.7) remains the only legitimate silent asset and exists for empty
destinations only.

### 3.3 Publication identity and reuse (`ProxyAssetStore.h`, §16.1 / §16.3)

The fingerprint identifies the *render inputs*; it excludes the state bytes and the plug-in's own
variation, so two files of one identity are not guaranteed identical — and an earlier file may have
been an incomplete render. The previous rule compared format and length only ("reuse") and refused
anything else ("collision", forever). New rule:

* **Reuse requires identical bytes** (`filesHaveIdenticalBytes`, streamed comparison after the
  format / length validation; equal length is necessary, not sufficient).
* **A non-identical occupant of the canonical name is never touched**; the new render is published
  under the next free sibling name `track_<id>_<fingerprint>_<n>.wav` and the metadata names that
  file (`relativePath`, `lengthSamples`); a byte-identical sibling is reused like the canonical file.
  `ProxyPublishOutcome::publishedUnderSiblingName` / `collisionNote` report what happened; the
  cleanup policy (report-only enumeration of unreferenced generations) is unchanged.
* Failed publication still retains the previous files and metadata and never marks the destination
  Current (unchanged; exercised by `p1e-pubfail` and the new `p1f-collide` checks).

Measured chain with the first (silent) generation file **kept** under the canonical name:
Debug render 1 → published as `…_2.wav` (18 944 104 bytes, `00e1c27c…`), metadata `lengthSamples
2 368 000`, asset check ok, destination Current, the silent canonical file untouched; Debug render 2
→ byte-identical sibling reused; Release renders 1–2 against the same folder → byte-identical to the
Debug render → the sibling reused, nothing new written. The saved probe copy reloads with the sibling
path (`published … schema=2 pub=83 save=83 | destination=Current`).

## 4. Legitimate variation (category 2) and its handling

* Groove Agent SE is byte-deterministic once loaded, but the stimulus passes advance its internal
  round-robin state: the final render's bytes depend on how many passes were needed
  (`00e1c27c…` after 5 passes in four runs; `c27c2032…` / `3806b1a5…` in later runs with a changed
  flush). Each is a complete, valid rendition; the publication rule above turns such a re-render into
  a sibling file instead of a failure, and never into a false reuse.
* VB3-II varies at sample level between runs (SPIKE-02 H5) — same handling.
* Consequently repeated re-renders of one identity may accumulate sibling files; they are listed by
  the existing unreferenced-generation report and are never deleted automatically (unchanged policy).

## 5. Findings outside the root cause

* **VB3-II (TSE preset) cannot be proxied — before and after this change.** Its render fails at the
  tail cap (`TailLimitReached`, 30 s of material at −1.6 … −2.0 dBFS after the last event) with
  readiness enabled, disabled (`--no-readiness`) and without the offline indication
  (`--realtime-indication`). The cause is the known organ-dc defect: after any note the plug-in parks
  a DC offset of −4.4 dBFS on its output (the standalone `ExportLevelFocusedTests --probe-vst3-dc`
  shows dc = −0.605, rms = |dc| after note-off and even after `reset()`), and the tail policy's
  absolute-peak detector never sees silence. The render fails honestly and nothing is published; the
  readiness decision was made AC-coupled so that such a plug-in's notes still count as answering
  (12/12), and the flush reports the residual. A DC-aware tail policy would be a tail-policy version
  bump (§15.2) — not done here.
* VB3-II's state blob is byte-stable (one SHA-256 across all runs); GA SE's is not.
* The 1.1.13 cycle-takes scenario failed once in eight runs today at "combined past-end" with the
  MIDI take 128 samples (one device block) shorter than the audio take: the timeline advanced one
  block less than the monotone clock between the acknowledged boundaries. It passed on the rerun
  (Debug and Release). Not touched by this change; recorded for a follow-up (candidates: a block
  that did not advance the playhead while the run captured, e.g. a seek applied during the run).

## 6. Delivery and remaining uncertainties

* Tests: `MiniDAWSelftests` 3300 checks (new `p1d-ready` late-loading / immediate / DC-parking /
  disabled, `p1d-nooutput`, `p1f-collide` sibling / same-length-different-content / identical
  sibling reuse / different length / directory under the canonical name / missing temp), all other
  focused executables unchanged; in-app `--stability-proxy-recording` (TSE copy: Render now now
  goes through readiness; the published generation is complete, 49.3 s), `--stability-midi-cycle-takes`,
  `--stability-live-midi`, `--stability-mixdown`, `--stability-organ-dc`, `--stability-inspector-panel`,
  `--stability-midi-routing` PASS; the probe runs listed above on Debug and Release.
* The user's real project still references the silent `0eb66f3d…` generation in its last save.
  Recommended: open it in 1.1.14 on this machine (Primary present) and re-render Track 3 (Render now
  in Manual mode, or the Auto mode's own re-render after the content changes); the silent file is
  left in place. A load-time plausibility audit of already-published generations (notes scheduled
  but the asset is digital silence ⇒ Stale) would catch this class retroactively; proposed as a
  follow-up, not implemented here (it reads every proxy asset at load).
* Readiness is a response-based heuristic with bounds: an instrument whose kit takes longer than
  20 s to produce the first answering note, or whose response keeps changing for 60 s, renders
  unverified (logged); the no-output rule then still refuses a fully silent asset, but a partially
  loaded one would pass. No such instrument was observed.
* Readiness adds wall-clock to every render (GA SE ≈ 1.2–1.8 s, instant instruments ≈ 0.8 s) and
  advances plug-in-internal state (round-robin) before the render; the proxy therefore matches a
  live performance that followed a few test hits, not a freshly instantiated plug-in.
