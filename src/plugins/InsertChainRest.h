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
// Neither rule is a proof that a plug-in cannot sound later: a transport-driven or free-running
// generator can stay silent for any number of blocks and then start without input, MIDI or a
// parameter change. So a stage may rest at all only when every active plug-in in it declares
// itself an audio-processing effect (`categoryAllowsRest`: an "Fx" with a processing
// sub-category such as EQ, Dynamics, Reverb, Delay or Distortion, and no Instrument, Synth,
// Sampler, Drum, Generator, Tools or plain "Fx"). Such a plug-in turns input into output; with
// silent input its output is expected to stay silent. That is the plug-in's own declaration,
// not something DAL can verify. Instruments therefore never rest, and neither does a stage
// whose editor is open. (Inserts receive no MIDI from DAL, so accepting MIDI is not a reason.)
//
// Wake: any non-silent input wakes the stage in the SAME block, before processing, so the first
// sample is never lost. A parameter change (from any thread), a state or preset load, the
// transport starting or stopping and a jump in play position (locate, cycle wrap) wake it too and
// make it re-qualify from the start (`Params::wake`, `noteTransport`). Resting does not reset plug-ins: a stage only
// rests after its output has already been silent for longer than its tail, so nothing old can
// come back. Skipping a stage leaves its buffer as it came in, which is silence.
//
// Threads: `Params` are written on the message thread (atomics, `generation` last with release).
// `State` belongs to whichever single lane processes the row in a block (callback, render-pool
// worker or read-ahead worker); lanes hand the row over through the existing synchronisation.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>

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
    /// Bumped on every change of the chain itself (publish, instance, Active/Bypass, tail report).
    std::atomic<std::uint32_t> generation { 0 };
    /// Bumped by a parameter change, a state or preset load: the stage works again and re-measures,
    /// but a tail report already found unreliable stays unreliable.
    std::atomic<std::uint32_t> wake { 0 };
};

struct State
{
    std::uint32_t seenGeneration = ~0u;
    std::uint32_t seenWake = ~0u;
    bool resting = false;
    bool everHadInput = false;
    bool unreliable = false;
    std::int64_t silentInSamples = 0;
    std::int64_t silentOutSamples = 0;
    bool transportKnown = false;
    bool lastPlaying = false;
    std::int64_t expectedSample = 0;
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
        if (const std::uint32_t w = p.wake.load(std::memory_order_acquire); w != seenWake)
        {
            seenWake = w;
            wakeAndRequalify();
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

    /// [Processing lane] Before `shouldSkip`, once per processed segment: the transport this
    /// segment plays. Start, stop and a jump in play position (locate, cycle wrap) wake the stage,
    /// because a transport-driven plug-in may start sounding there.
    void noteTransport(const bool playing, const std::int64_t timelineSample, const int n) noexcept
    {
        if (transportKnown && (playing != lastPlaying || (playing && timelineSample != expectedSample)))
        {
            wakeAndRequalify();
        }
        transportKnown = true;
        lastPlaying = playing;
        expectedSample = timelineSample + n;
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
    void wakeAndRequalify() noexcept
    {
        resting = false;
        silentInSamples = 0;
        silentOutSamples = 0;
    }

    [[nodiscard]] static std::int64_t juce_max64(const std::int64_t a, const std::int64_t b) noexcept
    {
        return a > b ? a : b;
    }
};

/// [Message thread] May a plug-in with this declared category (`PluginDescription::category`, the
/// VST3 sub-categories joined by '|') rest at all? Only an "Fx" with at least one processing
/// sub-category and none that can make sound on its own or is too broad to judge.
[[nodiscard]] inline bool categoryAllowsRest(const std::string_view category) noexcept
{
    constexpr std::string_view processing[] = { "EQ",         "Dynamics",    "Reverb",   "Delay",
                                                "Distortion", "Filter",      "Modulation", "Pitch Shift",
                                                "Restoration", "Mastering",  "Spatial",  "Analyzer" };
    constexpr std::string_view generating[] = { "Instrument", "Synth", "Sampler", "Drum",   "Piano",
                                                "Generator",  "External", "Tools", "Network", "OnlyARA" };
    bool fx = false;
    bool processes = false;
    std::size_t start = 0;
    while (start <= category.size())
    {
        const std::size_t bar = category.find('|', start);
        const std::string_view token = category.substr(start, bar == std::string_view::npos ? std::string_view::npos
                                                                                            : bar - start);
        for (const std::string_view g : generating)
        {
            if (token == g)
            {
                return false;
            }
        }
        fx = fx || token == "Fx";
        for (const std::string_view s : processing)
        {
            processes = processes || token == s;
        }
        if (bar == std::string_view::npos)
        {
            break;
        }
        start = bar + 1;
    }
    return fx && processes;
}

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
