#pragma once

// =============================================================================
// TimelineViewportModel  —  scroll position + time scale in session samples (message thread, UI
// only)
// =============================================================================
//
// Holds **visibleStart** (sample index) and **samplesPerPixel** (zoom: higher = more zoomed *out*).
// The visible *length* in samples is always **derived** from the timeline **pixel** width: roughly
// `round(widthPx * samplesPerPixel)` — it is **not** stored, so **resizing the window does not
// change scale**; it shows more or less time at the same zoom.
//
// Ruler and lane use the same `x → sample` rule (see `TimelineRulerView::xToSessionSampleClamped`):
//   `s = visStart + round(xClamped * samplesPerPixel)`.
//
// The session’s **arrangement extent** lives on `SessionSnapshot`. Never stored in project files
// (viewport is UI-only). See `PHASE_PLAN` / `DECISION_LOG`.
// =============================================================================

#include <cstdint>
#include <functional>

// ---------------------------------------------------------------------------
// [Message thread] Pan only changes `visibleStart`. Zoom only changes `samplesPerPixel` (and the
// one-time `setSamplesPerPixelIfUnset` seed). After session mutations that can shrink `ext`, call
// `clampToExtent(widthPx, ext)`.
// ---------------------------------------------------------------------------
class TimelineViewportModel
{
public:
    using OnVisibleRangeChanged = std::function<void()>;

    explicit TimelineViewportModel(OnVisibleRangeChanged onRangeChanged = {});

    void setOnVisibleRangeChanged(OnVisibleRangeChanged onRangeChanged);

    [[nodiscard]] std::int64_t getVisibleStartSamples() const noexcept;
    /// Pixels of timeline width per *one* session sample. Higher = more zoomed out. Set after seed.
    [[nodiscard]] double getSamplesPerPixel() const noexcept;

    /// [Message thread] If `samplesPerPixel_` is still 0, set to `spp` and notify. Used at startup
    /// (e.g. from device sample rate: `spp = sampleRate / defaultPixelsPerSecond`).
    void setSamplesPerPixelIfUnset(double samplesPerPixel) noexcept;

    /// Derived: `round(widthPx * samplesPerPixel)` (at least 1 when `samplesPerPixel_` is set).
    [[nodiscard]] std::int64_t getVisibleLengthSamples(double widthPx) const noexcept;
    /// `visibleStart` + `getVisibleLengthSamples(widthPx)`.
    [[nodiscard]] std::int64_t getVisibleEndSamples(double widthPx) const noexcept;

    // [Message thread] `delta < 0` = pan left. `widthPx` is the timeline **column** width (ruler
    // full width, or lane width without track header). `ext` = `getArrangementExtentSamples()`.
    void panBySamples(
        std::int64_t delta, double widthPx, std::int64_t arrangementExtent) noexcept;

    // [Message thread] If `visStart + visibleLength(width)` extends past `ext`, shift `visStart` left.
    void clampToExtent(double widthPx, std::int64_t arrangementExtent) noexcept;

    /// [Message thread] **Only** path that changes `samplesPerPixel_` after `setSamplesPerPixelIfUnset`.
    /// `pointerXPx` is **local** x in the same coordinates as `widthPx` (0 = left of timeline
    /// column). Keeps the sample at that pixel column anchored. `sppMin` / `sppMax` are samples per
    /// pixel; typical `sppMax = jmax(1, ext/width)`.
    void zoomAroundSample(
        double factor,
        double pointerXPx,
        double widthPx,
        std::int64_t arrangementExtent,
        double samplesPerPixelMin,
        double samplesPerPixelMax) noexcept;

private:
    std::int64_t visibleStartSamples_ = 0;
    double       samplesPerPixel_     = 0.0;
    OnVisibleRangeChanged onVisibleRangeChanged_;
};
