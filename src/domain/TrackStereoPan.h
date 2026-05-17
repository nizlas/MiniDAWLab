#pragma once

// TrackStereoPan — arrangement track stereo pan helpers (linear balance law; center-preserving).
// Pan applies after channel fader/mute in `PlaybackEngine` / `ExperimentalInstrumentHost`. Equal-power
// pan at center was not used so existing centered mixes stay at the same perceived level (v1).

#include <cmath>

#include <juce_core/juce_core.h>

/// Clamp stored / incoming pan to [-1, +1] (full left … full right).
[[nodiscard]] inline float sanitizeTrackStereoPan(double pan) noexcept
{
    if (!std::isfinite(pan))
    {
        return 0.0f;
    }
    return juce::jlimit(-1.0f, 1.0f, static_cast<float>(pan));
}

/// Linear balance pan (v1): center keeps L=R=1 so centered tracks match pre-pan gain.
/// Full left: L=1 R=0; full right: L=0 R=1. Applied after channel fader gain.
[[nodiscard]] inline float trackPanLawGainLeft(float pan) noexcept
{
    pan = sanitizeTrackStereoPan(pan);
    return (pan <= 0.0f) ? 1.0f : (1.0f - pan);
}

[[nodiscard]] inline float trackPanLawGainRight(float pan) noexcept
{
    pan = sanitizeTrackStereoPan(pan);
    return (pan <= 0.0f) ? (1.0f + pan) : 1.0f;
}

/// DAW-style pan readout: L / L1…L99 / C / R1…R99 / R (internal pan never shown raw).
[[nodiscard]] inline juce::String formatPanForDisplay(float pan) noexcept
{
    pan = sanitizeTrackStereoPan(pan);
    constexpr float kCenterDeadband = 0.005f;
    if (std::fabs(pan) <= kCenterDeadband)
    {
        return "C";
    }
    constexpr float kFullEps = 1.0e-4f;
    if (pan <= -1.0f + kFullEps)
    {
        return "L";
    }
    if (pan >= 1.0f - kFullEps)
    {
        return "R";
    }
    const int amt = static_cast<int>(std::lround(std::fabs(static_cast<double>(pan)) * 100.0));
    const int clamped = juce::jlimit(1, 99, amt);
    if (pan < 0.0f)
    {
        return "L" + juce::String(clamped);
    }
    return "R" + juce::String(clamped);
}
