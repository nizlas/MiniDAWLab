#pragma once

// =============================================================================
// LevelMeterAccumulator — lock-free block-level statistics handed from audio to UI
// =============================================================================
//
// ROLE
//   The audio thread folds every rendered block of one stereo (or mono) signal into relaxed
//   atomics: per-channel sample-peak hold, count of samples beyond full scale, sum of squares,
//   block/sample counters and a non-finite counter. The message thread drains-and-resets at its
//   own rate (meter repaint timer), so no peak that occurred between two UI reads is ever lost —
//   the hold is a max over *all* blocks since the previous drain, not the last block only.
//
// WHAT IS MEASURED
//   Sample peak (not true/inter-sample peak) and "overs" = samples with |x| > 1.0 (above 0 dBFS).
//   A float value above full scale is an overload *warning* at the measuring point: the signal
//   has not necessarily been clipped there; it will be when converted to fixed-point PCM or a
//   device format that cannot represent it. Callers label the numbers accordingly.
//
// THREADING
//   [Audio thread] `audioThread_fold` — no locks, no allocation, no logging.
//   [Message thread] `drainAndReset` / `reset`. Relaxed atomics only; never used to synchronize
//   anything else (same discipline as `PlaybackEngine::outputPeakHold_`).
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace level_meter
{

/// One drained window: everything the audio thread folded since the previous drain.
struct Reading
{
    /// Largest |sample| per channel (linear). +inf when a non-finite sample was seen.
    float peak[2] = { 0.0f, 0.0f };
    /// Samples with |x| > 1.0 per channel (above 0 dBFS).
    std::uint32_t overs[2] = { 0, 0 };
    /// Sum of squares per channel over `samples` frames (RMS = sqrt(sumSquares / samples)).
    double sumSquares[2] = { 0.0, 0.0 };
    /// Plain sum per channel (DC offset = sum / samples) — a constant offset is a diagnosis in
    /// itself: it eats headroom and clicks at every start/stop and file edge.
    double sum[2] = { 0.0, 0.0 };
    std::uint64_t samples = 0;
    std::uint32_t blocks = 0;
    std::uint32_t nonFinite = 0;
    /// Channels folded in the last block of the window: 0 (nothing folded), 1 (mono) or 2.
    int channels = 0;

    [[nodiscard]] bool hasSignalData() const noexcept { return blocks > 0; }
    [[nodiscard]] double rms(const int ch) const noexcept
    {
        return (samples > 0 && ch >= 0 && ch < 2) ? std::sqrt(sumSquares[ch] / static_cast<double>(samples)) : 0.0;
    }
    [[nodiscard]] double dcOffset(const int ch) const noexcept
    {
        return (samples > 0 && ch >= 0 && ch < 2) ? sum[ch] / static_cast<double>(samples) : 0.0;
    }
};

/// Per-block statistics computed once on the audio thread, so several accumulators (live meter,
/// diagnostics window) can fold the same block without re-scanning it.
struct BlockStats
{
    float peak[2] = { 0.0f, 0.0f };
    std::uint32_t overs[2] = { 0, 0 };
    double sumSquares[2] = { 0.0, 0.0 };
    double sum[2] = { 0.0, 0.0 };
    bool nonFinite[2] = { false, false };
    int channels = 0;
    int numSamples = 0;
};

/// [Audio thread] One scalar pass per channel (sum / sum of squares / overs) plus a SIMD min/max.
/// A NaN/Inf anywhere poisons the sum of squares, which is how non-finite samples are detected
/// (SIMD min/max comparisons silently skip NaN). No allocation, no locks.
[[nodiscard]] inline BlockStats analyzeBlock(const float* left, const float* right, const int numSamples) noexcept
{
    BlockStats s;
    if (left == nullptr || numSamples <= 0)
    {
        return s;
    }
    const float* chans[2] = { left, right };
    s.channels = right != nullptr ? 2 : 1;
    s.numSamples = numSamples;
    for (int ch = 0; ch < s.channels; ++ch)
    {
        const float* data = chans[ch];
        double sumSq = 0.0;
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const double v = static_cast<double>(data[i]);
            sumSq += v * v;
            sum += v;
        }
        if (!std::isfinite(sumSq))
        {
            s.nonFinite[ch] = true;
            s.peak[ch] = std::numeric_limits<float>::infinity();
            continue;
        }
        s.sumSquares[ch] = sumSq;
        s.sum[ch] = sum;
        const juce::Range<float> mm = juce::FloatVectorOperations::findMinAndMax(data, numSamples);
        s.peak[ch] = juce::jmax(std::abs(mm.getStart()), std::abs(mm.getEnd()));
        if (s.peak[ch] > 1.0f)
        {
            std::uint32_t overs = 0;
            for (int i = 0; i < numSamples; ++i)
            {
                overs += (std::abs(data[i]) > 1.0f) ? 1u : 0u;
            }
            s.overs[ch] = overs;
        }
    }
    return s;
}

class Accumulator
{
public:
    Accumulator() = default;
    Accumulator(const Accumulator&) = delete;
    Accumulator& operator=(const Accumulator&) = delete;

    /// [Audio thread] Fold one block. `right` may be null for a mono signal (channels = 1).
    void audioThread_fold(const float* left, const float* right, const int numSamples) noexcept
    {
        audioThread_foldStats(analyzeBlock(left, right, numSamples));
    }

    /// [Audio thread] Fold pre-computed block statistics (shared between accumulators).
    void audioThread_foldStats(const BlockStats& s) noexcept
    {
        if (s.channels <= 0 || s.numSamples <= 0)
        {
            return;
        }
        for (int ch = 0; ch < s.channels; ++ch)
        {
            if (s.nonFinite[ch])
            {
                nonFinite_.fetch_add(1, std::memory_order_relaxed);
            }
            float current = peak_[ch].load(std::memory_order_relaxed);
            while (s.peak[ch] > current && !peak_[ch].compare_exchange_weak(current, s.peak[ch], std::memory_order_relaxed))
            {
            }
            if (s.overs[ch] > 0)
            {
                overs_[ch].fetch_add(s.overs[ch], std::memory_order_relaxed);
            }
            if (!s.nonFinite[ch])
            {
                double cur = sumSquares_[ch].load(std::memory_order_relaxed);
                while (!sumSquares_[ch].compare_exchange_weak(cur, cur + s.sumSquares[ch], std::memory_order_relaxed))
                {
                }
                double curSum = sum_[ch].load(std::memory_order_relaxed);
                while (!sum_[ch].compare_exchange_weak(curSum, curSum + s.sum[ch], std::memory_order_relaxed))
                {
                }
            }
        }
        channels_.store(s.channels, std::memory_order_relaxed);
        samples_.fetch_add(static_cast<std::uint64_t>(s.numSamples), std::memory_order_relaxed);
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    /// [Message thread] Everything folded since the previous call; resets the window.
    [[nodiscard]] Reading drainAndReset() noexcept
    {
        Reading r;
        for (int ch = 0; ch < 2; ++ch)
        {
            r.peak[ch] = peak_[ch].exchange(0.0f, std::memory_order_relaxed);
            r.overs[ch] = overs_[ch].exchange(0, std::memory_order_relaxed);
            r.sumSquares[ch] = sumSquares_[ch].exchange(0.0, std::memory_order_relaxed);
            r.sum[ch] = sum_[ch].exchange(0.0, std::memory_order_relaxed);
        }
        r.samples = samples_.exchange(0, std::memory_order_relaxed);
        r.blocks = blocks_.exchange(0, std::memory_order_relaxed);
        r.nonFinite = nonFinite_.exchange(0, std::memory_order_relaxed);
        r.channels = channels_.load(std::memory_order_relaxed);
        return r;
    }

    /// [Message thread] Discard the window (track switch / project replace).
    void reset() noexcept { (void)drainAndReset(); }

private:
    std::atomic<float> peak_[2] = { 0.0f, 0.0f };
    std::atomic<std::uint32_t> overs_[2] = { 0u, 0u };
    std::atomic<double> sumSquares_[2] = { 0.0, 0.0 };
    std::atomic<double> sum_[2] = { 0.0, 0.0 };
    std::atomic<std::uint64_t> samples_{ 0 };
    std::atomic<std::uint32_t> blocks_{ 0 };
    std::atomic<std::uint32_t> nonFinite_{ 0 };
    std::atomic<int> channels_{ 0 };
};

/// Linear peak → dBFS text for meters: "-inf" for silence, otherwise one decimal with an explicit
/// sign ("-3.2", "+1.4", "0.0"). Never clamps: a value above 0 dBFS stays visible as positive.
[[nodiscard]] inline juce::String peakToDbfsText(const float linearPeak)
{
    if (!std::isfinite(linearPeak))
    {
        return "NaN";
    }
    if (linearPeak <= 1.0e-6f)
    {
        return juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e"));
    }
    const double db = 20.0 * std::log10(static_cast<double>(linearPeak));
    if (std::fabs(db) < 0.05)
    {
        return "0.0";
    }
    return (db > 0.0 ? "+" : "") + juce::String(db, 1);
}

} // namespace level_meter
