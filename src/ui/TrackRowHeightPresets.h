#pragma once

#include <optional>

#include <juce_core/juce_core.h>

// =============================================================================
// TrackRowHeightPresets — the ONE central definition of the arrangement row-height
// grid (logical px): the title-row geometry it is derived from, the shared presets
// Micro / Mini / Small / Medium / Large, the grid snapping rule, the persistence
// keys and the lane event-detail thresholds. No other file may hard-code these.
// =============================================================================

namespace track_row_heights
{

// -----------------------------------------------------------------------------
// Title-row geometry the grid is derived from (shared with `TrackHeaderView`,
// which `static_assert`s its own constants against these).
// -----------------------------------------------------------------------------
/// The square control cell (Power / Mute / Solo / Monitor / Arm / editor) and therefore the
/// height of the header's title row: the 14 pt name and the three title-row cells share it.
inline constexpr int kHeaderControlCellPx = 22;
/// Vertical padding above the title row (every height keeps the title row top-aligned here, so
/// dragging a row never makes its name jump).
inline constexpr int kHeaderRowTopPadPx = 2;
/// Gap between the title row and the second control row (Monitor / Arm / editor / alternatives).
inline constexpr int kHeaderRowGapPx = 3;
/// Bottom-edge resize band inside the header (hit area for the row-height drag; it sits BELOW the
/// title row in every height, so even Micro keeps a usable resize zone that never steals a click).
inline constexpr int kHeaderResizeBandPx = 4;

// -----------------------------------------------------------------------------
// The grid: height = kMicroRowHeightPx + n * kRowHeightStepPx, n >= 0.
// -----------------------------------------------------------------------------
/// Micro = ONE title row with minimal vertical margin: top pad + control cell + resize band
/// = 2 + 22 + 4 = 28 px. Icon, number, Power / Mute / Solo and the name are fully visible and
/// clickable; nothing else fits.
inline constexpr int kMicroRowHeightPx = kHeaderRowTopPadPx + kHeaderControlCellPx + kHeaderResizeBandPx;
/// Step = half a Micro row (14 px). It is the smallest even step for which Small (two steps)
/// equals two stacked title rows: the second control row needs cell (22) + row gap (3) = 25 px
/// <= 2 x 14 = 28 px. Mini (one step) adds 14 px of air under the single title row.
inline constexpr int kRowHeightStepPx = kMicroRowHeightPx / 2;
static_assert(kMicroRowHeightPx == 28 && kRowHeightStepPx == 14, "row-height grid: documented values");
static_assert(2 * kRowHeightStepPx >= kHeaderControlCellPx + kHeaderRowGapPx,
              "Small (two steps above Micro) must hold the second control row");

/// Shared arrangement row-height preset. Selecting one sets EVERY normal arrangement row
/// (including Group and Stereo Out rows, including rows scrolled out of view and the hidden
/// members of collapsed visual groups — their NORMAL height) to the preset height in one layout
/// pass and becomes the default height for tracks created afterwards. One-shot command, not a
/// mode: individual bottom-edge drags still work and the dropdown then reports "Custom" until all
/// rows match a preset again.
enum class TrackRowHeightPreset
{
    Micro = 0,
    Mini = 1,
    Small = 2,
    Medium = 3,
    Large = 4,
};

/// Grid index n of each preset (Micro / Mini / Small are three adjacent steps; Medium is at
/// least two steps above Small, Large at least two above Medium).
[[nodiscard]] constexpr int gridStepsForPreset(const TrackRowHeightPreset p) noexcept
{
    switch (p)
    {
    case TrackRowHeightPreset::Micro:
        return 0;
    case TrackRowHeightPreset::Mini:
        return 1;
    case TrackRowHeightPreset::Small:
        return 2;
    case TrackRowHeightPreset::Large:
        return 12;
    case TrackRowHeightPreset::Medium:
    default:
        return 5;
    }
}

[[nodiscard]] constexpr int heightPxForGridSteps(const int n) noexcept
{
    return kMicroRowHeightPx + (n < 0 ? 0 : n) * kRowHeightStepPx;
}

[[nodiscard]] constexpr int heightPxForPreset(const TrackRowHeightPreset p) noexcept
{
    return heightPxForGridSteps(gridStepsForPreset(p));
}

/// Micro 28 | Mini 42 | Small 56 (two title rows) | Medium 98 (~ the old 96 default) |
/// Large 196 (~ 2 x Medium, the old 192).
inline constexpr int kMicroPresetPx = heightPxForPreset(TrackRowHeightPreset::Micro);
inline constexpr int kMiniPresetPx = heightPxForPreset(TrackRowHeightPreset::Mini);
inline constexpr int kSmallPresetPx = heightPxForPreset(TrackRowHeightPreset::Small);
inline constexpr int kMediumPresetPx = heightPxForPreset(TrackRowHeightPreset::Medium);
inline constexpr int kLargePresetPx = heightPxForPreset(TrackRowHeightPreset::Large);
static_assert(kMicroPresetPx == 28 && kMiniPresetPx == 42 && kSmallPresetPx == 56 && kMediumPresetPx == 98
                  && kLargePresetPx == 196,
              "preset heights: documented values");
static_assert(kSmallPresetPx == 2 * kMicroRowHeightPx, "Small is exactly two stacked title rows");
static_assert(gridStepsForPreset(TrackRowHeightPreset::Medium) >= gridStepsForPreset(TrackRowHeightPreset::Small) + 2
                  && gridStepsForPreset(TrackRowHeightPreset::Large) >= gridStepsForPreset(TrackRowHeightPreset::Medium) + 2,
              "Medium >= Small + 2 steps and Large >= Medium + 2 steps");

/// The minimum of every normal row (= Micro). Collapsed visual-group members display as 4 px
/// strips through a separate mechanism; that display height is NOT subject to this grid.
inline constexpr int kMinRowHeightPx = kMicroRowHeightPx;
/// Documented safety cap, NOT a normal-use limit (steps above Large are allowed freely): protects
/// layout from malformed file metadata / absurd geometry. 28 + 78 x 14 = 1120 px, grid-aligned.
inline constexpr int kRowHeightSafetyMaxPx = heightPxForGridSteps(78);
static_assert(kRowHeightSafetyMaxPx == 1120, "safety cap: documented value");

/// Clamp to the legal range WITHOUT snapping (project load keeps valid older off-grid heights as
/// "Custom" until the user resizes or picks a preset).
[[nodiscard]] constexpr int clampRowHeightPx(const int px) noexcept
{
    return px < kMinRowHeightPx ? kMinRowHeightPx : (px > kRowHeightSafetyMaxPx ? kRowHeightSafetyMaxPx : px);
}

/// Snap to the nearest grid height (ties round up), clamped. Drags call this with the drag's
/// ORIGINAL height + total movement, so the result is a pure function of the pointer position and
/// cannot flutter at a step boundary.
[[nodiscard]] constexpr int snapRowHeightPxToGrid(const int px) noexcept
{
    const int clamped = clampRowHeightPx(px);
    const int steps = (clamped - kMicroRowHeightPx + kRowHeightStepPx / 2) / kRowHeightStepPx;
    return clampRowHeightPx(heightPxForGridSteps(steps));
}

[[nodiscard]] constexpr bool isOnRowHeightGrid(const int px) noexcept
{
    return px >= kMinRowHeightPx && px <= kRowHeightSafetyMaxPx
           && (px - kMicroRowHeightPx) % kRowHeightStepPx == 0;
}

/// The preset a height exactly corresponds to, or nullopt (a custom height).
[[nodiscard]] constexpr std::optional<TrackRowHeightPreset> presetMatchingHeightPx(const int px) noexcept
{
    for (const TrackRowHeightPreset p : { TrackRowHeightPreset::Micro, TrackRowHeightPreset::Mini,
                                          TrackRowHeightPreset::Small, TrackRowHeightPreset::Medium,
                                          TrackRowHeightPreset::Large })
    {
        if (heightPxForPreset(p) == px)
        {
            return p;
        }
    }
    return std::nullopt;
}

/// Project-file key (root `trackRowHeightPreset`, v26+): "micro" | "mini" | "small" | "medium" | "large".
[[nodiscard]] inline juce::String persistenceKeyForPreset(const TrackRowHeightPreset p)
{
    switch (p)
    {
    case TrackRowHeightPreset::Micro:
        return "micro";
    case TrackRowHeightPreset::Mini:
        return "mini";
    case TrackRowHeightPreset::Small:
        return "small";
    case TrackRowHeightPreset::Large:
        return "large";
    case TrackRowHeightPreset::Medium:
    default:
        return "medium";
    }
}

/// Absent key (every pre-v26 project) and any unknown value load as Medium — the historical
/// default — so older projects keep their established look.
[[nodiscard]] inline TrackRowHeightPreset presetFromPersistenceKey(const juce::String& key)
{
    if (key == "micro")
    {
        return TrackRowHeightPreset::Micro;
    }
    if (key == "mini")
    {
        return TrackRowHeightPreset::Mini;
    }
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

[[nodiscard]] inline juce::String displayNameForPreset(const TrackRowHeightPreset p)
{
    switch (p)
    {
    case TrackRowHeightPreset::Micro:
        return "Micro";
    case TrackRowHeightPreset::Mini:
        return "Mini";
    case TrackRowHeightPreset::Small:
        return "Small";
    case TrackRowHeightPreset::Large:
        return "Large";
    case TrackRowHeightPreset::Medium:
    default:
        return "Medium";
    }
}

// -----------------------------------------------------------------------------
// Lane event detail (painting only — hit geometry, clip functions and musical data are
// untouched). Shared by the audio lanes (`ClipWaveformView`) and the MIDI lanes.
// -----------------------------------------------------------------------------
enum class LaneEventDetail
{
    /// Events as thin fields showing their real time extent only (no waveform / notes / text).
    Bars,
    /// The event box WITH its content illustration (audio waveform / MIDI note preview, scaled
    /// and clipped to the box's inner area) but WITHOUT the name label — the content has
    /// priority when the height does not allow both.
    Content,
    /// The existing full rendering (waveform / note preview + name label).
    Full,
};

/// Content needs the event box itself to be readable: 2 x the 4 px event margin + a 24 px body
/// = 32 px (Mini 42 qualifies, Micro 28 does not). Decided from the lane's ACTUAL height, so an
/// older saved Custom height between the presets behaves sensibly as well.
inline constexpr int kLaneDetailContentMinPx = 32;
/// The name label additionally needs its 14 px top-left strip above at least 24 px of waveform /
/// note area inside the box margins: 8 + 14 + 2 + 24 = 48 px (Small 56 qualifies, Mini 42 does not).
inline constexpr int kLaneDetailFullMinPx = 48;
/// Height of the thin event field in `Bars` mode, centred in the lane.
inline constexpr int kLaneBarFieldHeightPx = 8;
static_assert(kMicroPresetPx < kLaneDetailContentMinPx && kMiniPresetPx >= kLaneDetailContentMinPx
                  && kMiniPresetPx < kLaneDetailFullMinPx && kSmallPresetPx >= kLaneDetailFullMinPx,
              "detail thresholds: Micro = Bars, Mini = Content (no label), Small and above = Full");

[[nodiscard]] constexpr LaneEventDetail laneEventDetailForHeightPx(const int laneHeightPx) noexcept
{
    if (laneHeightPx >= kLaneDetailFullMinPx)
    {
        return LaneEventDetail::Full;
    }
    if (laneHeightPx >= kLaneDetailContentMinPx)
    {
        return LaneEventDetail::Content;
    }
    return LaneEventDetail::Bars;
}

} // namespace track_row_heights
