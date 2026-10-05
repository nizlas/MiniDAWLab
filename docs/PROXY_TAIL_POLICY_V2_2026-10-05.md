# Proxy tail policy v2 — DC-tracked tail detection for VB3-II (2026-10-05, 1.1.15)

Scope: the VB3-II destination (row 5, user's own preset with Overdrive / Tube Feedback) in the TSE
project could not be proxied in 1.1.14 — the render ran into the 30 s tail cap
(`TailLimitReached`) and nothing was published. This report records the root cause, the chosen
detector, the playback boundary rule, the policy versioning, what was measured, and what remains
open. Steering: `docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md` §14.2 step 8, §15.2, §15.6.
Evidence: `docs/evidence/proxy-tail-policy-v2-2026-10-05/`.

Nothing of the user's instrument settings, the 10 Hz Pro-Q 3 insert, the proxy boundary
(instrument output, before Pre/Post inserts, fader, pan, routing) or the rendered samples was
changed: the fix changes **what the tail detector and the plausibility check judge** and adds one
boundary rule to proxy playback. No normalization, limiter, DC filter or fade is applied to the
rendered music.

## 1. Root cause (measured on a complete temp copy, Release, tail-policy v1 forced)

`--stability-proxy-render-probe <copy> 5 … --tail-policy-v1 --retain-failed`
(`evidence/before-v1-release/release-render1.txt`):

| Measure | Value |
|---|---|
| Content | 6 clips, 159 notes, first note-on 22.500 s, last event 92.306 s |
| Readiness | verified, 22/22 notes answer, 4 passes, 7.8 s; parked offset handed to the render L/R −0.6053 / −0.6053 |
| Music `[0, 92.306 s)` | raw peak −1.8 dBFS, residual peak −0.7 dBFS, RMS −6.2 dBFS, 66/93 seconds audible |
| Offset after the last note | L −0.6054, R −0.6054 (in phase, constant to the last sample) |
| Reverb | residual last above −70 dBFS at **96.246 s** (3.9 s after the last event) |
| Raw |x| | never below −70 dBFS (the offset alone is −4.4 dBFS) ⇒ `TailLimitReached` after 30.005 s, 122.304 s rendered, nothing published |

The plugin parks a constant −0.605 on both channels after any note (the organ-dc defect measured
in `docs/ORGAN_DC_INSPECTOR_METER_2026-10-03.md`); tail-policy v1 judged the absolute per-block
peak, so a decayed reverb on top of a parked offset could never count as silence. The user's 10 Hz
low-cut insert removed the offset in live playback, but the proxy is rendered *before* the inserts
by design (§7.1) — so the detector, not the signal path, had to understand the offset.

## 2. The detector (tail-policy v2, `kTailPolicyVersion = 2`)

`DcTrackingPeakMeter` / `ProxyTailDetector` in `src/instruments/ProxyRenderTypes.h`; used by
`proxy_render::renderProxyDestination` (`ProxyRenderExecutor.h`).

* Per channel a one-pole offset estimate `dc[n] = dc[n−1] + a·(x[n] − dc[n−1])`,
  `a = 1 − exp(−1/(τ·fs))`, τ = `kTailDcTimeConstantSec` = 0.5 s — corner ≈ 0.32 Hz, defined at
  every sample rate; attenuation of real signal ≈ −24 dB at 5 Hz, −30 dB at 10 Hz, −36 dB at
  20 Hz, −42 dB at 40 Hz, so a bass or slow reverb component still has to decay itself.
* Judged per block: the **residual peak** `max |x[n] − dc[n]|`, **per channel, louder channel
  wins** — never a stereo mid-sum (unequal or anti-phase offsets on L/R are judged honestly).
* The estimate runs continuously over every block of the render (music and tail), so the music→
  tail boundary is not a step; the detector *decides* only in the tail phase. Window 1.0 s counted
  in rendered samples, threshold −70 dBFS, cap 30 s — unchanged from v1. The block that straddles
  the boundary is judged whole (tolerance ≤ one render block in the accepted tail length).
* The readiness verification's settle / flush use the same meter and hand the parked offset to the
  render as the detector's seed (`seedDcEstimate`), so the asset's first sample is not a step the
  detector has to let decay. The readiness stimulus is never part of the asset.
* Not a per-block mean subtraction: no sample of the asset is altered; the meter only judges.
* Reaching the cap with residual output still above the threshold stays an honest
  `TailLimitReached`; non-finite samples still fail the render; cancellation and the failure paths
  leave the previous generation and metadata intact (unchanged code paths).

### Plausibility (step 8) — a parked offset is not "audible output"

`ProxyRenderResult::maxResidualPeakLinear` (over the render's own blocks; readiness excluded) must
exceed the tail threshold, otherwise `NoAudibleOutput`. The raw peak stays in the result for
diagnostics (`maxPeakLinear`, `dcEstimateAtEnd`, `firstSample` / `lastSample`). Verified cases:
scheduled notes + totally silent instrument; notes + constant offset only; real tones on an
offset; an instrument whose response begins only after loading (Groove Agent SE, §5); readiness
stimulus sounding but the render silent ⇒ still `NoAudibleOutput`.

## 3. Policy identity and older generations

* `kTailPolicyVersion` 1 → **2**; fingerprint F12 includes the policy versions, so a new render
  carries a new generation name; `ProjectFileProxyMetadataV20` records the versions as before.
* `proxy_playback::comparableRenderPoliciesFor`: a generation recorded under tail v1 — or with no
  recorded policy versions, read as v1 — is **comparable** with v2: whatever v1 accepted (silence
  in raw terms) v2 accepts too, with the same end point. The recorded-config recompute (machine
  without the Primary) uses the recorded policies; with the Primary present
  `publishedComparableFingerprint` is computed under the recorded schema + policies. Any other
  policy drift is not comparable ⇒ `Stale` under the existing stale handling.
* Consequence: existing proxies keep their status after the update (measured §5: the Groove Agent
  generation published by 1.1.14 reads `Current` on load under 1.1.15) — no fake `Current`, no
  automatic re-render loop. A manual *Render now* publishes a new generation under a new name and
  the old file is kept (never overwritten, never deleted).
* Metadata compatibility: unchanged schema; old projects load as before.

## 4. Playback boundary — resting level past EOF

With v2 an asset may legitimately end on the instrument's parked offset (−0.605). Returning to
zero at EOF would be a 0.6 step the Primary never produces. `ProxyPlaybackReader` therefore
measures each channel's mean over the asset's final 1.0 s when the asset is opened
(`restingLevelAtEnd`; zero when both channels' means are below the tail threshold — an
offset-free proxy still ends in digital silence) and continues at that level past EOF: in
`audioThread_fetch` (early return past EOF, the straddling block, the loop-head remainder) and in
`convertRangeLinear` (exact-copy and sinc / resampled paths). The pre-roll before sample 0 stays
zero. This is a boundary rule of the reader, defined in one place; the asset is not padded.

Measured through the production playback path with the user's insert chain (Pro-Q 3 10 Hz low-cut,
Post; row fader 0.562) — `--stability-proxy-playback-edges <copy> 5 …`, Release,
`evidence/edges-release/proxy-playback-edges.txt`; pre-insert = proxy boundary, post-strip = after
the insert chain, fader and pan:

| Window (proxy as source) | Pre-insert | Post-strip peak | Verdict |
|---|---|---|---|
| EOF −2.6 … −0.6 s (reverb decaying) | DC −0.6054 / −0.6054 | −40.7 dBFS (decaying reverb) | content |
| EOF −0.6 … +1.4 s (crossing the asset end) | DC continues | −65.5 dBFS | **no step at EOF** |
| EOF +1.4 … +3.4 s (continued transport) | DC continues | −82.0 dBFS | silence |
| Loop wrap EOF+2 → EOF−3 (Cycle) | DC continues | −73.0 dBFS | **no step at the wrap** |
| Offline mixdown [EOF−5, EOF+3] (Stereo Out) | — | largest sample-to-sample jump within ±0.5 s of EOF **0.00000**; slices decay −18 → −97 dBFS monotonically | **no step** |

The Primary under the same windows produces nothing there (a seek into the tail plays no notes),
so the proxy's reverb tail is content the live path cannot even reproduce from a seek.

## 5. Verification

Code inspection, measured audio, UI verification and listening are distinguished below.

**Regression tests (measured, deterministic)** — `MiniDAWSelftests` 3478 checks, 0 failures;
other focused executables unchanged (110 / 149 / 47 / 56 / 49 / 121). New or rewritten cases:

* `p1d-tail` at 8 k / 44.1 k / 48 k / 96 k × blocks 64 / 512 / 1024: constant DC (tail ends at
  the window), DC + decaying tone (same end point as the tone without DC), 5 / 10 / 20 / 40 Hz
  decays preserved (tail ends only when the slow component has decayed), unequal and anti-phase
  L/R offsets, a real tail still above threshold at the cap ⇒ `TailLimitReached`, a varying floor
  exactly at −70 dBFS, a changing offset (transient counts as output); each case failed under v1
  judging (`setJudgeRawPeakForDiagnostics`) where the fault is the offset.
* `p1d-ready` DC-parking fake: readiness verified, parked offset handed over, render **Succeeded**
  with the asset's last sample at the parked value (was `TailLimitReached`); `p1d-nooutput`:
  DC-only fake ⇒ `NoAudibleOutput` (raw > 0.5, residual < −70), probe-only fake ⇒ readiness
  verified but `NoAudibleOutput`.
* `tail-compat`: v1 and unrecorded comparable with v2, other drift and a newer tail version not;
  F12 changes with the tail version; `p1g-currency` / `layer-compat` record policy versions.
* `p1g-rest`: resting-level continuation same-rate, resampled 44.1 → 48 k (worst 1e-6), silent
  ending ⇒ zeros.

**Real VB3-II (measured audio, production path, Release and Debug)** — complete fresh copy of the
user's project (17 files) under `%TEMP%\dal-tse-vb3\`; the original was never touched:

| | before (v1 forced) | after v2, Release | after v2, Debug |
|---|---|---|---|
| status | `TailLimitReached`, 122.304 s | **Succeeded**, 97.259 s (tail 4.960 s) | Succeeded, 97.259 s |
| music raw / residual peak, RMS, audible s | −1.8 / −0.7 dBFS, −6.2, 66/93 | identical | identical |
| residual last above −70 | 96.246 s | 96.246 s | 96.246 s |
| first / last sample L/R | −0.6054 / −0.6054 | −0.6054 / −0.6054 | same |
| asset SHA-256 | 8402d121… (retained for diagnostics only) | 4522ed22… | **4522ed22… (byte-identical)** |

Probe tones: the lead-in 0–22.5 s is the parked offset only (AC −200 dBFS per second) — nothing
of the readiness stimulus is in the file. Publication through the production path: canonical
generation `track_5_sha256_adcca050…wav`, 37 347 432 bytes, metadata `lengthSamples` 4 668 416,
asset check ok, destination `Current`; the Debug render reused the identical file.

**Save and reopen (production paths)** — the publish step saved through the window's Ctrl+S path;
the saved copy reloaded in the Debug build: `published: sha256:adcca050… schema=2 pub=1 save=1 |
destination=Current`; a sibling-named copy (`…-proxyedges.dalproj`) loaded for the edge
measurements with `runtime=ProxyCurrent` and played the file (`evidence/save-reopen-log-excerpt.txt`).

**Playback through the user's filter** — §4 table (EOF, past EOF, loop wrap, mixdown): no new
step. Start / Stop: §6.

**Groove Agent SE regression (asynchronous loading)** — `evidence/ga-regression-release/`: the
1.1.14 generation (tail v1) reads `destination=Current` on load under v2; readiness verified
after 5 passes (1.75 s); render Succeeded 49.333 s (identical length to 1.1.14), raw = residual
peak −0.3 dBFS, offset 0; published under a new generation name, old file kept.

**Not done / not claimed** — no listening test was performed in this task (all verdicts above are
measurements); in-app UI scenarios beyond the probe and edge runs were not repeated for this
delivery (the changed code is the executor's judging, the reader's EOF rule and comparability,
each covered above); the real VB3-II instance was not compared for bit identity across separate
instances (not required).

## 6. Remaining defect within this assignment — transport Start / Stop step through the user's filter

**Status: open, measured, not fixed in 1.1.15.** Accepted as a known limitation for this delivery
so the mixer work can start; no transport-hold mechanism was implemented. Not every proxy
transition is click-free.

* **Reproduction**: TSE project (or the temp copy), VB3-II row with the proxy as source (Primary
  unavailable or proxy selected), insert chain Pro-Q 3 10 Hz low-cut (Post), fader 0.562.
  Transport stopped at any position where the asset is at its parked offset — the lead-in before
  the first note (0–22.5 s), any pause, the tail or past EOF. Press Play; or Stop while playing in
  such a region; or restart.
* **Audio path**: proxy reader (0 when stopped → −0.6054 when playing, and back) → row's Post
  insert (10 Hz high-pass turns the 0.6 step into a short low-frequency pulse) → fader → Stereo Out.
* **Measured level** (`evidence/edges-release/proxy-playback-edges.txt`): Play from stopped at
  1.0 s: post-strip peak 0.0517 (−25.7 dBFS; previous runs 0.0539–0.0620), Stop: 0.0661
  (−23.6 dBFS), restart: 0.0664 (−23.6 dBFS) — at the row fader 0.562, i.e. ≈ −19 dBFS at unity
  — followed by the filter's settle at ≈ −40 dBFS for about two seconds. Pre-insert the step is
  the full 0 ↔ −0.6054 on both channels.
* **Difference vs Primary**: the live VB3-II keeps running while the transport is stopped, so its
  offset persists across Stop / Play and the filter sees no step (Primary windows: −59.7 dBFS at
  Play, −136.9 dBFS at Stop, −159.9 dBFS at restart). The Primary's own offset onset happens with
  its first note, inside the attack.
* **Why it is not fixed here**: the correct place is proxy *transport* behaviour (the proxy's
  idle output while stopped), not the asset and not the insert chain; a fade or ramp cannot hide a
  0.6 offset change through a 10 Hz high-pass (the filter would need a change slower than ~10 s),
  so only offset continuity across Stop / Play can remove it. Deferred to the backlog
  (`docs/PHASE_PLAN.md`).

## 7. How to create the new organ proxy (user steps)

1. Run a 1.1.15 build (`build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe`, the
   installer or the portable zip).
2. Open the project; the VB3-II row (update mode **Manual** in the saved project) still shows its
   old schema-1 generation from September as `Stale` — exactly as before; nothing is re-rendered
   automatically.
3. On the VB3-II row use *Render now* (Inspector / proxy status) with the Primary loaded. The
   readiness verification takes about 8 s for this preset, the render about 11 s; the status
   becomes `Current` with a 97.3 s asset. The old generation file is kept.
4. Save the project (Ctrl+S). The proxy plays whenever the Primary is unavailable or the proxy is
   selected as the source; the 10 Hz insert keeps removing the offset as before.
