#pragma once

// =============================================================================
// PlayheadPixelMapping — one time→pixel convention for every current-time indicator
// =============================================================================
//
// ROLE
//   All visible current-time/playhead markers (main ruler stroke, lanes overlay line, MIDI editor
//   ruler stroke and grid line) must land on the **same** pixel column for the same playback
//   position. That only holds if they share three things: the same display **sample**, the same
//   sample→x formula, and the same rounding.
//
// CONVENTION (chosen here, referenced from every renderer)
//   1. Position is a **double** session-sample value (so smoothed/extrapolated UI positions keep
//      sub-sample resolution instead of being truncated per component).
//   2. `x = originX + (sample - visibleStart) / samplesPerPixel` — identical to
//      `TimelineRulerView::sessionSampleToLocalX`, in the **local** coordinates of the component
//      that owns `originX`.
//   3. Strokes are drawn at the **pixel centre**: `floor(x) + 0.5`. JUCE strokes straddle the
//      coordinate, so an integral x smears a 1–1.5 px line across two device columns (the visible
//      shimmer) while floor+0.5 keeps it crisp and makes two components agree on one column even
//      when their unrounded x differ by a fraction of a pixel.
//
// THREADING
//   Pure functions, [Message thread] paint/timer use only.
// =============================================================================

#include <cmath>
#include <cstdint>

#include <juce_graphics/juce_graphics.h>

namespace playhead_pixel
{
/// Canonical sample→local-x map (see convention 2). Returns `originX` when zoom is unusable.
[[nodiscard]] inline float localXForSample(const double sessionSample,
                                           const float originX,
                                           const std::int64_t visibleStartSamples,
                                           const double samplesPerPixel) noexcept
{
    if (!(samplesPerPixel > 0.0) || !std::isfinite(samplesPerPixel) || !std::isfinite(sessionSample))
    {
        return originX;
    }
    return originX + (float)((sessionSample - (double)visibleStartSamples) / samplesPerPixel);
}

/// Pixel-centre snap (see convention 3). Use for every current-time stroke.
[[nodiscard]] inline float snapToPixelCentre(const float x) noexcept
{
    return std::floor(x) + 0.5f;
}

/// Narrow invalidation stripe around a stroke at `centreX`, covering `[top, bottom)` and one pixel
/// of slack per side so antialiased edges are always inside the repainted region.
[[nodiscard]] inline juce::Rectangle<int> dirtyStripe(const float centreX,
                                                      const int top,
                                                      const int bottom,
                                                      const float thicknessPx) noexcept
{
    if (!std::isfinite(centreX) || bottom <= top)
    {
        return {};
    }
    const int halfSpan = (int)std::ceil(thicknessPx * 0.5f) + 2;
    const int cx = (int)std::floor(centreX);
    return { cx - halfSpan, top, halfSpan * 2 + 1, bottom - top };
}
} // namespace playhead_pixel
