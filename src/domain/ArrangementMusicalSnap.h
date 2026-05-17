#pragma once

// =============================================================================
// ArrangementMusicalSnap — timeline sample snapping to project musical grid (Slice D)
// =============================================================================
// Uses `ProjectMusicalTime` + device/display sample rate; grid matches arrangement ruler basis.

#include "domain/MusicalTimeConversions.h"
#include "domain/ProjectMusicalTime.h"
#include "ui/SnapSettings.h"

#include <cmath>
#include <cstdint>

/// Beat length of one snap cell for the given resolution (quarter-note beat axis).
[[nodiscard]] inline double arrangementSnapGridStepBeats(SnapResolution resolution,
                                                          ProjectMusicalTime mt) noexcept
{
    mt = sanitizeProjectMusicalTime(mt);
    switch (resolution)
    {
    case SnapResolution::Bar:
        return beatsPerBar(mt);
    case SnapResolution::Half:
        return 2.0;
    case SnapResolution::Quarter:
        return 1.0;
    case SnapResolution::Eighth:
        return 0.5;
    case SnapResolution::Sixteenth:
        return 0.25;
    default:
        return 1.0;
    }
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
