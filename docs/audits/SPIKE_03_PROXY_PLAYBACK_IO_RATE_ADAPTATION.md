# SPIKE-03 — Proxy playback I/O and sample-rate adaptation (evidence gate for P1G)

Status: **decided** (2026-09-06). This report closes OI-002: it selects the bounded playback
mechanism required by steering §7.3/§15.3 (PI-030/PI-031) and records the measured bounds and
resulting P1 constants. Implementation: `src/instruments/ProxyPlaybackReader.h`; deterministic
evidence: the `spike03-*` selftests in `tests/selftest/MiniDAWSelftestsMain.cpp` (synthetic WAVs
only — no plugin rendering was repeated for this gate).

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
  sample 0 (span rule §15.6). Every output frame is a pure function of `t` (linear interpolation of
  the two neighbouring asset samples; bit-exact copy fast path when the ratio is 1.0), so seek,
  loop wrap and re-reads are bit-deterministic regardless of fill chunking or history — verified by
  the `spike03-seek` selftest, which replays overlapping ranges after forward seeks and loop wraps
  and requires exact equality with an independent reference computation.
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

### Conversion quality note

Fill-side conversion uses **linear interpolation (conversion v1)**. For proxy monitoring/stand-in
playback this is adequate (images ≈ −40 dB worst case at 44.1↔48 ratios); the mechanism keeps the
conversion entirely inside `convertRangeIntoRing`, so upgrading to a higher-order kernel later is a
local change that cannot affect the audio thread. Native-rate re-render remains the quality path
(steering §15.3) and is never a playback prerequisite.

## P1 constants (implemented in `ProxyPlaybackReader.h`)

| Constant | Value | Meaning |
|---|---|---|
| `kRingFrames` | 131072 (2^17) | ring capacity in timeline frames ≈ 2.73 s @ 48 kHz |
| `kReadAheadTargetFrames` | 65536 (2^16) | resident read-ahead ≈ 1.37 s @ 48 kHz |
| `kFillChunkFrames` | 8192 | frames converted per fill step (bounds scratch + round latency) |
| `kMaxConversionRatio` | 4.0 | corrupt-metadata guard; 44.1↔48↔96 are far inside |

**Per-reader memory bound:** ring 2 ch × 131072 × 4 B = **1.00 MiB** + fill scratch
2 ch × (8192 × 4 + 8) × 4 B ≈ **0.25 MiB** ⇒ **1.25 MiB/reader** (constant, independent of asset
length — verified with a 60 s asset). **Global bound:** one reader per instrument destination plus
one shared I/O thread; the 16-reader requirement costs **20.0 MiB** (measured).

## Measurements (Debug build, this machine, deterministic synthetic WAVs)

From the `spike03` selftest suite (all 673 selftest checks green, exit 0):

| Measurement | Result |
|---|---|
| 16 simultaneous readers, 64 blocks × 512 frames each consumed | all exact, **3.8 ms** total |
| 16-reader memory | **1.25 MiB/reader, 20.0 MiB total** (asserted < 24 MiB) |
| Cold far-seek → range resident (`seekReady`) | **0.26 ms** |
| `audioThread_fetch` cost per 512-frame block (resident) | **79 ns** |
| Sustained sequential stream, 10 s of material | **12.8 ms** (**779× realtime**), 0 underruns |
| Same-rate playback (ratio 1.0) | bit-exact vs the file (fast path) |
| 44.1 kHz asset → 48 kHz timeline and 48 kHz → 44.1 kHz | exact vs independent reference at head/mid/near-EOF |
| Seek + loop wrap + re-read | bit-identical to fresh reads (stateless mapping) |
| EOF | exact zeros, fully served, **never counted as underrun** |
| Real underrun (no fill service) | silence + counted + reported distinct from EOF; recovers when service resumes |
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
