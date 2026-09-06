#pragma once

// =============================================================================
// ProxyPlaybackReader — P1G bounded, realtime-safe proxy asset streaming
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §7.3, §15.3, PI-030/PI-031;
//  mechanism selected by SPIKE-03 — docs/audits/SPIKE_03_PROXY_PLAYBACK_IO_RATE_ADAPTATION.md)
// =============================================================================
//
// MECHANISM (SPIKE-03 selection): bounded read-ahead ring per reader, filled by ONE
// shared low-priority I/O thread (`ProxyPlaybackIoService`), with sample-rate
// conversion performed DURING THE FILL (off the audio thread). The ring holds
// TIMELINE-domain frames, so the audio thread consumes exactly one ring frame per
// output sample: its entire contract is atomics + memcpy + zero-fill. Unbounded
// full-file preload is rejected (steering §7.3); a separate short-asset preload
// mode is unnecessary because the ring serves all asset lengths in constant memory.
//
// SAMPLE-POSITION MAPPING (deterministic, stateless):
//   Proxy generations render from project start — asset sample 0 IS timeline
//   reference sample 0 (span rule §15.6). For timeline frame t the asset position
//   is  p(t) = t * ratio  with  ratio = assetRate / timelineRate  (the generation's
//   recorded render rate over the TLD-1 timeline reference rate). Conversion is
//   BAND-LIMITED Kaiser-windowed-sinc interpolation evaluated from the ABSOLUTE
//   position p(t): every output frame depends only on t, never on chunking or fill
//   history, so seek, loop wrap and re-reads are bit-deterministic by construction.
//   ratio == 1.0 is a bit-exact copy fast path. Asset samples outside [0, len) read
//   as exactly 0.0f, so EOF and pre-roll are silence without special cases (§15.6:
//   no padding is stored).
//   NOTE: the mapping is anchored in TIMELINE frames — a device/engine-rate change
//   alone changes NOTHING here (PI-030): timeline integers are device-rate-agnostic
//   under TLD-1 and the ring is timeline-domain. Derived state (this reader) can
//   still be discarded/rebuilt freely without touching the generation.
//
// CONVERSION QUALITY (P1G hardening — the proxy is the authoritative sound
//   reference when Primary is unavailable, so conversion must suit normal
//   full-quality music playback; linear interpolation is insufficient):
//   polyphase Kaiser-windowed sinc, kSincZeroCrossings=48 per side, 512 phases
//   per crossing (linear inter-phase interpolation, error < -100 dB), Kaiser
//   beta=10 (~98 dB design stopband), cutoff scale 0.938 of the LOWER Nyquist —
//   for downsampling the kernel is stretched so the stopband begins at the
//   OUTPUT Nyquist (proper anti-aliasing), for upsampling images are rejected
//   at the input Nyquist. Passband is flat (<0.01 dB) through ~19.3 kHz for the
//   44.1/48 pair. All coefficients live in one shared immutable table built on
//   first use OFF the audio thread; evaluation runs on the fill (I/O) thread
//   only. juce::Interpolators::WindowedSinc was evaluated and rejected: it keeps
//   a FIXED input-Nyquist cutoff (it aliases when downsampling 48->44.1) and its
//   stream state makes output depend on chunking, which would break this
//   reader's by-position determinism (seek == fresh reader). The Kaiser window
//   uses the same modified-Bessel-I0 series as juce::dsp::SpecialFunctions.
//
// PREPARED LOOP WRAP (P1G hardening — a known transport loop is NOT a seek):
//   the engine announces the active cycle [loopStart, loopEnd) through
//   audioThread_setPreparedLoop (atomics only). The I/O thread then keeps a
//   preallocated LOOP-HEAD cache of the first kLoopHeadFrames timeline frames
//   from loopStart resident (position-stateless conversion — identical bits to
//   the ring). When a callback crosses the boundary, the wrapped segment is
//   served from the loop head (seqlock-guarded copy, no locks/allocation) while
//   the ring recenters in the background: no zeros are inserted, no frames are
//   duplicated or dropped, and NO underrun is counted. An unprepared arbitrary
//   seek still recenters through the normal ProxyPreparing path. Genuine I/O
//   starvation (neither ring nor loop head can serve pre-EOF frames) still
//   counts and reports PlaybackUnderrun.
//
// AUDIO-THREAD GUARANTEES (PI-031, verified by selftests):
//   audioThread_fetch performs NO filesystem access, NO locks/mutexes, NO
//   allocation, NO file open/close, and never destroys anything. It publishes the
//   desired position (release), copies covered frames from the preallocated ring,
//   zero-fills anything not covered, and counts a real underrun ONLY for frames
//   before the asset's timeline EOF (post-EOF silence is normal, never an underrun).
//   An overwrite race with a concurrent forward window move is detected by
//   re-checking validStart after the copy and downgrading to silence + underrun.
//
// MEMORY BOUND (documented P1 constants; SPIKE-03 report):
//   ring: kRingFrames (2^17) frames * 2ch * 4B                        = 1.00 MiB
//   asset fill scratch: (kFillChunk*maxRatio + 2*maxSupport+16)*2*4B  ≈ 0.25 MiB
//   converted-output fill scratch: kFillChunkFrames * 2ch * 4B        ≈ 0.06 MiB
//   loop-head cache: kLoopHeadFrames * 2ch * 4B                       ≈ 0.13 MiB
//   shared sinc kernel table (once per process, immutable)            ≈ 0.10 MiB
//   => ≈ 1.45 MiB per reader + one juce reader object; N readers cost N * 1.45 MiB
//   and share ONE I/O thread. 16 readers ≈ 23.2 MiB (measured in the selftest).
//
// LIFECYCLE / RETIREMENT (Windows handle safety):
//   * ctor (message thread) opens and validates the WAV synchronously (header-level
//     I/O only); failure leaves openFailed() true and the reader is never published.
//   * fill runs only inside ProxyPlaybackIoService::run, which holds a shared_ptr
//     for the duration of one service round — the file handle can never be closed
//     mid-read.
//   * retirement: unregister from the service (message thread), publish the
//     replacement view, DRAIN the audio callback, then drop the last shared_ptr.
//     The destructor (closes the Windows handle, frees the ring) therefore always
//     runs on a non-audio thread. Callers must never let the audio thread hold the
//     last reference (the playback coordinator's retire list enforces this).
//
// UNDERRUN BEHAVIOR: missing pre-EOF frames are served as silence and counted;
// the stream recovers automatically when the fill catches up. A permanent read
// failure sets streamFailed() — the coordinator demotes the source to ProxyCorrupt
// off the audio thread. EOF is a normal state, distinct from underrun.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace proxy_playback
{

//==============================================================================
// P1 constants (SPIKE-03; kept together so the evidence report can cite them)
//==============================================================================
/// Ring capacity in timeline frames (power of two). 2^17 = 131072 ≈ 2.73 s @ 48 kHz.
inline constexpr int kRingFrames = 1 << 17;
/// Fill keeps [desired, desired + readAhead) resident. 2^16 ≈ 1.37 s @ 48 kHz.
inline constexpr int kReadAheadTargetFrames = 1 << 16;
/// Asset frames converted per fill iteration (bounds fill-scratch memory and the
/// service latency of one round).
inline constexpr int kFillChunkFrames = 8192;
/// Conversion ratios outside this bound are rejected at open (corrupt metadata guard;
/// legitimate 44.1↔48↔96 proxies are far inside it).
inline constexpr double kMaxConversionRatio = 4.0;
/// Loop-head cache capacity in timeline frames (≈ 0.34 s @ 48 kHz — covers many
/// ring-recenter service rounds after a prepared loop wrap).
inline constexpr int kLoopHeadFrames = 1 << 14;

//==============================================================================
// Band-limited conversion kernel (P1G hardening — see file header)
//==============================================================================
/// Sinc zero crossings per side. 2*48+1 taps at unit stretch => ~98 dB Kaiser
/// stopband with a transition narrow enough to keep 18 kHz flat for 44.1/48.
inline constexpr int kSincZeroCrossings = 48;
/// Table phases per zero crossing (linear inter-phase interpolation, < -100 dB error).
inline constexpr int kSincPhasesPerCrossing = 512;
/// Kaiser window beta (A ≈ 98 dB).
inline constexpr double kSincKaiserBeta = 10.0;
/// Kernel cutoff as a fraction of the lower of the two Nyquist frequencies.
/// 0.938 puts the stopband edge exactly at the output Nyquist for 48->44.1.
inline constexpr double kSincCutoffScale = 0.938;
/// Widest kernel half-support in ASSET samples (stretch bounded by kMaxConversionRatio).
inline constexpr int kSincMaxSupportFrames
    = static_cast<int>(kSincZeroCrossings * kMaxConversionRatio / kSincCutoffScale) + 2;

/// Modified Bessel function of the first kind, I0 — same series as
/// juce::dsp::SpecialFunctions::besselI0 (juce_dsp is not linked by every target
/// that uses this header, so the 10-line established polynomial is inlined).
inline double proxySincBesselI0(const double x) noexcept
{
    const double ax = std::abs(x);
    if (ax < 3.75)
    {
        double y = x / 3.75;
        y *= y;
        return 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492
               + y * (0.2659732 + y * (0.360768e-1 + y * 0.45813e-2)))));
    }
    const double y = 3.75 / ax;
    return (std::exp(ax) / std::sqrt(ax))
           * (0.39894228 + y * (0.1328592e-1 + y * (0.225319e-2 + y * (-0.157565e-2
              + y * (0.916281e-2 + y * (-0.2057706e-1 + y * (0.2635537e-1
              + y * (-0.1647633e-1 + y * 0.392377e-2))))))));
}

/// One shared immutable polyphase table: h(u) = sinc(u) * kaiser(u / Z) sampled at
/// u = k / kSincPhasesPerCrossing, k = 0 .. Z*phases (+1 guard zero). Built once on
/// first use (message thread constructs the first reader) — NEVER the audio thread.
class ProxySincKernelTable final
{
public:
    static const ProxySincKernelTable& instance()
    {
        static const ProxySincKernelTable table;
        return table;
    }

    /// Kernel value at |u| (u in sinc zero-crossing units). u must be >= 0.
    [[nodiscard]] float valueAt(const double u) const noexcept
    {
        const double idx = u * static_cast<double>(kSincPhasesPerCrossing);
        const auto k = static_cast<size_t>(idx);
        if (k + 1 >= table_.size())
        {
            return 0.0f;
        }
        const auto frac = static_cast<float>(idx - static_cast<double>(k));
        return table_[k] + frac * (table_[k + 1] - table_[k]);
    }

    /// Raw table pointer for the fill thread's flattened inner loop (Debug builds
    /// do not inline valueAt; per-tap call overhead would starve 16 readers).
    /// Valid lookups satisfy idx <= kSincZeroCrossings * kSincPhasesPerCrossing,
    /// with one guard zero after the last entry for the linear interpolation.
    [[nodiscard]] const float* data() const noexcept { return table_.data(); }

private:
    ProxySincKernelTable()
    {
        const int n = kSincZeroCrossings * kSincPhasesPerCrossing;
        table_.resize(static_cast<size_t>(n) + 2, 0.0f);
        const double i0Beta = proxySincBesselI0(kSincKaiserBeta);
        table_[0] = 1.0f;
        for (int k = 1; k <= n; ++k)
        {
            const double u = static_cast<double>(k) / kSincPhasesPerCrossing;
            const double x = juce::MathConstants<double>::pi * u;
            const double sinc = std::sin(x) / x;
            const double w = u / kSincZeroCrossings; // 0..1 across the support
            const double kaiser = proxySincBesselI0(kSincKaiserBeta * std::sqrt(1.0 - w * w)) / i0Beta;
            table_[static_cast<size_t>(k)] = static_cast<float>(sinc * kaiser);
        }
        // table_[n + 1] stays 0 (guard for the lookup's linear interpolation).
    }

    std::vector<float> table_;

    JUCE_DECLARE_NON_COPYABLE(ProxySincKernelTable)
};

/// Immutable mapping between the generation's recorded render rate and the TLD-1
/// timeline reference rate (steering §15.3). Never depends on the device rate.
struct ProxyStreamMapping
{
    double assetRate = 48000.0;    ///< generation's recorded render sample rate
    double timelineRate = 48000.0; ///< TLD-1 timeline reference rate
    std::int64_t assetLengthFrames = 0;

    [[nodiscard]] double ratio() const noexcept { return assetRate / timelineRate; }

    /// First timeline frame that maps entirely past the asset (EOF in timeline domain).
    [[nodiscard]] std::int64_t timelineEofFrames() const noexcept
    {
        if (assetLengthFrames <= 0)
        {
            return 0;
        }
        return static_cast<std::int64_t>(
            std::ceil(static_cast<double>(assetLengthFrames) / ratio()));
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return assetRate > 0.0 && timelineRate > 0.0 && std::isfinite(assetRate)
               && std::isfinite(timelineRate) && assetLengthFrames >= 0
               && ratio() <= kMaxConversionRatio && ratio() >= (1.0 / kMaxConversionRatio);
    }
};

//==============================================================================
// The reader
//==============================================================================
class ProxyPlaybackReader final
{
public:
    /// [Message thread] Opens + validates the asset synchronously (header I/O only).
    /// On failure `openFailed()` is true and the object must not be published to audio.
    ProxyPlaybackReader(const juce::File& assetFile, const ProxyStreamMapping& mapping)
        : mapping_(mapping)
    {
        ringL_.resize(static_cast<size_t>(kRingFrames), 0.0f);
        ringR_.resize(static_cast<size_t>(kRingFrames), 0.0f);
        const int chunkCap = assetChunkCapacityFrames();
        assetL_.resize(static_cast<size_t>(chunkCap), 0.0f);
        assetR_.resize(static_cast<size_t>(chunkCap), 0.0f);
        outL_.resize(static_cast<size_t>(kFillChunkFrames), 0.0f);
        outR_.resize(static_cast<size_t>(kFillChunkFrames), 0.0f);
        headL_.resize(static_cast<size_t>(kLoopHeadFrames), 0.0f);
        headR_.resize(static_cast<size_t>(kLoopHeadFrames), 0.0f);
        // Force the shared kernel table to exist BEFORE any fill thread needs it
        // (constructed here on the message thread; immutable afterwards).
        (void)ProxySincKernelTable::instance();

        if (!mapping_.valid())
        {
            openFailed_ = true;
            openError_ = "invalid stream mapping (rates/length/ratio)";
            return;
        }
        // Open the stream FIRST and guard null explicitly: juce::WavAudioFormatReader
        // dereferences the stream unconditionally. A freshly published file can be
        // transiently locked on Windows (AV scan) — retry briefly before failing.
        std::unique_ptr<juce::FileInputStream> stream;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            stream = assetFile.createInputStream();
            if (stream != nullptr)
            {
                break;
            }
            juce::Thread::sleep(10);
        }
        if (stream == nullptr)
        {
            openFailed_ = true;
            openError_ = "asset file could not be opened for reading";
            return;
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> r(
            wav.createReaderFor(stream.release(), true));
        if (r == nullptr)
        {
            openFailed_ = true;
            openError_ = "asset is not a readable WAV";
            return;
        }
        if (r->numChannels < 1 || static_cast<std::int64_t>(r->lengthInSamples)
                                       != mapping_.assetLengthFrames)
        {
            openFailed_ = true;
            openError_ = "asset shape does not match published metadata (channels/length)";
            return;
        }
        if (!juce::approximatelyEqual(r->sampleRate, mapping_.assetRate))
        {
            openFailed_ = true;
            openError_ = "asset sample rate does not match the recorded render rate";
            return;
        }
        reader_ = std::move(r);
        timelineEof_ = mapping_.timelineEofFrames();
    }

    /// Destructor closes the file handle — must run OFF the audio thread (see header).
    ~ProxyPlaybackReader() = default;

    ProxyPlaybackReader(const ProxyPlaybackReader&) = delete;
    ProxyPlaybackReader& operator=(const ProxyPlaybackReader&) = delete;

    [[nodiscard]] bool openFailed() const noexcept { return openFailed_; }
    [[nodiscard]] juce::String openError() const noexcept { return openError_; }
    [[nodiscard]] const ProxyStreamMapping& mapping() const noexcept { return mapping_; }
    [[nodiscard]] std::int64_t timelineEofFrames() const noexcept { return timelineEof_; }

    /// [Any thread] Permanent mid-stream read failure (disk error / file vanished).
    [[nodiscard]] bool streamFailed() const noexcept
    {
        return streamFailed_.load(std::memory_order_acquire);
    }

    /// [Any thread] Real pre-EOF underruns served as silence (EOF never counts).
    [[nodiscard]] std::uint64_t underrunCount() const noexcept
    {
        return underruns_.load(std::memory_order_relaxed);
    }

    /// [Any thread] True once the read-ahead window (or the prepared loop head)
    /// covers the last desired position (or the stream is entirely past EOF there).
    /// "Not ready" == ProxyPreparing.
    [[nodiscard]] bool isReadyAtDesired() const noexcept
    {
        const std::int64_t d = desired_.load(std::memory_order_acquire);
        if (d >= timelineEof_)
        {
            return true;
        }
        if (validStart_.load(std::memory_order_acquire) <= d
            && d < validEnd_.load(std::memory_order_acquire))
        {
            return true;
        }
        // A prepared loop head also counts as ready (a wrap is not "preparing").
        const std::int64_t hf = headFor_.load(std::memory_order_acquire);
        return hf >= 0 && d >= hf
               && d < hf + headValid_.load(std::memory_order_acquire);
    }

    //==========================================================================
    // Prepared loop wrapping (P1G hardening)
    //==========================================================================
    /// [Audio thread, any thread] Announce the active transport cycle so the I/O
    /// thread keeps the loop-start region resident BEFORE the playhead reaches the
    /// boundary. Atomics only — safe from the audio callback. A range change simply
    /// retargets the fill; the old head content stays valid for its own positions
    /// (conversion is position-stateless).
    void audioThread_setPreparedLoop(const std::int64_t loopStart,
                                     const std::int64_t loopEnd) noexcept
    {
        if (loopStart < 0 || loopEnd <= loopStart)
        {
            return;
        }
        if (loopStart_.load(std::memory_order_relaxed) != loopStart
            || loopEnd_.load(std::memory_order_relaxed) != loopEnd)
        {
            loopEnd_.store(loopEnd, std::memory_order_relaxed);
            loopStart_.store(loopStart, std::memory_order_release);
            // NO wake->signal() here: WaitableEvent::signal acquires a mutex
            // (PI-031 forbids it on the audio thread). The idle I/O service polls
            // every 20 ms — orders of magnitude inside the loop's lead time.
        }
    }

    /// [Any thread] True when the loop-head cache is resident for `loopStart`.
    [[nodiscard]] bool loopHeadReadyFor(const std::int64_t loopStart) const noexcept
    {
        return headFor_.load(std::memory_order_acquire) == loopStart
               && headValid_.load(std::memory_order_acquire) > 0;
    }

    /// [Message thread / tests] Block until the loop head announced via
    /// audioThread_setPreparedLoop is resident (or timeout / stream failure).
    bool messageThread_ensureLoopHeadReady(const int timeoutMs) noexcept
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        for (;;)
        {
            const std::int64_t ls = loopStart_.load(std::memory_order_acquire);
            if (streamFailed_.load(std::memory_order_acquire) || ls < 0)
            {
                return false;
            }
            if (loopHeadReadyFor(ls))
            {
                return true;
            }
            if (juce::Time::getMillisecondCounterHiRes() >= deadline)
            {
                return false;
            }
            if (auto* wake = serviceWake_.load(std::memory_order_acquire))
            {
                wake->signal();
            }
            progress_.wait(2);
        }
    }

    /// [Any thread] Diagnostics: the last timeline start the audio thread fetched.
    [[nodiscard]] std::int64_t lastFetchedTimelineStart() const noexcept
    {
        return lastFetchStart_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t fetchCallCount() const noexcept
    {
        return fetchCalls_.load(std::memory_order_relaxed);
    }

    //==========================================================================
    // AUDIO THREAD — the entire realtime contract
    //==========================================================================
    /// [Audio thread] Copy timeline frames [t0, t0+n) into dstL/dstR (COPY, not add).
    /// Frames not resident are zero-filled; pre-EOF misses count one underrun.
    /// Returns true when every requested pre-EOF frame was served from the ring.
    bool audioThread_fetch(const std::int64_t t0, float* dstL, float* dstR,
                           const int n) noexcept
    {
        if (n <= 0 || dstL == nullptr || dstR == nullptr)
        {
            return false;
        }
        fetchCalls_.fetch_add(1, std::memory_order_relaxed);
        lastFetchStart_.store(t0, std::memory_order_relaxed);
        // Publish the position we want resident so the fill thread tracks/seeks.
        desired_.store(t0, std::memory_order_release);

        // Entirely past EOF: normal silence, no ring dependency, no underrun.
        if (t0 >= timelineEof_)
        {
            zeroFill(dstL, dstR, 0, n);
            return true;
        }

        const std::int64_t vs = validStart_.load(std::memory_order_acquire);
        const std::int64_t ve = validEnd_.load(std::memory_order_acquire);
        const std::int64_t wantEnd = t0 + n;
        const std::int64_t copyStart = t0 < vs ? vs : t0;
        const std::int64_t copyEnd = wantEnd < ve ? wantEnd : ve;

        zeroFill(dstL, dstR, 0, n);
        bool full = false;
        if (copyStart < copyEnd)
        {
            copyFromRing(copyStart, copyEnd, dstL + (copyStart - t0), dstR + (copyStart - t0));
            // Overwrite-race check: the copied bytes are trustworthy only if the
            // window STILL covers the copied range afterwards (a concurrent forward
            // trim or a seek-reset may have rewritten those slots). Otherwise
            // downgrade to silence + underrun — never torn/garbage audio.
            const std::int64_t vsAfter = validStart_.load(std::memory_order_acquire);
            const std::int64_t veAfter = validEnd_.load(std::memory_order_acquire);
            if (vsAfter > copyStart || veAfter < copyEnd)
            {
                zeroFill(dstL, dstR, 0, n);
            }
            else
            {
                full = copyStart == t0 && copyEnd >= (wantEnd < timelineEof_ ? wantEnd : timelineEof_);
            }
        }
        // Prepared-loop head fallback: a wrapped segment lands here while the ring
        // is still recentered on the pre-wrap position. Identical bits to the ring
        // (conversion is position-stateless); seqlock guards a concurrent refill.
        if (!full)
        {
            const std::int64_t effEnd = wantEnd < timelineEof_ ? wantEnd : timelineEof_;
            const std::uint32_t seq0 = headSeq_.load(std::memory_order_acquire);
            if ((seq0 & 1u) == 0u)
            {
                const std::int64_t hf = headFor_.load(std::memory_order_acquire);
                const std::int64_t hv = headValid_.load(std::memory_order_acquire);
                if (hf >= 0 && t0 >= hf && effEnd <= hf + hv)
                {
                    const auto off = static_cast<size_t>(t0 - hf);
                    const auto run = static_cast<size_t>(effEnd - t0);
                    std::memcpy(dstL, headL_.data() + off, sizeof(float) * run);
                    std::memcpy(dstR, headR_.data() + off, sizeof(float) * run);
                    if (static_cast<int>(run) < n)
                    {
                        zeroFill(dstL, dstR, static_cast<int>(run), n - static_cast<int>(run));
                    }
                    if (headSeq_.load(std::memory_order_acquire) == seq0)
                    {
                        full = true;
                    }
                    else
                    {
                        zeroFill(dstL, dstR, 0, n); // torn refill — silence, counted below
                    }
                }
            }
        }
        if (!full && t0 < timelineEof_)
        {
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        return full;
    }

    //==========================================================================
    // FILL (I/O service thread) — never the audio thread
    //==========================================================================
    /// [I/O thread] One bounded service step: recenters the window on a seek and
    /// converts up to one chunk. Returns true when it made progress (idle backoff).
    bool serviceOnce() noexcept
    {
        if (reader_ == nullptr || streamFailed_.load(std::memory_order_acquire))
        {
            return false;
        }
        const std::int64_t d = desired_.load(std::memory_order_acquire);
        std::int64_t vs = validStart_.load(std::memory_order_relaxed);
        std::int64_t ve = validEnd_.load(std::memory_order_relaxed);

        // Seek / first fill: desired is outside the current window (any backward
        // move, or a forward jump past the resident data) -> recenter atomically:
        // empty the window FIRST (audio sees "not resident" -> silence), then refill.
        if (d < vs || d > ve)
        {
            validStart_.store(d, std::memory_order_release);
            validEnd_.store(d, std::memory_order_release);
            vs = d;
            ve = d;
        }

        // Read-ahead satisfied (or EOF reached)? Use the idle time to keep the
        // prepared loop head resident (prefetch BEFORE the playhead reaches the
        // boundary), then report idle.
        const std::int64_t target = d + kReadAheadTargetFrames;
        if (ve >= target || ve >= timelineEof_)
        {
            if (maybeFillLoopHead())
            {
                notifyProgress();
                return true;
            }
            notifyProgress();
            return false;
        }

        // Convert one chunk [ve, chunkEnd).
        std::int64_t chunkEnd = ve + kFillChunkFrames;
        if (chunkEnd > target)
        {
            chunkEnd = target;
        }
        if (chunkEnd > timelineEof_)
        {
            chunkEnd = timelineEof_;
        }
        // Ring-capacity guard: never overwrite frames the audio thread may read
        // this block ([d, d+deviceBlock)). Keeping the window within kRingFrames
        // of `d` guarantees it (read-ahead target is half the ring).
        if (chunkEnd - d > static_cast<std::int64_t>(kRingFrames))
        {
            chunkEnd = d + kRingFrames;
        }
        if (chunkEnd <= ve)
        {
            return false;
        }
        if (!convertRangeIntoRing(ve, chunkEnd))
        {
            streamFailed_.store(true, std::memory_order_release);
            notifyProgress();
            return false;
        }
        // Trim the tail if the window now exceeds ring capacity (forward overwrite).
        const std::int64_t newStart = chunkEnd - kRingFrames;
        if (newStart > vs)
        {
            validStart_.store(newStart, std::memory_order_release);
        }
        validEnd_.store(chunkEnd, std::memory_order_release);
        notifyProgress();
        return true;
    }

    /// [I/O service, message thread] The service installs its wake event at
    /// registration so blocking prefetch can nudge it without a service handle.
    void setServiceWakeEvent(juce::WaitableEvent* wakeOrNull) noexcept
    {
        serviceWake_.store(wakeOrNull, std::memory_order_release);
    }

    /// [Message thread] Offline-mixdown support: block until [t0, t0+n) is resident
    /// (or past EOF / failed / timeout). Never called on the audio thread.
    bool messageThread_ensureRangeReady(const std::int64_t t0, const int n,
                                        const int timeoutMs) noexcept
    {
        const std::int64_t end = t0 + n;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        desired_.store(t0, std::memory_order_release);
        for (;;)
        {
            if (streamFailed_.load(std::memory_order_acquire))
            {
                return false;
            }
            const std::int64_t effEnd = end < timelineEof_ ? end : timelineEof_;
            if (t0 >= timelineEof_
                || (validStart_.load(std::memory_order_acquire) <= t0
                    && effEnd <= validEnd_.load(std::memory_order_acquire)))
            {
                return true;
            }
            if (juce::Time::getMillisecondCounterHiRes() >= deadline)
            {
                return false;
            }
            if (auto* wake = serviceWake_.load(std::memory_order_acquire))
            {
                wake->signal();
            }
            progress_.wait(2);
        }
    }

private:
    [[nodiscard]] int assetChunkCapacityFrames() const noexcept
    {
        // Widest asset window one fill chunk can require: n*ratio + the sinc
        // kernel's half-support on both sides + rounding guard.
        return static_cast<int>(std::ceil(kFillChunkFrames * kMaxConversionRatio))
               + 2 * kSincMaxSupportFrames + 16;
    }

    static void zeroFill(float* l, float* r, const int off, const int n) noexcept
    {
        std::memset(l + off, 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(r + off, 0, sizeof(float) * static_cast<size_t>(n));
    }

    void copyFromRing(const std::int64_t from, const std::int64_t to, float* dstL,
                      float* dstR) noexcept
    {
        std::int64_t t = from;
        while (t < to)
        {
            const int slot = static_cast<int>(t & (kRingFrames - 1));
            const int run = static_cast<int>(
                std::min<std::int64_t>(to - t, kRingFrames - slot));
            std::memcpy(dstL + (t - from), ringL_.data() + slot,
                        sizeof(float) * static_cast<size_t>(run));
            std::memcpy(dstR + (t - from), ringR_.data() + slot,
                        sizeof(float) * static_cast<size_t>(run));
            t += run;
        }
    }

    /// [I/O thread] Read + convert timeline frames [from, to) into linear buffers
    /// (to - from <= kFillChunkFrames). Position-stateless band-limited Kaiser-
    /// windowed-sinc interpolation (see header) — deterministic for any chunking.
    /// ratio == 1.0 is a bit-exact copy. Returns false on a read error.
    bool convertRangeLinear(const std::int64_t from, const std::int64_t to,
                            float* dstL, float* dstR) noexcept
    {
        const double ratio = mapping_.ratio();
        const std::int64_t len = mapping_.assetLengthFrames;
        const bool exactCopy = juce::approximatelyEqual(ratio, 1.0);
        // Kernel stretch: cutoff at the LOWER Nyquist (anti-aliasing for
        // downsampling, image rejection for upsampling), scaled for margin.
        const double stretch = kSincCutoffScale * (ratio > 1.0 ? 1.0 / ratio : 1.0);
        const double halfSupport
            = exactCopy ? 1.0 : static_cast<double>(kSincZeroCrossings) / stretch;

        // Asset range this timeline range depends on (kernel support guard).
        const std::int64_t a0 = static_cast<std::int64_t>(
            std::floor(static_cast<double>(from) * ratio - halfSupport)) - 1;
        std::int64_t a1 = static_cast<std::int64_t>(
            std::floor(static_cast<double>(to - 1) * ratio + halfSupport)) + 2;
        const std::int64_t aStart = a0 < 0 ? 0 : (a0 > len ? len : a0);
        if (a1 > len)
        {
            a1 = len;
        }
        const int aCount = static_cast<int>(a1 > aStart ? a1 - aStart : 0);
        if (aCount > static_cast<int>(assetL_.size()))
        {
            // Structural bound violated — treat as failure rather than allocate.
            return false;
        }
        if (aCount > 0)
        {
            float* chans[2] = { assetL_.data(), assetR_.data() };
            if (!reader_->read(chans, 2, aStart, aCount))
            {
                return false;
            }
            if (reader_->numChannels < 2)
            {
                std::memcpy(assetR_.data(), assetL_.data(),
                            sizeof(float) * static_cast<size_t>(aCount));
            }
        }
        // Flattened raw-pointer inner loop: Debug builds (/Ob0) inline nothing, so
        // per-tap function calls would starve 16 readers on the single I/O thread.
        const auto& kernel = ProxySincKernelTable::instance();
        const float* const tbl = kernel.data();
        const float* const srcL = assetL_.data();
        const float* const srcR = assetR_.data();
        const double idxStep = stretch * static_cast<double>(kSincPhasesPerCrossing);
        for (std::int64_t t = from; t < to; ++t)
        {
            const auto out = static_cast<size_t>(t - from);
            if (exactCopy)
            {
                const std::int64_t ai = t - aStart;
                const bool in = t >= 0 && t < len && ai >= 0 && ai < aCount;
                dstL[out] = in ? srcL[static_cast<size_t>(ai)] : 0.0f;
                dstR[out] = in ? srcR[static_cast<size_t>(ai)] : 0.0f;
                continue;
            }
            const double p = static_cast<double>(t) * ratio;
            std::int64_t i0 = static_cast<std::int64_t>(std::ceil(p - halfSupport));
            std::int64_t i1 = static_cast<std::int64_t>(std::floor(p + halfSupport));
            // Samples outside [0, len) are exactly zero — skip them entirely. The
            // scratch window always covers [i0, i1] within [0, len) by construction.
            if (i0 < 0)
            {
                i0 = 0;
            }
            if (i1 >= len)
            {
                i1 = len - 1;
            }
            double accL = 0.0;
            double accR = 0.0;
            // Signed table position walks linearly with i: |a| is the lookup index.
            double a = (static_cast<double>(i0) - p) * idxStep; // negative until i > p
            std::int64_t rel = i0 - aStart;
            for (std::int64_t i = i0; i <= i1; ++i, a += idxStep, ++rel)
            {
                const double idx = a < 0.0 ? -a : a;
                const auto k = static_cast<int>(idx);
                const auto frac = static_cast<float>(idx - static_cast<double>(k));
                const double w
                    = static_cast<double>(tbl[k] + frac * (tbl[k + 1] - tbl[k]));
                accL += static_cast<double>(srcL[rel]) * w;
                accR += static_cast<double>(srcR[rel]) * w;
            }
            dstL[out] = static_cast<float>(accL * stretch);
            dstR[out] = static_cast<float>(accR * stretch);
        }
        return true;
    }

    /// [I/O thread] convertRangeLinear into the fill scratch, then scatter into the
    /// position-masked ring slots (at most two contiguous memcpy spans).
    bool convertRangeIntoRing(const std::int64_t from, const std::int64_t to) noexcept
    {
        std::int64_t t = from;
        while (t < to)
        {
            const auto run = static_cast<int>(
                std::min<std::int64_t>(to - t, kFillChunkFrames));
            if (!convertRangeLinear(t, t + run, outL_.data(), outR_.data()))
            {
                return false;
            }
            int done = 0;
            while (done < run)
            {
                const int slot = static_cast<int>((t + done) & (kRingFrames - 1));
                const int span = std::min(run - done, kRingFrames - slot);
                std::memcpy(ringL_.data() + slot, outL_.data() + done,
                            sizeof(float) * static_cast<size_t>(span));
                std::memcpy(ringR_.data() + slot, outR_.data() + done,
                            sizeof(float) * static_cast<size_t>(span));
                done += span;
            }
            t += run;
        }
        return true;
    }

    /// [I/O thread] Keep the announced loop head resident. Returns true when it
    /// (re)built the cache this step. Seqlock (odd = writing) guards audio reads.
    bool maybeFillLoopHead() noexcept
    {
        const std::int64_t ls = loopStart_.load(std::memory_order_acquire);
        if (ls < 0 || headFor_.load(std::memory_order_relaxed) == ls)
        {
            return false;
        }
        const std::int64_t le = loopEnd_.load(std::memory_order_acquire);
        const auto frames = static_cast<std::int64_t>(
            std::min<std::int64_t>(kLoopHeadFrames, le - ls));
        headSeq_.fetch_add(1, std::memory_order_acq_rel); // odd: writing
        headValid_.store(0, std::memory_order_release);
        bool ok = true;
        std::int64_t off = 0;
        while (ok && off < frames)
        {
            const auto run = static_cast<int>(
                std::min<std::int64_t>(frames - off, kFillChunkFrames));
            ok = convertRangeLinear(ls + off, ls + off + run,
                                    headL_.data() + off, headR_.data() + off);
            off += run;
        }
        headFor_.store(ls, std::memory_order_release);
        headValid_.store(ok ? frames : 0, std::memory_order_release);
        headSeq_.fetch_add(1, std::memory_order_release); // even: stable
        if (!ok)
        {
            streamFailed_.store(true, std::memory_order_release);
        }
        return true;
    }

    void notifyProgress() noexcept { progress_.signal(); }

    ProxyStreamMapping mapping_;
    std::unique_ptr<juce::AudioFormatReader> reader_;
    bool openFailed_ = false;
    juce::String openError_;
    std::int64_t timelineEof_ = 0;

    std::vector<float> ringL_, ringR_;   ///< preallocated timeline-domain ring
    std::vector<float> assetL_, assetR_; ///< preallocated fill/conversion scratch
    std::vector<float> outL_, outR_;     ///< preallocated converted-output scratch
    std::vector<float> headL_, headR_;   ///< preallocated prepared-loop head cache

    std::atomic<std::int64_t> validStart_{ 0 };
    std::atomic<std::int64_t> validEnd_{ 0 };
    std::atomic<std::int64_t> desired_{ 0 };
    std::atomic<std::uint64_t> underruns_{ 0 };
    std::atomic<bool> streamFailed_{ false };
    std::atomic<std::int64_t> lastFetchStart_{ 0 };
    std::atomic<std::uint64_t> fetchCalls_{ 0 };
    std::atomic<juce::WaitableEvent*> serviceWake_{ nullptr };
    juce::WaitableEvent progress_; ///< pulsed after each fill step (offline waits)

    // Prepared loop wrapping (see header): announced range + head-cache state.
    std::atomic<std::int64_t> loopStart_{ -1 };
    std::atomic<std::int64_t> loopEnd_{ -1 };
    std::atomic<std::int64_t> headFor_{ -1 };  ///< loopStart the head is built for
    std::atomic<std::int64_t> headValid_{ 0 }; ///< frames resident from headFor_
    std::atomic<std::uint32_t> headSeq_{ 0 };  ///< seqlock: odd while I/O rewrites
};

//==============================================================================
// Shared low-priority I/O service (ONE thread for every proxy reader)
//==============================================================================
class ProxyPlaybackIoService final : private juce::Thread
{
public:
    ProxyPlaybackIoService() : juce::Thread("ProxyPlaybackIO")
    {
        startThread(juce::Thread::Priority::low);
    }

    ~ProxyPlaybackIoService() override { shutdown(); }

    /// [Message thread] Stop servicing and join. Safe to call repeatedly.
    void shutdown()
    {
        signalThreadShouldExit();
        wake_.signal();
        stopThread(10000);
    }

    /// [Message thread] Begin servicing `reader`. The service keeps its own ref.
    void registerReader(std::shared_ptr<ProxyPlaybackReader> reader)
    {
        if (reader == nullptr || reader->openFailed())
        {
            return;
        }
        reader->setServiceWakeEvent(&wake_);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            readers_.push_back(std::move(reader));
        }
        wake_.signal();
    }

    /// [Message thread] Stop servicing `reader`. The caller still owns its ref; the
    /// service's ref drops after the current round (file close stays off-audio).
    void unregisterReader(const ProxyPlaybackReader* reader)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < readers_.size(); ++i)
        {
            if (readers_[i].get() == reader)
            {
                readers_[i]->setServiceWakeEvent(nullptr);
                readers_.erase(readers_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    /// [Message thread] Wake the service immediately (offline prefetch pacing).
    juce::WaitableEvent& wakeEvent() noexcept { return wake_; }

    /// [Tests] Number of currently registered readers.
    [[nodiscard]] size_t registeredCount() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return readers_.size();
    }

private:
    void run() override
    {
        std::vector<std::shared_ptr<ProxyPlaybackReader>> local;
        while (!threadShouldExit())
        {
            local.clear();
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                local = readers_;
            }
            bool progressed = false;
            for (const auto& r : local)
            {
                if (threadShouldExit())
                {
                    break;
                }
                // Bounded steps per reader per round keep round latency fair.
                for (int step = 0; step < 4 && r->serviceOnce(); ++step)
                {
                    progressed = true;
                }
            }
            if (!progressed)
            {
                wake_.wait(20);
            }
        }
    }

    mutable std::mutex mutex_; ///< message thread + service thread ONLY (never audio)
    std::vector<std::shared_ptr<ProxyPlaybackReader>> readers_;
    juce::WaitableEvent wake_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProxyPlaybackIoService)
};

} // namespace proxy_playback
