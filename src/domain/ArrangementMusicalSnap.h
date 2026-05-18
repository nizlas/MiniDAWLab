#pragma once

// =============================================================================
// ArrangementMusicalSnap — timeline sample snapping to project musical grid
// =============================================================================
// Grid uses `ProjectMusicalTime` (quarter-note beat axis) + device sample rate.
// Snap is **not** quantize: editors call this only while applying new positions.
//
// **Straight 1/N**: step = (4/N) quarter-note beats.
// **Triplet "1/N Triplet"**: step = straight(1/N) * 2/3.
// **Dotted "1/N Dotted"**: step = straight(1/N) * 3/2.

#include "domain/MusicalTimeConversions.h"
#include "domain/ProjectMusicalTime.h"
#include "ui/SnapSettings.h"

#include <cmath>
#include <cstdint>

namespace arrangement_snap_detail
{
[[nodiscard]] inline double straightGridStepBeatsFromDenominator(int denom) noexcept
{
    if (denom <= 0)
        return 0.0;
    return 4.0 / static_cast<double>(denom);
}
} // namespace arrangement_snap_detail

/// Beat length of one snap cell for the given resolution (quarter-note beat axis).
[[nodiscard]] inline double arrangementSnapGridStepBeats(SnapResolution resolution,
                                                          ProjectMusicalTime mt) noexcept
{
    mt = sanitizeProjectMusicalTime(mt);
    double straight = 0.0;

    switch (resolution)
    {
    case SnapResolution::Straight_1_1:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(1);
        break;
    case SnapResolution::Straight_1_2:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(2);
        break;
    case SnapResolution::Straight_1_4:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(4);
        break;
    case SnapResolution::Straight_1_8:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(8);
        break;
    case SnapResolution::Straight_1_16:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(16);
        break;
    case SnapResolution::Straight_1_32:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(32);
        break;
    case SnapResolution::Straight_1_64:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(64);
        break;
    case SnapResolution::Straight_1_128:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(128);
        break;

    case SnapResolution::Triplet_1_2:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(2);
        break;
    case SnapResolution::Triplet_1_4:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(4);
        break;
    case SnapResolution::Triplet_1_8:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(8);
        break;
    case SnapResolution::Triplet_1_16:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(16);
        break;
    case SnapResolution::Triplet_1_32:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(32);
        break;
    case SnapResolution::Triplet_1_64:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(64);
        break;

    case SnapResolution::Dotted_1_2:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(2);
        break;
    case SnapResolution::Dotted_1_4:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(4);
        break;
    case SnapResolution::Dotted_1_8:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(8);
        break;
    case SnapResolution::Dotted_1_16:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(16);
        break;
    case SnapResolution::Dotted_1_32:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(32);
        break;
    case SnapResolution::Dotted_1_64:
        straight = arrangement_snap_detail::straightGridStepBeatsFromDenominator(64);
        break;
    }

    switch (resolution)
    {
    case SnapResolution::Triplet_1_2:
    case SnapResolution::Triplet_1_4:
    case SnapResolution::Triplet_1_8:
    case SnapResolution::Triplet_1_16:
    case SnapResolution::Triplet_1_32:
    case SnapResolution::Triplet_1_64:
    {
        const double v = straight * (2.0 / 3.0);
        return (std::isfinite(v) && v > 0.0) ? v : straight;
    }
    case SnapResolution::Dotted_1_2:
    case SnapResolution::Dotted_1_4:
    case SnapResolution::Dotted_1_8:
    case SnapResolution::Dotted_1_16:
    case SnapResolution::Dotted_1_32:
    case SnapResolution::Dotted_1_64:
    {
        const double v = straight * 1.5;
        return (std::isfinite(v) && v > 0.0) ? v : straight;
    }
    default:
        break;
    }

    return (std::isfinite(straight) && straight > 0.0) ? straight : 1.0;
}

/// Nearest grid-aligned timeline sample (>= 0). Uses beat grid then `beatToSample`.
[[nodiscard]] inline std::int64_t snapSampleToGrid(std::int64_t sampleOnTimeline,
                                                   ProjectMusicalTime mt,
                                                   SnapResolution resolution,
                                                   double sampleRate) noexcept
{
    mt = sanitizeProjectMusicalTime(mt);
    sampleOnTimeline = (sampleOnTimeline < 0) ? 0 : sampleOnTimeline;

    const double stepBeats = arrangementSnapGridStepBeats(resolution, mt);
    if (!std::isfinite(stepBeats) || stepBeats <= 0.0)
    {
        return sampleOnTimeline;
    }

    const double spb = samplesPerBeat(mt, sampleRate);
    if (!std::isfinite(spb) || spb <= 0.0)
    {
        return sampleOnTimeline;
    }

    const double beats = static_cast<double>(sampleOnTimeline) / spb;
    if (!std::isfinite(beats))
    {
        return sampleOnTimeline;
    }

    const double snappedBeats = std::round(beats / stepBeats) * stepBeats;
    const std::int64_t out = beatToSample(snappedBeats, mt, sampleRate);
    return (out < 0) ? std::int64_t{ 0 } : out;
}

[[nodiscard]] inline std::int64_t snapSampleToGridIfEnabled(std::int64_t sampleOnTimeline,
                                                             const SnapSettings& settings,
                                                             ProjectMusicalTime mt,
                                                             double sampleRate) noexcept
{
    if (!settings.enabled)
    {
        return (sampleOnTimeline < 0) ? std::int64_t{ 0 } : sampleOnTimeline;
    }
    return snapSampleToGrid(sampleOnTimeline, mt, settings.resolution, sampleRate);
}
