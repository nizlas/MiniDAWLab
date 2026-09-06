# SPIKE-03 — Proxy playback I/O and sample-rate adaptation (evidence gate for P1G)

Status: **decided** (2026-09-06); **hardened** (2026-09-06): conversion quality raised to
full-quality music playback (band-limited Kaiser-windowed sinc) and prepared loop wrapping added —
see "Conversion quality (corrected contract)" and "Prepared loop wrapping" below. This report
closes OI-002: it selects the bounded playback mechanism required by steering §7.3/§15.3
(PI-030/PI-031) and records the measured bounds and resulting P1 constants. Implementation:
`src/instruments/ProxyPlaybackReader.h`; deterministic evidence: the `spike03-*`, `q-*` and
`p1g-loop*` selftests in `tests/selftest/MiniDAWSelftestsMain.cpp` (synthetic WAVs only — no
plugin rendering was repeated for this gate).

## Selected mechanism

**Bounded read-ahead ring per reader, filled by ONE shared low-priority I/O thread, with sample-rate
conversion performed during the fill (off the audio thread).** This is candidates (1)+(2) collapsed
into their simplest correct form:

* The ring holds **timeline-domain** frames: the fill thread reads the asset (WAV at the
  generation's recorded render rate) and converts while filling, so the audio thread consumes
  exactly **one ring frame per output sample**. The entire audio-thread contract is atomics +
  `memcpy` + zero-fill — no resampler state, no interpolation, no arithmetic beyond index math.
* Position mapping is **stateless**: timeline frame `t` reads asset position
  `p(t) = t · assetRate / timelineReferenceRate`, anchored at asset sample 0 = timeline reference
  sample 0 (span rule §15.6). Every output frame is a pure function of `t` (band-limited
  Kaiser-windowed-sinc interpolation around the absolute position `p(t)`; bit-exact copy fast path
  when the ratio is 1.0), so seek, loop wrap and re-reads are bit-deterministic regardless of fill
  chunking or history — verified by the `spike03-seek` and `q-seek` selftests, which replay
  overlapping ranges after forward seeks and loop wraps and require equality with an independent
  reference computation (bit-exact same-rate; exact-kernel reference cross-rate).
* The mapping is anchored in timeline frames, so a **device/engine-rate change alone changes
  nothing** (PI-030): TLD-1 timeline integers are device-rate-agnostic, and the generation's
  identity (its recorded render rate) is untouched. The reader is derived cache state and can be
  discarded/rebuilt freely without affecting proxy currency.

### Rejected alternatives

* **Unbounded full-file preload** — rejected by steering §7.3 (≈23 MB/min/proxy decoded; a
  ten-minute multi-proxy project demands gigabytes). Not measured further.
* **Separate budgeted full preload for short assets (candidate 3)** — unnecessary complexity: the
  ring serves all asset lengths in constant memory, and the measured fill throughput (≈779×
  realtime) makes a special short-asset path pointless. One mechanism, one code path.
* **Audio-thread streaming resampling over a source-rate ring (candidate 1 in its literal form)**
  — rejected because it puts interpolator state and fractional-position arithmetic on the audio
  thread for no benefit; converting during the fill gives the same bounds with a strictly simpler
  audio-thread contract.
* **Whole-file engine-rate derived cache on disk/RAM (candidate 2 in its literal form)** — rejected
  as the general solution for the same unboundedness reason as full preload; the windowed fill IS
  the derived representation, sized by the ring.
* **juce::BufferingAudioReader / ResamplingAudioSource** — JUCE's stock buffering reader performs
  blocking waits and TimeSliceThread scheduling not designed around our strict no-lock audio
  contract, and ResamplingAudioSource keeps interpolation state on the calling thread. Both would
  need wrapping/defensive copies that end up re-implementing the ring anyway.

### Conversion quality (corrected contract)

**The proxy is the authoritative sound reference when Primary is unavailable.** Cross-rate playback
necessarily performs conversion, and that conversion must be suitable for normal full-quality music
playback — the original v1 statement that a proxy has "no quality need" was incorrect and is
withdrawn. **Linear interpolation is insufficient as the final P1 implementation** (images only
≈ −40 dB at 44.1↔48). Bounded memory and audio-thread realtime safety remain mandatory and are
unchanged by this correction.

Selected implementation (P1G hardening): **position-stateless polyphase Kaiser-windowed sinc**
evaluated during the fill (still entirely off the audio thread, inside `convertRangeLinear`):

* kernel: `kSincZeroCrossings` = 48 per side, `kSincPhasesPerCrossing` = 512 (linear inter-phase
  interpolation, < −100 dB table error), Kaiser β = 10 (≈ 98 dB design stopband), cutoff at
  `kSincCutoffScale` = 0.938 of the **lower** of the two Nyquist frequencies — the kernel stretch
  anti-aliases downsampling (stopband at the output Nyquist) and rejects images when upsampling;
* one shared immutable coefficient table (≈ 0.10 MiB per process) built on first use off the audio
  thread using the established modified-Bessel-I0 series (same series as
  `juce::dsp::SpecialFunctions::besselI0`; `juce_dsp` itself is not linked by every target);
* exact long-term ratio by construction: every output frame is computed from the absolute rational
  position `p(t)` — there is no accumulating phase state, so there is nothing to drift and nothing
  to reset nondeterministically on seek/source replacement/rate change;
* stereo synchronization by construction: both channels share one position and one kernel window;
* preserved plugin latency is untouched (the mapping is a pure rate ratio; no extra compensation).

`juce::Interpolators::WindowedSinc` was inspected and **rejected**: it keeps a fixed input-Nyquist
cutoff (it aliases when downsampling 48 → 44.1 — unacceptable under this contract) and it is a
stateful stream interpolator whose output depends on chunking, which would break the reader's
by-position determinism (seek ≡ fresh reader). Measured quality of the selected kernel (Debug,
`q-*` selftests, analytic signals, independent exact-kernel/power-series-Bessel reference):

| Quality measurement | Requirement | Measured |
|---|---|---|
| Passband level error @ 1/10/18 kHz, 44.1→48 and 48→44.1 | ≤ 0.25 dB | **±0.0000 dB** |
| Alias rejection, 48→44.1 (23 kHz tone above output Nyquist) | ≥ 70 dB | **107.7 dB** |
| Image rejection, 44.1→48 (21 kHz → 23.1 kHz image, Hann-Goertzel) | ≥ 70 dB | **115.0 dB** |
| Output duration over 60 s+ input | exact rational | **exact** (ceil(len·out/in)) |
| Impulse response | finite, stable | finite, bounded ≤ 1.0, **exactly zero** outside support |
| Channel drift | none | ch1 stays the **bit-exact negation** of ch0 |
| Seek/reset vs fresh reader at same position | identical | **bit-identical** |

Native-rate re-render remains a quality refresh (steering §15.3) and is never a playback
prerequisite.

### Prepared loop wrapping (P1G hardening)

A known transport cycle is **not** a seek: the engine announces the active loop `[L, R)` per block
(`audioThread_noteProxyLoopRangeForCurrentBlock` → `audioThread_setPreparedLoop`, atomics only),
and the I/O thread keeps a preallocated **loop-head cache** (`kLoopHeadFrames` = 16384 timeline
frames ≈ 0.34 s @ 48 kHz) resident from the loop start *before* the playhead reaches the boundary.
A callback that crosses the boundary consumes the exact pre-boundary tail from the ring and the
exact loop-start frames from the head cache within the same output block (seqlock-guarded copy —
no locks, no allocation, no file I/O): no inserted zeros, no duplicated or omitted timeline frames,
and **no underrun is counted or reported**. An unprepared arbitrary seek still recenters through
the normal `ProxyPreparing` path, and genuine I/O starvation still counts and reports
`PlaybackUnderrun` (`p1g-loop` selftests, ramp source).

## P1 constants (implemented in `ProxyPlaybackReader.h`)

| Constant | Value | Meaning |
|---|---|---|
| `kRingFrames` | 131072 (2^17) | ring capacity in timeline frames ≈ 2.73 s @ 48 kHz |
| `kReadAheadTargetFrames` | 65536 (2^16) | resident read-ahead ≈ 1.37 s @ 48 kHz |
| `kFillChunkFrames` | 8192 | frames converted per fill step (bounds scratch + round latency) |
| `kMaxConversionRatio` | 4.0 | corrupt-metadata guard; 44.1↔48↔96 are far inside |
| `kLoopHeadFrames` | 16384 (2^14) | prepared loop-head cache ≈ 0.34 s @ 48 kHz |
| `kSincZeroCrossings` | 48 | sinc zero crossings per kernel side |
| `kSincPhasesPerCrossing` | 512 | polyphase table resolution (< −100 dB interp error) |
| `kSincKaiserBeta` | 10.0 | Kaiser window (≈ 98 dB design stopband) |
| `kSincCutoffScale` | 0.938 | cutoff fraction of the lower Nyquist (transition margin) |

**Per-reader memory bound:** ring 2 ch × 131072 × 4 B = **1.00 MiB** + asset fill scratch
2 ch × (8192·4 + 2·205 + 16) × 4 B ≈ **0.25 MiB** + converted-output scratch 2 ch × 8192 × 4 B ≈
**0.06 MiB** + loop-head cache 2 ch × 16384 × 4 B = **0.13 MiB** ⇒ **1.44 MiB/reader** (constant,
independent of asset length — verified with a 60 s asset), plus one shared immutable kernel table
≈ 0.10 MiB per process. **Global bound:** one reader per instrument destination plus one shared
I/O thread; the 16-reader requirement costs **23.05 MiB** (measured, asserted < 24 MiB).

## Measurements (Debug build, this machine, deterministic synthetic WAVs)

From the `spike03`/`q-*`/`p1g-loop*` selftest suite (all 931 selftest checks green, exit 0):

| Measurement | Result |
|---|---|
| 16 simultaneous cross-rate (44.1→48) sinc readers, 64 blocks × 512 frames each | fully served, **0 underruns**, 135 ms total |
| 16-reader audio-callback cost (one resident 512-frame fetch per reader) | **≈ 2–4 µs** aggregate (deadline 10 667 µs @ 48 kHz) |
| 16-reader memory | **1.44 MiB/reader, 23.05 MiB total** (asserted < 24 MiB) |
| Cold far-seek → range resident (`seekReady`) | **0.29 ms** |
| `audioThread_fetch` cost per 512-frame block (resident) | **≈ 91–158 ns** |
| Sustained same-rate stream, 10 s of material | **≈ 12 ms** (**≈ 860× realtime**), 0 underruns |
| Sustained cross-rate sinc stream (44.1→48), 10 s of material | **≈ 225 ms** (**≈ 44× realtime**, asserted > 16×), 0 underruns |
| Same-rate playback (ratio 1.0) | bit-exact vs the file (fast path) |
| 44.1 kHz asset → 48 kHz timeline and 48 kHz → 44.1 kHz | matches the independent exact-kernel reference at head/mid/near-EOF |
| Resampler quality (passband/alias/image/impulse/drift) | see "Conversion quality (corrected contract)" above |
| Seek + loop wrap + re-read | bit-identical to fresh reads (stateless mapping) |
| Prepared loop wrap (exact-boundary, mid-block, multi-wrap short loop, 30 passes, cross-rate) | exact frames, **0 underruns** (`p1g-loop`) |
| EOF | exact zeros, fully served, **never counted as underrun** |
| Real underrun (no fill service / starved position) | silence + counted + reported distinct from EOF; recovers when service resumes |
| Retirement while file open (Windows) | file deletable immediately after unregister + release (handle closed off-audio) |
| Open validation | missing file, non-WAV bytes, length mismatch, rate mismatch, absurd ratio → `openFailed()`, never published |

## Audio-thread contract (verified by construction and selftests)

`audioThread_fetch` performs: two atomic loads, one atomic store, bounded `memcpy` from the
preallocated ring, zero-fill, and one post-copy revalidation (downgrades a torn window race to
silence + underrun — never garbage audio). It performs **no** filesystem reads, **no** waiting on
any thread, **no** mutex acquisition, **no** allocation, **no** file open/close, and **no**
destruction. File open/validate happens in the constructor (message thread); reads happen on the
shared I/O thread; destruction is protocol-guaranteed off the audio thread (retire list + audio
callback drain — see `ProxyPlaybackReader.h` header notes).

## Underrun behavior (decided)

Missing pre-EOF frames are served as silence and counted; the stream recovers automatically when
the fill catches up (status surfaces as `PlaybackUnderrun`). A permanent read failure latches
`streamFailed()` and the source is demoted to `ProxyCorrupt` off the audio thread. EOF is normal
(assets end at completed tail, §15.6): post-EOF fetches are fully-served silence.

## Consequences for P1G

* The playback-source selector publishes an immutable per-destination view (atomic `shared_ptr`,
  the `activeOwner_` pattern) holding at most one reader; source changes latch at block boundaries.
* Silent generations need **no reader at all** (zeros by definition).
* Offline mixdown uses `messageThread_ensureRangeReady` (message-thread blocking prefetch) so
  faster-than-realtime export never manufactures underruns.
* A device-rate change requires no reader rebuild (timeline-domain ring); the rebuild path exists
  regardless because readers are disposable derived state.
