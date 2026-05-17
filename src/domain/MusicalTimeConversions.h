#pragma once

// =============================================================================
// MusicalTimeConversions — sample ↔ beat helpers (explicit sample rate; no allocations)
// =============================================================================

#include "domain/ProjectMusicalTime.h"

#include <cmath>
#include <cstdint>

[[nodiscard]] inline double beatsPerBar(ProjectMusicalTime mt) noexcept
{
    mt = sanitizeProjectMusicalTime(mt);
    return static_cast<double>(mt.numerator) * 4.0 / static_cast<double>(mt.denominator);
}

[[nodiscard]] inline double samplesPerBeat(ProjectMusicalTime mt, double sampleRate) noexcept
{
    mt = sanitizeProjectMusicalTime(mt);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        return 0.0;
    }
    return sampleRate * (60.0 / mt.bpm);
}

[[nodiscard]] inline double samplesPerBar(ProjectMusicalTime mt, double sampleRate) noexcept
{
    const double spb = samplesPerBeat(mt, sampleRate);
    if (spb <= 0.0)
    {
        return 0.0;
    }
    return spb * beatsPerBar(mt);
}

[[nodiscard]] inline double sampleToBeatPosition(std::int64_t samples,
                                                 ProjectMusicalTime mt,
                                                 double sampleRate) noexcept
{
    const double spb = samplesPerBeat(mt, sampleRate);
    if (spb <= 0.0)
    {
        return 0.0;
    }
    return static_cast<double>(samples) / spb;
}

[[nodiscard]] inline std::int64_t beatToSample(double beats,
                                               ProjectMusicalTime mt,
                                               double sampleRate) noexcept
{
    const double spb = samplesPerBeat(mt, sampleRate);
    if (spb <= 0.0 || !std::isfinite(beats))
    {
        return 0;
    }
    const double raw = beats * spb;
    if (!std::isfinite(raw))
    {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(raw));
}
