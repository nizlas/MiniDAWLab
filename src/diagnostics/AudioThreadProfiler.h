#pragma once

// =============================================================================
// AudioThreadProfiler â€” explicitly enabled, bounded cost attribution inside the audio callback
// =============================================================================
// Diagnostics only (`--stability-perf-profile`). OFF by default: every audio-thread call site
// pays ONE relaxed atomic load and nothing else. When enabled, the callback takes two clock
// reads per measured call and folds the elapsed time into preallocated relaxed atomics:
//
//   * per registered INSTANCE (instrument host or insert plug-in, keyed by TrackId + slot) â€”
//     calls / summed ms / worst ms, split by category;
//   * per CATEGORY per block â€” the exclusive time spent inside plug-in `processBlock` calls of
//     instruments, inside the proxy fetch/mix, and inside insert `processBlock` calls; the
//     block's remainder (callback total âˆ’ those three) is "DAL work" (MIDI scheduling, clip
//     rendering, routing, summing, meters, bookkeeping);
//   * per PHASE per block â€” INCLUSIVE wall time of the callback's sections (begin-block,
//     live MIDI, clip render incl. the audio rows' inserts, transport MIDI scheduling,
//     instrument mix incl. the instrument rows' inserts, bus finalize incl. the bus inserts),
//     so the remainder can be located. Phases overlap categories by design and are never
//     added to them;
//   * the WORST block (highest callback total) with its own category breakdown, so maxima are
//     reported from one real block instead of summing independent maxima;
//   * callback START intervals (late device callbacks) â€” a block that starts late overruns
//     the device deadline even when its own duration is within budget.
//
// Audio-thread contract: no allocation, no locks, no I/O; fixed arrays; relaxed atomics only.
// Registration (names) and reporting run on the message thread. A lost update during a
// message-thread reset is acceptable for diagnostics (same discipline as PlaybackEngine's load
// window).
//
// THREADS: instrument generation may run on render workers (engine/InstrumentRenderPool.h), so
// every fold a job can perform (`audioThread_addInstance`) uses integer-nanosecond fetch_add /
// CAS-max atomics; the per-block category scratch is atomic too (reset by the callback thread at
// block begin, read after its job join). Category sums are therefore SUMMED CPU TIME across all
// threads â€” not callback wall time; the parallel-section fields below carry the wall time of the
// dispatch â†’ join window and the callback's idle wait inside it.
// =============================================================================

#include "domain/Track.h"
#include "plugins/InsertSlotId.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace audio_profiler
{
enum class Category : int
{
    InstrumentPlugin = 0, ///< `AudioPluginInstance::processBlock` of an instrument host (live Primary / Secondary)
    ProxyMix = 1,         ///< proxy fetch + mix replacing the instrument generation stage
    InsertPlugin = 2,     ///< `processBlock` of one insert plug-in (audio / instrument / bus rows)
    Count = 3,
};

enum class Phase : int
{
    InstrumentBeginBlock = 0,
    LiveMidi,
    ClipRender,     ///< audio rows: clips + pre-gain + their inserts + fan-out (inclusive)
    MidiSchedule,   ///< transport MIDI scheduling into instrument hosts (+ MIDI source rows)
    Monitor,        ///< live input monitoring pass
    InstrumentMix,  ///< instrument rows: host (plug-in or proxy) + their inserts + fan-out (inclusive)
    Finalize,       ///< group / master bus strips incl. their inserts, device output
    Count,
};

[[nodiscard]] inline const char* categoryName(const Category c) noexcept
{
    switch (c)
    {
        case Category::InstrumentPlugin: return "instrument-plugin";
        case Category::ProxyMix: return "proxy-mix";
        case Category::InsertPlugin: return "insert-plugin";
        case Category::Count: break;
    }
    return "?";
}

[[nodiscard]] inline const char* phaseName(const Phase p) noexcept
{
    switch (p)
    {
        case Phase::InstrumentBeginBlock: return "instrument-begin-block";
        case Phase::LiveMidi: return "live-midi";
        case Phase::ClipRender: return "clip-render(incl. audio inserts)";
        case Phase::MidiSchedule: return "midi-schedule";
        case Phase::Monitor: return "monitor";
        case Phase::InstrumentMix: return "instrument-mix(incl. inserts)";
        case Phase::Finalize: return "finalize(buses incl. inserts)";
        case Phase::Count: break;
    }
    return "?";
}

class AudioThreadProfiler
{
public:
    static constexpr int kMaxInstances = 512;
    static constexpr int kCategories = static_cast<int>(Category::Count);
    static constexpr int kPhases = static_cast<int>(Phase::Count);

    static AudioThreadProfiler& get() noexcept
    {
        static AudioThreadProfiler instance;
        return instance;
    }

    // ------------------------------------------------------------------ message thread
    /// Enabling resets every window so the first snapshot measures from this moment.
    void setEnabled(const bool on) noexcept
    {
        if (on)
        {
            resetWindows();
        }
        enabled_.store(on, std::memory_order_relaxed);
    }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    /// Returns the instance index for (category, trackId, slotId), creating it when new, or âˆ’1
    /// when the table is full. Re-registering an existing key refreshes its name only.
    int registerInstance(const Category category, const TrackId trackId, const InsertSlotId slotId,
                         const juce::String& name)
    {
        const int n = registered_.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
        {
            Instance& in = instances_[(size_t)i];
            if (in.category == category && in.trackId == trackId && in.slotId == slotId)
            {
                in.name = name;
                return i;
            }
        }
        if (n >= kMaxInstances)
        {
            return -1;
        }
        Instance& in = instances_[(size_t)n];
        in.category = category;
        in.trackId = trackId;
        in.slotId = slotId;
        in.name = name;
        registered_.store(n + 1, std::memory_order_release);
        return n;
    }

    struct InstanceReport
    {
        Category category = Category::InstrumentPlugin;
        TrackId trackId = kInvalidTrackId;
        InsertSlotId slotId = kInvalidInsertSlotId;
        juce::String name;
        std::array<std::uint64_t, kCategories> calls{};
        std::array<double, kCategories> sumMs{};
        std::array<double, kCategories> maxMs{};
        [[nodiscard]] double totalMs() const noexcept { return sumMs[0] + sumMs[1] + sumMs[2]; }
        [[nodiscard]] std::uint64_t totalCalls() const noexcept { return calls[0] + calls[1] + calls[2]; }
    };

    struct Report
    {
        std::uint64_t blocks = 0;
        double totalSumMs = 0.0;
        double totalMaxMs = 0.0;
        /// Summed CPU time of the category's calls (across every thread that ran them).
        std::array<double, kCategories> categorySumMs{};
        std::array<double, kCategories> categoryMaxMs{};
        std::array<std::uint64_t, kCategories> categoryCalls{};
        /// Callback wall time minus the callback THREAD's own share of the category calls plus the
        /// parallel section's wall time — i.e. everything the callback thread did outside plug-in /
        /// proxy calls and outside waiting for / running the generation jobs.
        double remainderSumMs = 0.0;
        double remainderMaxMs = 0.0;
        std::array<double, kPhases> phaseSumMs{};
        std::array<double, kPhases> phaseMaxMs{};
        /// Parallel generation section (dispatch → every job joined): wall time, the callback
        /// thread's idle wait inside the join, jobs per block, blocks that ran in parallel.
        double parallelWallSumMs = 0.0;
        double parallelWallMaxMs = 0.0;
        double parallelWaitSumMs = 0.0;
        double parallelWaitMaxMs = 0.0;
        std::uint64_t parallelBlocks = 0;
        std::uint64_t parallelJobs = 0;
        /// The single block with the highest callback total and its own breakdown.
        double worstTotalMs = 0.0;
        std::array<double, kCategories> worstCategoryMs{};
        double worstRemainderMs = 0.0;
        double worstParallelWallMs = 0.0;
        int worstBlockSamples = 0;
        /// Start-to-start intervals between consecutive callbacks.
        std::uint64_t intervals = 0;
        double intervalSumMs = 0.0;
        double intervalMaxMs = 0.0;
        std::uint32_t lateStarts = 0; ///< intervals > 1.25 × the block's nominal period
        std::vector<InstanceReport> instances; ///< every registered instance (unsorted)
    };

    /// Copies and clears every window (instances keep their registration).
    [[nodiscard]] Report snapshotAndReset()
    {
        Report r;
        r.blocks = blocks_.exchange(0, std::memory_order_relaxed);
        r.totalSumMs = nsToMs(totalSumNs_.exchange(0, std::memory_order_relaxed));
        r.totalMaxMs = nsToMs(totalMaxNs_.exchange(0, std::memory_order_relaxed));
        for (int c = 0; c < kCategories; ++c)
        {
            r.categorySumMs[(size_t)c] = nsToMs(catSumNs_[(size_t)c].exchange(0, std::memory_order_relaxed));
            r.categoryMaxMs[(size_t)c] = nsToMs(catMaxNs_[(size_t)c].exchange(0, std::memory_order_relaxed));
            r.categoryCalls[(size_t)c] = catCalls_[(size_t)c].exchange(0, std::memory_order_relaxed);
            r.worstCategoryMs[(size_t)c] = nsToMs(worstCatNs_[(size_t)c].exchange(0, std::memory_order_relaxed));
        }
        r.remainderSumMs = nsToMs(remSumNs_.exchange(0, std::memory_order_relaxed));
        r.remainderMaxMs = nsToMs(remMaxNs_.exchange(0, std::memory_order_relaxed));
        for (int p = 0; p < kPhases; ++p)
        {
            r.phaseSumMs[(size_t)p] = nsToMs(phaseSumNs_[(size_t)p].exchange(0, std::memory_order_relaxed));
            r.phaseMaxMs[(size_t)p] = nsToMs(phaseMaxNs_[(size_t)p].exchange(0, std::memory_order_relaxed));
        }
        r.parallelWallSumMs = nsToMs(parWallSumNs_.exchange(0, std::memory_order_relaxed));
        r.parallelWallMaxMs = nsToMs(parWallMaxNs_.exchange(0, std::memory_order_relaxed));
        r.parallelWaitSumMs = nsToMs(parWaitSumNs_.exchange(0, std::memory_order_relaxed));
        r.parallelWaitMaxMs = nsToMs(parWaitMaxNs_.exchange(0, std::memory_order_relaxed));
        r.parallelBlocks = parBlocks_.exchange(0, std::memory_order_relaxed);
        r.parallelJobs = parJobs_.exchange(0, std::memory_order_relaxed);
        r.worstTotalMs = nsToMs(worstTotalNs_.exchange(0, std::memory_order_relaxed));
        r.worstRemainderMs = nsToMs(worstRemNs_.exchange(0, std::memory_order_relaxed));
        r.worstParallelWallMs = nsToMs(worstParWallNs_.exchange(0, std::memory_order_relaxed));
        r.worstBlockSamples = worstSamples_.exchange(0, std::memory_order_relaxed);
        r.intervals = intervals_.exchange(0, std::memory_order_relaxed);
        r.intervalSumMs = nsToMs(intervalSumNs_.exchange(0, std::memory_order_relaxed));
        r.intervalMaxMs = nsToMs(intervalMaxNs_.exchange(0, std::memory_order_relaxed));
        r.lateStarts = lateStarts_.exchange(0, std::memory_order_relaxed);
        const int n = registered_.load(std::memory_order_acquire);
        r.instances.reserve((size_t)n);
        for (int i = 0; i < n; ++i)
        {
            Instance& in = instances_[(size_t)i];
            InstanceReport ir;
            ir.category = in.category;
            ir.trackId = in.trackId;
            ir.slotId = in.slotId;
            ir.name = in.name;
            for (int c = 0; c < kCategories; ++c)
            {
                ir.calls[(size_t)c] = in.calls[(size_t)c].exchange(0, std::memory_order_relaxed);
                ir.sumMs[(size_t)c] = nsToMs(in.sumNs[(size_t)c].exchange(0, std::memory_order_relaxed));
                ir.maxMs[(size_t)c] = nsToMs(in.maxNs[(size_t)c].exchange(0, std::memory_order_relaxed));
            }
            r.instances.push_back(std::move(ir));
        }
        return r;
    }

    // ------------------------------------------------------------------ audio thread + render workers
    [[nodiscard]] bool audioThread_enabled() const noexcept
    {
        return enabled_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static std::int64_t ticks() noexcept { return juce::Time::getHighResolutionTicks(); }

    /// [Callback thread] Start of a block: reset the per-block scratch (no job runs before this).
    void audioThread_beginBlock(const std::int64_t startTicks) noexcept
    {
        for (auto& v : blockCatNs_) { v.store(0, std::memory_order_relaxed); }
        for (auto& v : blockCalls_) { v.store(0, std::memory_order_relaxed); }
        blockCallbackThreadCatNs_ = 0;
        for (auto& v : blockPhaseNs_) { v = 0; }
        blockParWallNs_ = 0;
        blockParWaitNs_ = 0;
        blockParJobs_ = 0;
        blockWasParallel_ = false;
        if (lastStartTicks_ != 0)
        {
            blockIntervalNs_ = ticksToNs(startTicks - lastStartTicks_);
        }
        else
        {
            blockIntervalNs_ = -1;
        }
        lastStartTicks_ = startTicks;
    }

    /// [Any render thread] Set by the render pool around every generation job (worker or the
    /// participating callback thread): folds made inside a job are summed CPU time covered by the
    /// generation section's wall, not extra callback wall time.
    static void setInsideGenerationJob(const bool inside) noexcept { tlsInsideGenerationJob_ = inside; }
    [[nodiscard]] static bool isInsideGenerationJob() noexcept { return tlsInsideGenerationJob_; }

    /// [Callback thread or a render worker] Fold one measured call. `slot` may be −1 (unregistered →
    /// category totals only).
    void audioThread_addInstance(const int slot, const Category category, const std::int64_t startTicks) noexcept
    {
        const std::int64_t ns = ticksToNs(ticks() - startTicks);
        const auto c = (size_t)category;
        blockCatNs_[c].fetch_add(ns, std::memory_order_relaxed);
        blockCalls_[c].fetch_add(1, std::memory_order_relaxed);
        if (!tlsInsideGenerationJob_)
        {
            blockCallbackThreadCatNs_ += ns; // callback thread, outside the generation section
        }
        if (slot >= 0 && slot < kMaxInstances)
        {
            Instance& in = instances_[(size_t)slot];
            in.calls[c].fetch_add(1, std::memory_order_relaxed);
            in.sumNs[c].fetch_add(ns, std::memory_order_relaxed);
            casMax(in.maxNs[c], ns);
        }
    }

    /// [Callback thread] Inclusive wall time of one callback section.
    void audioThread_addPhase(const Phase phase, const std::int64_t startTicks) noexcept
    {
        blockPhaseNs_[(size_t)phase] += ticksToNs(ticks() - startTicks);
    }

    /// [Callback thread] The generation section of this block: wall (dispatch → join), the
    /// callback's idle wait inside the join, job count and whether it actually ran in parallel.
    void audioThread_noteGenerationSection(const double wallMs, const double waitMs, const int jobs,
                                           const bool parallel) noexcept
    {
        blockParWallNs_ += (std::int64_t)(wallMs * 1.0e6);
        blockParWaitNs_ += (std::int64_t)(waitMs * 1.0e6);
        blockParJobs_ += jobs;
        blockWasParallel_ = blockWasParallel_ || parallel;
    }

    /// [Callback thread] End of a block (after every job joined): fold the block into the windows.
    void audioThread_endBlock(const std::int64_t startTicks, const int numSamples, const double sampleRate) noexcept
    {
        const std::int64_t totalNs = ticksToNs(ticks() - startTicks);
        for (int c = 0; c < kCategories; ++c)
        {
            const std::int64_t v = blockCatNs_[(size_t)c].load(std::memory_order_relaxed);
            catSumNs_[(size_t)c].fetch_add(v, std::memory_order_relaxed);
            casMax(catMaxNs_[(size_t)c], v);
            catCalls_[(size_t)c].fetch_add(blockCalls_[(size_t)c].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        // Remainder = the callback thread's own wall time outside its own plug-in / proxy calls and
        // outside the generation section (whose wall already contains the parallel plug-in work).
        const std::int64_t remainderNs = std::max<std::int64_t>(
            0, totalNs - blockCallbackThreadCatNs_ - blockParWallNs_);
        remSumNs_.fetch_add(remainderNs, std::memory_order_relaxed);
        casMax(remMaxNs_, remainderNs);
        for (int p = 0; p < kPhases; ++p)
        {
            phaseSumNs_[(size_t)p].fetch_add(blockPhaseNs_[(size_t)p], std::memory_order_relaxed);
            casMax(phaseMaxNs_[(size_t)p], blockPhaseNs_[(size_t)p]);
        }
        parWallSumNs_.fetch_add(blockParWallNs_, std::memory_order_relaxed);
        casMax(parWallMaxNs_, blockParWallNs_);
        parWaitSumNs_.fetch_add(blockParWaitNs_, std::memory_order_relaxed);
        casMax(parWaitMaxNs_, blockParWaitNs_);
        if (blockWasParallel_)
        {
            parBlocks_.fetch_add(1, std::memory_order_relaxed);
        }
        parJobs_.fetch_add((std::uint64_t)blockParJobs_, std::memory_order_relaxed);
        blocks_.fetch_add(1, std::memory_order_relaxed);
        totalSumNs_.fetch_add(totalNs, std::memory_order_relaxed);
        if (totalNs > totalMaxNs_.load(std::memory_order_relaxed))
        {
            totalMaxNs_.store(totalNs, std::memory_order_relaxed);
            worstTotalNs_.store(totalNs, std::memory_order_relaxed);
            for (int c = 0; c < kCategories; ++c)
            {
                worstCatNs_[(size_t)c].store(blockCatNs_[(size_t)c].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            worstRemNs_.store(remainderNs, std::memory_order_relaxed);
            worstParWallNs_.store(blockParWallNs_, std::memory_order_relaxed);
            worstSamples_.store(numSamples, std::memory_order_relaxed);
        }
        if (blockIntervalNs_ >= 0)
        {
            intervals_.fetch_add(1, std::memory_order_relaxed);
            intervalSumNs_.fetch_add(blockIntervalNs_, std::memory_order_relaxed);
            casMax(intervalMaxNs_, blockIntervalNs_);
            const double periodNs = (sampleRate > 0.0 && numSamples > 0)
                                        ? 1.0e9 * (double)numSamples / sampleRate
                                        : 0.0;
            if (periodNs > 0.0 && (double)blockIntervalNs_ > 1.25 * periodNs)
            {
                lateStarts_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

private:
    AudioThreadProfiler() = default;

    struct Instance
    {
        Category category = Category::InstrumentPlugin;
        TrackId trackId = kInvalidTrackId;
        InsertSlotId slotId = kInvalidInsertSlotId;
        juce::String name; ///< message thread only
        std::array<std::atomic<std::uint64_t>, kCategories> calls{};
        std::array<std::atomic<std::int64_t>, kCategories> sumNs{};
        std::array<std::atomic<std::int64_t>, kCategories> maxNs{};
    };

    [[nodiscard]] static std::int64_t ticksToNs(const std::int64_t t) noexcept
    {
        const double perSec = (double)juce::Time::getHighResolutionTicksPerSecond();
        return perSec > 0.0 ? (std::int64_t)((double)t * 1.0e9 / perSec) : 0;
    }
    [[nodiscard]] static double nsToMs(const std::int64_t ns) noexcept { return (double)ns / 1.0e6; }
    static void casMax(std::atomic<std::int64_t>& a, const std::int64_t v) noexcept
    {
        std::int64_t cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }
    }

    void resetWindows() noexcept
    {
        (void)snapshotAndReset();
        lastStartTicks_ = 0;
    }

    std::atomic<bool> enabled_{ false };
    std::atomic<int> registered_{ 0 };
    std::array<Instance, kMaxInstances> instances_{};
    inline static thread_local bool tlsInsideGenerationJob_ = false;

    // per-block scratch: category / call counters are folded by any render thread (atomic);
    // the rest is written by the callback thread only
    std::array<std::atomic<std::int64_t>, kCategories> blockCatNs_{};
    std::array<std::atomic<std::uint64_t>, kCategories> blockCalls_{};
    std::int64_t blockCallbackThreadCatNs_ = 0;
    std::array<std::int64_t, kPhases> blockPhaseNs_{};
    std::int64_t blockParWallNs_ = 0;
    std::int64_t blockParWaitNs_ = 0;
    int blockParJobs_ = 0;
    bool blockWasParallel_ = false;
    std::int64_t lastStartTicks_ = 0;
    std::int64_t blockIntervalNs_ = -1;

    // windows (relaxed atomics; message thread drains)
    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::int64_t> totalSumNs_{ 0 };
    std::atomic<std::int64_t> totalMaxNs_{ 0 };
    std::array<std::atomic<std::int64_t>, kCategories> catSumNs_{};
    std::array<std::atomic<std::int64_t>, kCategories> catMaxNs_{};
    std::array<std::atomic<std::uint64_t>, kCategories> catCalls_{};
    std::atomic<std::int64_t> remSumNs_{ 0 };
    std::atomic<std::int64_t> remMaxNs_{ 0 };
    std::array<std::atomic<std::int64_t>, kPhases> phaseSumNs_{};
    std::array<std::atomic<std::int64_t>, kPhases> phaseMaxNs_{};
    std::atomic<std::int64_t> parWallSumNs_{ 0 };
    std::atomic<std::int64_t> parWallMaxNs_{ 0 };
    std::atomic<std::int64_t> parWaitSumNs_{ 0 };
    std::atomic<std::int64_t> parWaitMaxNs_{ 0 };
    std::atomic<std::uint64_t> parBlocks_{ 0 };
    std::atomic<std::uint64_t> parJobs_{ 0 };
    std::atomic<std::int64_t> worstTotalNs_{ 0 };
    std::array<std::atomic<std::int64_t>, kCategories> worstCatNs_{};
    std::atomic<std::int64_t> worstRemNs_{ 0 };
    std::atomic<std::int64_t> worstParWallNs_{ 0 };
    std::atomic<int> worstSamples_{ 0 };
    std::atomic<std::uint64_t> intervals_{ 0 };
    std::atomic<std::int64_t> intervalSumNs_{ 0 };
    std::atomic<std::int64_t> intervalMaxNs_{ 0 };
    std::atomic<std::uint32_t> lateStarts_{ 0 };
};
} // namespace audio_profiler
