#pragma once

// =============================================================================
// ArrangementMusicalGrid — choose ruler/lane musical grid density from zoom + tempo (Slice B UI aid)
// =============================================================================
// Sample 0 = bar 1 beat 1. Uses rounded samples-per-beat / samples-per-bar for aligned ticks.

#include "domain/MusicalTimeConversions.h"
#include "domain/ProjectMusicalTime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

struct ArrangementMusicalGridPlan
{
    std::int64_t spBeat = 0;
    std::int64_t spBar = 0;
    int barsPerMajor = 1;
    bool drawEveryBarLine = false;
    bool drawBeatLines = false;
    /// 2 or 4 subdivisions per beat (sixteenth/eighth); 0 = none.
    int subDivisionsPerBeat = 0;
    bool labelsUseBarBeatDetail = false;
};

[[nodiscard]] inline std::int64_t musicalSubdivisionStrideSamples(std::int64_t spBeat,
                                                                  const int divisionsPerBeat) noexcept
{
    if (divisionsPerBeat <= 1 || spBeat <= 0)
    {
        return 0;
    }
    const std::int64_t q = spBeat / divisionsPerBeat;
    return (q > 0 && q * divisionsPerBeat == spBeat) ? q : 0;
}

[[nodiscard]] inline ArrangementMusicalGridPlan computeArrangementMusicalGridPlan(
    ProjectMusicalTime mt,
    double displaySampleRate,
    double samplesPerPixel) noexcept
{
    ArrangementMusicalGridPlan plan;
    mt = sanitizeProjectMusicalTime(mt);
    if (!std::isfinite(displaySampleRate) || displaySampleRate <= 0.0
        || !std::isfinite(samplesPerPixel) || samplesPerPixel <= 0.0)
    {
        return plan;
    }

    const double spBeatD = samplesPerBeat(mt, displaySampleRate);
    const double spBarD = samplesPerBar(mt, displaySampleRate);
    if (spBeatD <= 0.0 || spBarD <= 0.0)
    {
        return plan;
    }

    plan.spBeat = static_cast<std::int64_t>(std::llround(spBeatD));
    plan.spBar = static_cast<std::int64_t>(std::llround(spBarD));
    if (plan.spBeat < 1)
    {
        plan.spBeat = 1;
    }
    if (plan.spBar < plan.spBeat)
    {
        plan.spBar = plan.spBeat * std::max(1, mt.numerator);
    }

    const double pxPerBeat = static_cast<double>(plan.spBeat) / samplesPerPixel;
    const double pxPerBar = static_cast<double>(plan.spBar) / samplesPerPixel;

    int barsPerMajor = 1;
    constexpr double kMinMajorLabelPx = 72.0;
    while (static_cast<double>(barsPerMajor) * pxPerBar < kMinMajorLabelPx && barsPerMajor < 4096)
    {
        barsPerMajor *= 2;
    }
    plan.barsPerMajor = barsPerMajor;

    plan.drawEveryBarLine = pxPerBar >= 10.0 || barsPerMajor == 1;
    plan.drawBeatLines = pxPerBeat >= 7.0;
    plan.subDivisionsPerBeat = 0;
    if (pxPerBeat >= 48.0)
    {
        plan.subDivisionsPerBeat = 4;
    }
    else if (pxPerBeat >= 26.0)
    {
        plan.subDivisionsPerBeat = 2;
    }

    plan.labelsUseBarBeatDetail = pxPerBeat >= 32.0 && barsPerMajor == 1;

    return plan;
}
