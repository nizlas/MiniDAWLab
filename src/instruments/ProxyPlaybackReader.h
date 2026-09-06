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
//   LINEAR interpolation evaluated from the ABSOLUTE position p(t): every output
//   frame depends only on t, never on chunking or fill history, so seek, loop wrap
//   and re-reads are bit-deterministic by construction. ratio == 1.0 is a bit-exact
//   copy fast path. Asset samples outside [0, len) read as exactly 0.0f, so EOF and
//   pre-roll are silence without special cases (§15.6: no padding is stored).
//   NOTE: the mapping is anchored in TIMELINE frames — a device/engine-rate change
//   alone changes NOTHING here (PI-030): timeline integers are device-rate-agnostic
//   under TLD-1 and the ring is timeline-domain. Derived state (this reader) can
//   still be discarded/rebuilt freely without touching the generation.
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
//   ring: kRingFrames (2^17) frames * 2ch * 4B                  = 1.00 MiB
//   fill scratch: (kFillChunkFrames * maxRatio + 4) * 2ch * 4B  ≈ 0.07 MiB (ratio ≤ 2)
//   => ≈ 1.1 MiB per reader + one juce reader object; N readers cost N * 1.1 MiB and
//   share ONE I/O thread. 16 readers ≈ 17.6 MiB (measured in the SPIKE-03 selftest).
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

    /// [Any thread] True once the read-ahead window covers the last desired position
    /// (or the stream is entirely past EOF there). "Not ready" == ProxyPreparing.
    [[nodiscard]] bool isReadyAtDesired() const noexcept
    {
        const std::int64_t d = desired_.load(std::memory_order_acquire);
        if (d >= timelineEof_)
        {
            return true;
        }
        return validStart_.load(std::memory_order_acquire) <= d
               && d < validEnd_.load(std::memory_order_acquire);
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

        // Read-ahead satisfied (or EOF reached)?
        const std::int64_t target = d + kReadAheadTargetFrames;
        if (ve >= target || ve >= timelineEof_)
        {
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

    /// [Message thread] Offline-mixdown support: block until [t0, t0+n) is resident
    /// (or past EOF / failed / timeout). Never called on the audio thread.
    bool messageThread_ensureRangeReady(const std::int64_t t0, const int n,
                                        juce::WaitableEvent& serviceWake,
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
            serviceWake.signal();
            progress_.wait(2);
        }
    }

private:
    [[nodiscard]] int assetChunkCapacityFrames() const noexcept
    {
        // Widest asset window one fill chunk can require: n*ratio + interpolation guard.
        return static_cast<int>(std::ceil(kFillChunkFrames * kMaxConversionRatio)) + 8;
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

    /// [I/O thread] Read + convert timeline frames [from, to) into the ring.
    /// Stateless-by-position linear interpolation (see header) — deterministic for
    /// any chunking. Returns false on a read error.
    bool convertRangeIntoRing(const std::int64_t from, const std::int64_t to) noexcept
    {
        const double ratio = mapping_.ratio();
        const std::int64_t len = mapping_.assetLengthFrames;

        // Asset range this timeline range depends on (inclusive guard sample).
        const std::int64_t a0 = static_cast<std::int64_t>(
            std::floor(static_cast<double>(from) * ratio));
        std::int64_t a1 = static_cast<std::int64_t>(
            std::floor(static_cast<double>(to - 1) * ratio)) + 2;
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
        const bool exactCopy = juce::approximatelyEqual(ratio, 1.0);
        for (std::int64_t t = from; t < to; ++t)
        {
            const int slot = static_cast<int>(t & (kRingFrames - 1));
            if (exactCopy)
            {
                const std::int64_t ai = t - aStart;
                const bool in = t >= 0 && t < len && ai >= 0 && ai < aCount;
                ringL_[static_cast<size_t>(slot)] = in ? assetL_[static_cast<size_t>(ai)] : 0.0f;
                ringR_[static_cast<size_t>(slot)] = in ? assetR_[static_cast<size_t>(ai)] : 0.0f;
                continue;
            }
            const double p = static_cast<double>(t) * ratio;
            const std::int64_t i0 = static_cast<std::int64_t>(std::floor(p));
            const float frac = static_cast<float>(p - static_cast<double>(i0));
            const auto sampleAt = [&](const std::int64_t i, const std::vector<float>& buf) noexcept -> float
            {
                if (i < 0 || i >= len)
                {
                    return 0.0f;
                }
                const std::int64_t rel = i - aStart;
                return (rel >= 0 && rel < aCount) ? buf[static_cast<size_t>(rel)] : 0.0f;
            };
            ringL_[static_cast<size_t>(slot)]
                = sampleAt(i0, assetL_) * (1.0f - frac) + sampleAt(i0 + 1, assetL_) * frac;
            ringR_[static_cast<size_t>(slot)]
                = sampleAt(i0, assetR_) * (1.0f - frac) + sampleAt(i0 + 1, assetR_) * frac;
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

    std::atomic<std::int64_t> validStart_{ 0 };
    std::atomic<std::int64_t> validEnd_{ 0 };
    std::atomic<std::int64_t> desired_{ 0 };
    std::atomic<std::uint64_t> underruns_{ 0 };
    std::atomic<bool> streamFailed_{ false };
    std::atomic<std::int64_t> lastFetchStart_{ 0 };
    std::atomic<std::uint64_t> fetchCalls_{ 0 };
    juce::WaitableEvent progress_; ///< pulsed after each fill step (offline waits)
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
