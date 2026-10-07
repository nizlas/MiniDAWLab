#pragma once

#include <optional>

#include <juce_core/juce_core.h>

// =============================================================================
// TrackRowHeightPresets — the ONE central definition of the shared arrangement
// track heights Small / Medium / Large (logical px), their persistence keys and
// the mapping helpers used by the toolbar dropdown, `TrackLanesView` and the
// project save/load path. No other file may hard-code these heights.
// =============================================================================

namespace track_row_heights
{

/// Shared arrangement row-height preset. Selecting one sets EVERY existing
/// arrangement track (including Group and Stereo Out rows, including rows that
/// are scrolled out of the viewport) to the same height in one layout pass and
/// becomes the default height for tracks created afterwards. It is a one-shot
/// command, not a locked mode: individual bottom-edge drags still work and the
/// dropdown then reports "Custom" until all rows match a preset again.
enum class TrackRowHeightPreset
{
    Small = 0,
    Medium = 1,
    Large = 2,
};

/// Small = the smallest row height where the title row, the FULL main button
/// strip (including the Solo cell and the instrument-editor cell) and the
/// bottom resize band all fit without overlap for every row kind. The binding
/// case is a row with a subtitle (instrument / MIDI rows):
///   outer pad Y (4) + name block with subtitle (30) + name-to-buttons gap (3)
///   + control-strip cell (22) + resize band (5) = 64.
/// A `static_assert` in `TrackHeaderView.cpp` ties this constant to the actual
/// header geometry constants, so it cannot silently drift. At this height the
/// small standalone proxy/alternatives button does not fit and hides entirely
/// (no paint, no hit target, no tooltip — see `getAlternativesButtonBounds`).
inline constexpr int kSmallRowHeightPx = 64;

/// Medium = the pre-existing normal default row height in DAL (unchanged).
inline constexpr int kMediumRowHeightPx = 96;

/// Large = clearly bigger with a simple documented proportion: 2 × Medium.
inline constexpr int kLargeRowHeightPx = 192;

[[nodiscard]] constexpr int heightPxForPreset(const TrackRowHeightPreset p) noexcept
{
    switch (p)
    {
    case TrackRowHeightPreset::Small:
        return kSmallRowHeightPx;
    case TrackRowHeightPreset::Large:
        return kLargeRowHeightPx;
    case TrackRowHeightPreset::Medium:
    default:
        return kMediumRowHeightPx;
    }
}

/// The preset a height exactly corresponds to, or nullopt (a custom height).
[[nodiscard]] constexpr std::optional<TrackRowHeightPreset>
presetMatchingHeightPx(const int px) noexcept
{
    if (px == kSmallRowHeightPx)
    {
        return TrackRowHeightPreset::Small;
    }
    if (px == kMediumRowHeightPx)
    {
        return TrackRowHeightPreset::Medium;
    }
    if (px == kLargeRowHeightPx)
    {
        return TrackRowHeightPreset::Large;
    }
    return std::nullopt;
}

/// Project-file key (root `trackRowHeightPreset`, v26): "small" | "medium" | "large".
[[nodiscard]] inline juce::String persistenceKeyForPreset(const TrackRowHeightPreset p)
{
    switch (p)
    {
    case TrackRowHeightPreset::Small:
        return "small";
    case TrackRowHeightPreset::Large:
        return "large";
    case TrackRowHeightPreset::Medium:
    default:
        return "medium";
    }
}

/// Absent key (every pre-v26 project) and any unknown value load as Medium —
/// the historical default — so older projects keep their established look.
[[nodiscard]] inline TrackRowHeightPreset presetFromPersistenceKey(const juce::String& key)
{
    if (key == "small")
    {
        return TrackRowHeightPreset::Small;
    }
    if (key == "large")
    {
        return TrackRowHeightPreset::Large;
    }
    return TrackRowHeightPreset::Medium;
}

} // namespace track_row_heights
