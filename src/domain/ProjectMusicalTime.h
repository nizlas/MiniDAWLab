#pragma once

// =============================================================================
// ProjectMusicalTime — global project tempo / meter metadata (Slice A: no playback effect)
// =============================================================================
// Stored on `SessionSnapshot` for lock-free reads alongside timeline state.
// Independent from per–MIDI-clip `pattern.bpm` on experimental instrument clips.

#include <cmath>

struct ProjectMusicalTime
{
    double bpm = 120.0;
    int numerator = 4;
    int denominator = 4;
    int ticksPerQuarter = 960;
};

[[nodiscard]] inline ProjectMusicalTime sanitizeProjectMusicalTime(ProjectMusicalTime m) noexcept
{
    if (!std::isfinite(m.bpm) || m.bpm <= 0.0)
    {
        m.bpm = 120.0;
    }
    else if (m.bpm < 20.0)
    {
        m.bpm = 20.0;
    }
    else if (m.bpm > 300.0)
    {
        m.bpm = 300.0;
    }

    if (m.numerator < 1 || m.numerator > 64)
    {
        m.numerator = 4;
    }
    if (m.denominator < 1 || m.denominator > 64)
    {
        m.denominator = 4;
    }

    if (m.ticksPerQuarter < 1 || m.ticksPerQuarter > 384000)
    {
        m.ticksPerQuarter = 960;
    }

    return m;
}
