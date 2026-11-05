#pragma once

// =============================================================================
// InsertChainRest — automatic DSP rest for one insert chain stage
// (docs/RESOURCE_SAVING_2026-11-05.md §4)
// =============================================================================
// A stage (the Pre or Post inserts of one row) may skip `processBlock` for a block when it
// provably has nothing to produce: its input is digitally silent in this block AND
//
//   * since its plug-ins were prepared it has never received input and its output has stayed
//     silent for `latency + 2 blocks`, or
//   * every active plug-in reports a finite, plausible tail (0 < tail <= 60 s; 0 means
//     "not reported", +inf means infinite), none of them has been seen sounding after its
//     reported tail ran out, and input and output have both been silent for at least
//     `2 x tail + latency + 1 block`. The second tail length is a check: a plug-in heard after
//     its reported tail ran out is marked unreliable and its stage never rests again.
//
// A stage that contains a plug-in which calls itself an instrument never rests, nor does one whose
// editor is open. (Inserts receive no MIDI from DAL, so accepting MIDI is not a reason.) Any non-silent input wakes the stage in the SAME block, before
// processing, so the first sample is never lost. Resting does not reset plug-ins: a stage only
// rests after its output has already been silent for longer than its tail, so nothing old can
// come back. Skipping a stage leaves its buffer as it came in, which is silence.
//
// Threads: `Params` are written on the message thread (atomics, `generation` last with release).
// `State` belongs to whichever single lane processes the row in a block (callback, render-pool
// worker or read-ahead worker); lanes hand the row over through the existing synchronisation.

#include <atomic>
#include <cmath>
#include <cstdint>

namespace insert_rest
{

/// Silence means digital silence (every sample exactly 0). Skipping a block whose input and
/// recent output are exactly zero therefore produces the same samples as processing it would,
/// so the result does not depend on where a lane happened to decide (callback block, render
/// pool, read-ahead segment) and stays bit-identical.
inline constexpr float kSilenceThreshold = 0.0f;
/// A reported tail above this is treated as unknown (keeps running), like +inf.
inline constexpr double kMaxPlausibleTailSeconds = 60.0;

[[nodiscard]] inline bool isSilent(const float* const* channels, const int numChannels, const int n) noexcept
{
    for (int c = 0; c < numChannels; ++c)
    {
        const float* const row = channels[c];
        if (row == nullptr)
        {
            continue;
        }
        for (int i = 0; i < n; ++i)
        {
            if (std::fabs(row[i]) > kSilenceThreshold)
            {
                return false;
            }
        }
    }
    return true;
}

/// How a stage's tail is known (diagnostics and tests).
enum class TailKind : int
{
    Empty = 0,     ///< no active plug-in in the stage
    Finite,        ///< every active plug-in reports 0 < tail <= 60 s
    Unknown,       ///< some plug-in reports 0 (VST3 default) or more than 60 s
    Infinite,      ///< some plug-in reports kInfiniteTail
};

struct Params
{
    std::atomic<std::int64_t> tailSamples { 0 };
    std::atomic<int> latencySamples { 0 };
    std::atomic<int> tailKind { (int)TailKind::Empty };
    /// A plug-in that is an instrument, or an open editor.
    std::atomic<bool> neverRest { false };
    /// Bumped on every change that must wake the stage (publish, Active/Bypass, state load).
    std::atomic<std::uint32_t> generation { 0 };
};

struct State
{
    std::uint32_t seenGeneration = ~0u;
    bool resting = false;
    bool everHadInput = false;
    bool unreliable = false;
    std::int64_t silentInSamples = 0;
    std::int64_t silentOutSamples = 0;
    /// Diagnostics (relaxed; read on the message thread).
    std::atomic<std::uint64_t> restedBlocks { 0 };
    std::atomic<std::uint64_t> processedBlocks { 0 };

    /// [Processing lane] Before processing: true = skip `processBlock` for this block.
    /// `suspended` (offline export): always process, so an export equals an uninterrupted render.
    [[nodiscard]] bool shouldSkip(const bool inputSilent, const Params& p, const bool suspended = false) noexcept
    {
        const std::uint32_t gen = p.generation.load(std::memory_order_acquire);
        if (gen != seenGeneration)
        {
            // Something changed in the chain: work again and re-measure. `everHadInput` stays:
            // the plug-ins that kept their state may still hold a tail.
            seenGeneration = gen;
            resting = false;
            unreliable = false;
            silentInSamples = 0;
            silentOutSamples = 0;
        }
        if (!inputSilent || suspended)
        {
            resting = false; // wake in this block, before processing
        }
        if (resting)
        {
            restedBlocks.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        processedBlocks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// [Processing lane] After processing a block of `n` samples.
    void afterProcess(const bool inputSilent, const bool outputSilent, const int n, const Params& p) noexcept
    {
        const auto kind = (TailKind)p.tailKind.load(std::memory_order_relaxed);
        const std::int64_t latency = juce_max64(0, p.latencySamples.load(std::memory_order_relaxed));
        const std::int64_t tail = juce_max64(0, p.tailSamples.load(std::memory_order_relaxed));
        const std::int64_t reported = tail + latency + n; // the plug-ins' own promise
        const std::int64_t window = reported + tail;       // plus one more tail as a check

        if (inputSilent)
        {
            silentInSamples += n;
        }
        else
        {
            everHadInput = true;
            silentInSamples = 0;
        }
        if (outputSilent)
        {
            silentOutSamples += n;
        }
        else
        {
            // Sounding after the reported tail ran out: the report cannot be trusted.
            if (kind == TailKind::Finite && silentInSamples > reported)
            {
                unreliable = true;
            }
            silentOutSamples = 0;
        }

        if (!inputSilent || p.neverRest.load(std::memory_order_relaxed) || kind == TailKind::Empty)
        {
            return;
        }
        if (!everHadInput)
        {
            resting = silentOutSamples >= latency + 2 * (std::int64_t)n;
            return;
        }
        resting = kind == TailKind::Finite && !unreliable && silentInSamples >= window
                  && silentOutSamples >= window;
    }


private:
    [[nodiscard]] static std::int64_t juce_max64(const std::int64_t a, const std::int64_t b) noexcept
    {
        return a > b ? a : b;
    }
};

/// [Message thread] Classify a stage from its active plug-ins' reported tails (seconds).
[[nodiscard]] inline TailKind classifyTail(const double tailSeconds, TailKind soFar) noexcept
{
    TailKind one = TailKind::Finite;
    if (std::isinf(tailSeconds))
    {
        one = TailKind::Infinite;
    }
    else if (!(tailSeconds > 0.0) || tailSeconds > kMaxPlausibleTailSeconds)
    {
        one = TailKind::Unknown;
    }
    if (soFar == TailKind::Empty)
    {
        return one;
    }
    // The least favourable kind wins: Infinite > Unknown > Finite.
    return (int)one > (int)soFar ? one : soFar;
}

} // namespace insert_rest
