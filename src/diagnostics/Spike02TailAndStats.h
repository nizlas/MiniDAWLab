#pragma once

// ============================================================================
// SPIKE-02 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE.
//
// Pure, allocation-light helpers for the SPIKE-02 isolated-render measurements
// (canonical steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §14/§15/§21
// PID-004/PID-005, roadmap P1D): block-processing timing statistics and the
// tail/silence-policy candidate evaluator. Standard C++ only (no JUCE), so the
// deterministic Level-1 selftests can exercise the exact evaluation logic that
// interprets the real-plugin measurements.
//
// Removal: delete src/diagnostics/Spike02*.* and the S2* auto plans in
// Spike01StateCapturePanel.cpp (see the SPIKE-02 report §18 for the full list).
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace spike02
{

//==============================================================================
// dBFS conversion
//==============================================================================

/// Linear [0..1] amplitude -> dBFS. Silence clamps to -200 dB to keep tables finite.
[[nodiscard]] inline double dbfsFromLinear(const double linear) noexcept
{
    if (!(linear > 0.0))
    {
        return -200.0;
    }
    return 20.0 * std::log10(linear);
}

[[nodiscard]] inline double linearFromDbfs(const double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

//==============================================================================
// Block-processing timing statistics (B/C/D metrics)
//==============================================================================

struct TimingStats
{
    std::size_t count = 0;
    double meanMs = 0.0;
    double medianMs = 0.0;
    double p95Ms = 0.0;
    double maxMs = 0.0;
    double minMs = 0.0;
    double sumMs = 0.0;
};

/// Median = lower-median for even counts; p95 = value at ceil(0.95*n)-1 of the sorted
/// series (nearest-rank). Deterministic, selftest-covered.
[[nodiscard]] inline TimingStats computeTimingStats(std::vector<double> samplesMs)
{
    TimingStats s;
    s.count = samplesMs.size();
    if (samplesMs.empty())
    {
        return s;
    }
    std::sort(samplesMs.begin(), samplesMs.end());
    for (const double v : samplesMs)
    {
        s.sumMs += v;
    }
    s.meanMs = s.sumMs / (double)s.count;
    s.minMs = samplesMs.front();
    s.maxMs = samplesMs.back();
    s.medianMs = samplesMs[(s.count - 1) / 2];
    const std::size_t rank = (std::size_t)std::ceil(0.95 * (double)s.count);
    s.p95Ms = samplesMs[rank > 0 ? rank - 1 : 0];
    return s;
}

//==============================================================================
// Per-block levels and simple summaries (idle floor, output validity)
//==============================================================================

/// One processed block's absolute peak and RMS, linear [0..1].
struct BlockLevel
{
    double peak = 0.0;
    double rms = 0.0;
};

struct LevelSummary
{
    double maxPeak = 0.0;        ///< linear
    double maxPeakDb = -200.0;   ///< dBFS
    double meanRms = 0.0;        ///< linear
    double meanRmsDb = -200.0;   ///< dBFS
    double maxRmsDb = -200.0;    ///< dBFS (loudest single block, RMS)
    std::size_t blocks = 0;
};

[[nodiscard]] inline LevelSummary summarizeLevels(const std::vector<BlockLevel>& series)
{
    LevelSummary s;
    s.blocks = series.size();
    if (series.empty())
    {
        return s;
    }
    double sumRms = 0.0;
    double maxRms = 0.0;
    for (const auto& b : series)
    {
        s.maxPeak = std::max(s.maxPeak, b.peak);
        maxRms = std::max(maxRms, b.rms);
        sumRms += b.rms;
    }
    s.meanRms = sumRms / (double)series.size();
    s.maxPeakDb = dbfsFromLinear(s.maxPeak);
    s.meanRmsDb = dbfsFromLinear(s.meanRms);
    s.maxRmsDb = dbfsFromLinear(maxRms);
    return s;
}

//==============================================================================
// Tail-policy candidate evaluation (G)
//
// Semantics under evaluation (locked structure, steering §15): after the final
// scheduled event, the render is complete when the ABSOLUTE per-block peak has
// stayed below threshold X for a continuous window Y; reaching the maximum tail
// Z while materially non-silent is Failed (never publishes).
//==============================================================================

struct TailCandidate
{
    double thresholdDb = -70.0; ///< X: absolute peak threshold (dBFS)
    double windowSec = 1.0;     ///< Y: required continuous below-threshold window
    double capSec = 30.0;       ///< Z: maximum tail
};

struct TailCandidateResult
{
    TailCandidate candidate;

    /// True when a continuous below-threshold window of windowSec completed within capSec.
    bool completed = false;
    /// Published tail length: time from the final event to the START of the qualifying
    /// window (the first block of the silence run). 0 when the output was already below
    /// threshold at the final event.
    double tailSec = 0.0;
    /// Time from the final event until the detector DECIDED (end of the qualifying window).
    double decisionSec = 0.0;
    /// True when capSec elapsed with no qualifying window (Failed — never publishes).
    bool failedAtCap = false;
    /// Highest block peak (dBFS) observed AFTER the qualifying window ended, i.e. material
    /// the detector would have cut off. -200 when nothing follows or not completed.
    double peakAfterDecisionDb = -200.0;
    /// Highest block RMS (dBFS) after the decision point.
    double rmsAfterDecisionDb = -200.0;
    /// True when at least one block AFTER the decision point rose back above threshold
    /// (the candidate would have clipped real material or re-triggering noise).
    bool roseAboveThresholdAfterDecision = false;
};

/// Evaluate one candidate over a measured post-final-event series.
/// `series` = per-block levels starting at the block containing the final event's
/// release; `blockSec` = seconds of audio per block. The series SHOULD extend to the
/// largest cap under evaluation so "rose again" can be detected past early decisions.
[[nodiscard]] inline TailCandidateResult evaluateTailCandidate(const std::vector<BlockLevel>& series,
                                                               const double blockSec,
                                                               const TailCandidate candidate)
{
    TailCandidateResult r;
    r.candidate = candidate;
    if (series.empty() || !(blockSec > 0.0))
    {
        r.failedAtCap = true;
        return r;
    }

    const double thresholdLinear = linearFromDbfs(candidate.thresholdDb);
    const std::size_t windowBlocks =
        std::max<std::size_t>(1, (std::size_t)std::llround(candidate.windowSec / blockSec));
    const std::size_t capBlocks = (std::size_t)std::llround(candidate.capSec / blockSec);

    std::size_t runStart = 0;
    std::size_t runLength = 0;
    std::size_t decisionBlock = 0; // exclusive end of qualifying window
    for (std::size_t i = 0; i < series.size(); ++i)
    {
        const bool below = series[i].peak < thresholdLinear;
        if (below)
        {
            if (runLength == 0)
            {
                runStart = i;
            }
            ++runLength;
            if (runLength >= windowBlocks)
            {
                r.completed = true;
                decisionBlock = i + 1;
                r.tailSec = (double)runStart * blockSec;
                r.decisionSec = (double)decisionBlock * blockSec;
                break;
            }
        }
        else
        {
            runLength = 0;
        }
        // Cap check: the decision must land within the cap.
        if (i + 1 >= capBlocks)
        {
            break;
        }
    }

    if (!r.completed)
    {
        r.failedAtCap = true;
        return r;
    }

    for (std::size_t i = decisionBlock; i < series.size(); ++i)
    {
        r.peakAfterDecisionDb = std::max(r.peakAfterDecisionDb, dbfsFromLinear(series[i].peak));
        r.rmsAfterDecisionDb = std::max(r.rmsAfterDecisionDb, dbfsFromLinear(series[i].rms));
        if (series[i].peak >= thresholdLinear)
        {
            r.roseAboveThresholdAfterDecision = true;
        }
    }
    return r;
}

/// The full SPIKE-02 candidate grid: X in {-60,-70,-80,-90} dBFS, Y in {0.5,1,2} s,
/// Z in {15,30,60} s.
[[nodiscard]] inline std::vector<TailCandidateResult>
evaluateTailCandidateGrid(const std::vector<BlockLevel>& series, const double blockSec)
{
    static constexpr double thresholds[] = { -60.0, -70.0, -80.0, -90.0 };
    static constexpr double windows[] = { 0.5, 1.0, 2.0 };
    static constexpr double caps[] = { 15.0, 30.0, 60.0 };

    std::vector<TailCandidateResult> out;
    out.reserve(4 * 3 * 3);
    for (const double x : thresholds)
    {
        for (const double y : windows)
        {
            for (const double z : caps)
            {
                out.push_back(evaluateTailCandidate(series, blockSec, TailCandidate{ x, y, z }));
            }
        }
    }
    return out;
}

} // namespace spike02
