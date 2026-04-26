#pragma once

// =============================================================================
// TimelineViewportModel  —  visible [start, start+length) in session samples (message thread, UI
// only)
// =============================================================================
//
// Holds the **horizontal** pan + scale denominator shared by `TimelineRulerView` and
// `ClipWaveformView`, separate from `Session` / `SessionSnapshot` / `Transport`. The session’s
// **arrangement extent** (navigable / playable) lives on `SessionSnapshot`; this model only
// controls which slice of the arrangement is shown in the view.
//
// Never stored in `Session` or project files. See `PHASE_PLAN` / `DECISION_LOG`.
// =============================================================================

#include <cstdint>
#include <functional>

// ---------------------------------------------------------------------------
// [Message thread] Maps half-open [visibleStart, visibleStart + visibleLength) to lane / ruler
// width. Pan does not change `visibleLength` (zoom is a follow-up); wheel adjusts `visibleStart`.
// ---------------------------------------------------------------------------
class TimelineViewportModel
{
public:
    using OnVisibleRangeChanged = std::function<void()>;

    explicit TimelineViewportModel(OnVisibleRangeChanged onRangeChanged = {});

    void setOnVisibleRangeChanged(OnVisibleRangeChanged onRangeChanged);

    [[nodiscard]] std::int64_t getVisibleStartSamples() const noexcept;
    /// Denominator for x mapping (at least 1 when used for draw).
    [[nodiscard]] std::int64_t getVisibleLengthSamples() const noexcept;
    /// `visibleStart + max(1, visibleLength)` — end sample exclusive for the panned world line.
    [[nodiscard]] std::int64_t getVisibleEndSamples() const noexcept;

    /// [Message thread] If `visibleLength` is still 0, set to `samples` and notify. Used at startup.
    void setVisibleLengthIfUnset(std::int64_t samples) noexcept;

    // [Message thread] `delta < 0` = pan left (decrease `visibleStart`). `arrangementExtent` is
    // `Session::getArrangementExtentSamples()` (max seek / pan bound).
    void panBySamples(std::int64_t delta, std::int64_t arrangementExtent) noexcept;

    // [Message thread] If `visibleStart + visibleLength` extends past `arrangementExtent`, shift
    // `visibleStart` left. After session mutations that can shrink apparent extent, call with the
    // current `getArrangementExtentSamples()`.
    void clampToExtent(std::int64_t arrangementExtent) noexcept;

private:
    std::int64_t visibleStartSamples_ = 0;
    std::int64_t visibleLengthSamples_ = 0;
    OnVisibleRangeChanged onVisibleRangeChanged_;
};
