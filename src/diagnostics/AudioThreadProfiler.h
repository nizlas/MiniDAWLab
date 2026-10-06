#pragma once

// =============================================================================
// AudioThreadProfiler — explicitly enabled, bounded cost attribution inside the audio callback
// =============================================================================
// Diagnostics only (`--stability-perf-profile`). OFF by default: every audio-thread call site
// pays ONE relaxed atomic load and nothing else. When enabled, the callback takes two clock
// reads per measured call and folds the elapsed time into preallocated relaxed atomics:
//
//   * per registered INSTANCE (instrument host or insert plug-in, keyed by TrackId + slot) —
//     calls / summed ms / worst ms, split by category;
//   * per CATEGORY per block — the exclusive time spent inside plug-in `processBlock` calls of
//     instruments, inside the proxy fetch/mix, and inside insert `processBlock` calls; the
//     block's remainder (callback total − those three) is "DAL work" (MIDI scheduling, clip
//     rendering, routing, summing, meters, bookkeeping);
//   * per PHASE per block — INCLUSIVE wall time of the callback's sections (begin-block,
//     live MIDI, clip render incl. the audio rows' inserts, transport MIDI scheduling,
//     instrument mix incl. the instrument rows' inserts, bus finalize incl. the bus inserts),
//     so the remainder can be located. Phases overlap categories by design and are never
//     added to them;
//   * the WORST block (highest callback total) with its own category breakdown, so maxima are
//     reported from one real block instead of summing independent maxima;
//   * callback START intervals (late device callbacks) — a block that starts late overruns
//     the device deadline even when its own duration is within budget.
//
// Audio-thread contract: no allocation, no locks, no I/O; fixed arrays; relaxed atomics only.
// Registration (names) and reporting run on the message thread. The per-block scratch fields
// are written by the audio thread only. A lost update during a message-thread reset is
// acceptable for diagnostics (same discipline as PlaybackEngine's load window).
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

    /// Returns the instance index for (category, trackId, slotId), creating it when new, or −1
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
        std::array<double, kCategories> categorySumMs{};
        std::array<double, kCategories> categoryMaxMs{};
        std::array<std::uint64_t, kCategories> categoryCalls{};
        double remainderSumMs = 0.0;
        double remainderMaxMs = 0.0;
        std::array<double, kPhases> phaseSumMs{};
        std::array<double, kPhases> phaseMaxMs{};
        /// The single block with the highest callback total and its own breakdown.
        double worstTotalMs = 0.0;
        std::array<double, kCategories> worstCategoryMs{};
        double worstRemainderMs = 0.0;
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
        r.totalSumMs = totalSum_.exchange(0.0, std::memory_order_relaxed);
        r.totalMaxMs = totalMax_.exchange(0.0, std::memory_order_relaxed);
        for (int c = 0; c < kCategories; ++c)
        {
            r.categorySumMs[(size_t)c] = catSum_[(size_t)c].exchange(0.0, std::memory_order_relaxed);
            r.categoryMaxMs[(size_t)c] = catMax_[(size_t)c].exchange(0.0, std::memory_order_relaxed);
            r.categoryCalls[(size_t)c] = catCalls_[(size_t)c].exchange(0, std::memory_order_relaxed);
            r.worstCategoryMs[(size_t)c] = worstCat_[(size_t)c].exchange(0.0, std::memory_order_relaxed);
        }
        r.remainderSumMs = remSum_.exchange(0.0, std::memory_order_relaxed);
        r.remainderMaxMs = remMax_.exchange(0.0, std::memory_order_relaxed);
        for (int p = 0; p < kPhases; ++p)
        {
            r.phaseSumMs[(size_t)p] = phaseSum_[(size_t)p].exchange(0.0, std::memory_order_relaxed);
            r.phaseMaxMs[(size_t)p] = phaseMax_[(size_t)p].exchange(0.0, std::memory_order_relaxed);
        }
        r.worstTotalMs = worstTotal_.exchange(0.0, std::memory_order_relaxed);
        r.worstRemainderMs = worstRem_.exchange(0.0, std::memory_order_relaxed);
        r.worstBlockSamples = worstSamples_.exchange(0, std::memory_order_relaxed);
        r.intervals = intervals_.exchange(0, std::memory_order_relaxed);
        r.intervalSumMs = intervalSum_.exchange(0.0, std::memory_order_relaxed);
        r.intervalMaxMs = intervalMax_.exchange(0.0, std::memory_order_relaxed);
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
                ir.sumMs[(size_t)c] = in.sumMs[(size_t)c].exchange(0.0, std::memory_order_relaxed);
                ir.maxMs[(size_t)c] = in.maxMs[(size_t)c].exchange(0.0, std::memory_order_relaxed);
            }
            r.instances.push_back(std::move(ir));
        }
        return r;
    }

    // ------------------------------------------------------------------ audio thread
    [[nodiscard]] bool audioThread_enabled() const noexcept
    {
        return enabled_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static std::int64_t ticks() noexcept { return juce::Time::getHighResolutionTicks(); }

    void audioThread_beginBlock(const std::int64_t startTicks) noexcept
    {
        for (auto& v : blockCat_) { v = 0.0; }
        for (auto& v : blockPhase_) { v = 0.0; }
        if (lastStartTicks_ != 0)
        {
            blockIntervalMs_ = ticksToMs(startTicks - lastStartTicks_);
        }
        else
        {
            blockIntervalMs_ = -1.0;
        }
        lastStartTicks_ = startTicks;
    }

    /// Fold one measured call: `slot` may be −1 (unregistered → category totals only).
    void audioThread_addInstance(const int slot, const Category category, const std::int64_t startTicks) noexcept
    {
        const double ms = ticksToMs(ticks() - startTicks);
        const auto c = (size_t)category;
        blockCat_[c] += ms;
        ++blockCalls_[c];
        if (slot >= 0 && slot < kMaxInstances)
        {
            Instance& in = instances_[(size_t)slot];
            in.calls[c].fetch_add(1, std::memory_order_relaxed);
            relaxedAdd(in.sumMs[c], ms);
            relaxedMax(in.maxMs[c], ms);
        }
    }

    void audioThread_addPhase(const Phase phase, const std::int64_t startTicks) noexcept
    {
        blockPhase_[(size_t)phase] += ticksToMs(ticks() - startTicks);
    }

    void audioThread_endBlock(const std::int64_t startTicks, const int numSamples, const double sampleRate) noexcept
    {
        const double totalMs = ticksToMs(ticks() - startTicks);
        double catTotal = 0.0;
        for (int c = 0; c < kCategories; ++c)
        {
            const double v = blockCat_[(size_t)c];
            catTotal += v;
            relaxedAdd(catSum_[(size_t)c], v);
            relaxedMax(catMax_[(size_t)c], v);
        }
        const double remainder = std::max(0.0, totalMs - catTotal);
        relaxedAdd(remSum_, remainder);
        relaxedMax(remMax_, remainder);
        for (int p = 0; p < kPhases; ++p)
        {
            relaxedAdd(phaseSum_[(size_t)p], blockPhase_[(size_t)p]);
            relaxedMax(phaseMax_[(size_t)p], blockPhase_[(size_t)p]);
        }
        for (int c = 0; c < kCategories; ++c)
        {
            catCalls_[(size_t)c].fetch_add(blockCalls_[(size_t)c], std::memory_order_relaxed);
            blockCalls_[(size_t)c] = 0;
        }
        blocks_.fetch_add(1, std::memory_order_relaxed);
        relaxedAdd(totalSum_, totalMs);
        if (totalMs > totalMax_.load(std::memory_order_relaxed))
        {
            totalMax_.store(totalMs, std::memory_order_relaxed);
            worstTotal_.store(totalMs, std::memory_order_relaxed);
            for (int c = 0; c < kCategories; ++c)
            {
                worstCat_[(size_t)c].store(blockCat_[(size_t)c], std::memory_order_relaxed);
            }
            worstRem_.store(remainder, std::memory_order_relaxed);
            worstSamples_.store(numSamples, std::memory_order_relaxed);
        }
        if (blockIntervalMs_ >= 0.0)
        {
            intervals_.fetch_add(1, std::memory_order_relaxed);
            relaxedAdd(intervalSum_, blockIntervalMs_);
            relaxedMax(intervalMax_, blockIntervalMs_);
            const double periodMs = (sampleRate > 0.0 && numSamples > 0)
                                        ? 1000.0 * (double)numSamples / sampleRate
                                        : 0.0;
            if (periodMs > 0.0 && blockIntervalMs_ > 1.25 * periodMs)
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
        std::array<std::atomic<double>, kCategories> sumMs{};
        std::array<std::atomic<double>, kCategories> maxMs{};
    };

    [[nodiscard]] static double ticksToMs(const std::int64_t t) noexcept
    {
        const double perSec = (double)juce::Time::getHighResolutionTicksPerSecond();
        return perSec > 0.0 ? (double)t * 1000.0 / perSec : 0.0;
    }
    static void relaxedAdd(std::atomic<double>& a, const double v) noexcept
    {
        a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
    }
    static void relaxedMax(std::atomic<double>& a, const double v) noexcept
    {
        if (v > a.load(std::memory_order_relaxed))
        {
            a.store(v, std::memory_order_relaxed);
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

    // audio thread only (per block scratch)
    std::array<double, kCategories> blockCat_{};
    std::array<std::uint64_t, kCategories> blockCalls_{};
    std::array<double, kPhases> blockPhase_{};
    std::int64_t lastStartTicks_ = 0;
    double blockIntervalMs_ = -1.0;

    // windows (relaxed atomics; message thread drains)
    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<double> totalSum_{ 0.0 };
    std::atomic<double> totalMax_{ 0.0 };
    std::array<std::atomic<double>, kCategories> catSum_{};
    std::array<std::atomic<double>, kCategories> catMax_{};
    std::array<std::atomic<std::uint64_t>, kCategories> catCalls_{};
    std::atomic<double> remSum_{ 0.0 };
    std::atomic<double> remMax_{ 0.0 };
    std::array<std::atomic<double>, kPhases> phaseSum_{};
    std::array<std::atomic<double>, kPhases> phaseMax_{};
    std::atomic<double> worstTotal_{ 0.0 };
    std::array<std::atomic<double>, kCategories> worstCat_{};
    std::atomic<double> worstRem_{ 0.0 };
    std::atomic<int> worstSamples_{ 0 };
    std::atomic<std::uint64_t> intervals_{ 0 };
    std::atomic<double> intervalSum_{ 0.0 };
    std::atomic<double> intervalMax_{ 0.0 };
    std::atomic<std::uint32_t> lateStarts_{ 0 };
};
} // namespace audio_profiler
