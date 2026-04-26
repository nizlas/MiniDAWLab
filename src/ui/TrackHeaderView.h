#pragma once

// =============================================================================
// TrackHeaderView  —  minimal label + active highlight; header-drag for track reorder (message)
// =============================================================================
// `mouseDown` on the **name** region calls `Session::setActiveTrack` (no snapshot republish). The
// right **arm** strip (`R` / record-arm) calls `RecorderService::armForRecording` / `disarm` only.
// Drag past a threshold (from the name strip) is coordinated by `TrackLanesView` (insert line,
// `Session::moveTrack`). The event lane to the right is not a header-drag target. Invalid drop uses
// `getForbiddenNoDropMouseCursor` (`ForbiddenCursor.h`).
// =============================================================================

#include "domain/Track.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

class RecorderService;
class Session;
class TrackHeaderView;

// [Message thread] `TrackLanesView` implements these; Began/Ended pair with move updates.
struct TrackHeaderDragHost
{
    std::function<void(TrackId, TrackHeaderView*)> onHeaderDragBegan;
    std::function<void(TrackId, juce::Point<int>)> onHeaderDragMoved;
    std::function<void(TrackId)> onHeaderDragEnded;
};

class TrackHeaderView : public juce::Component
{
public:
    TrackHeaderView(
        Session& session,
        RecorderService& recorder,
        TrackId trackId,
        std::function<void()> onActiveChanged,
        std::function<void()> onArmStateChanged,
        TrackHeaderDragHost dragHost) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // [Message thread] Only `TrackLanesView` calls these on the source header during a drag.
    void setSourceForbiddenForHeaderDrag() noexcept;
    void restoreSourceCursorAfterHeaderDrag() noexcept;

private:
    /// [Message / paint] Wider than the visible circle for click targets; `mouseDown` uses this.
    [[nodiscard]] juce::Rectangle<int> getArmButtonBounds() const noexcept;
    /// Small square (≈18px) for drawing a circular R; vertically centered in the header.
    [[nodiscard]] juce::Rectangle<int> getArmVisualCircleBounds() const noexcept;

    Session& session_;
    RecorderService& recorder_;
    const TrackId trackId_;
    std::function<void()> onActiveChanged_;
    std::function<void()> onArmStateChanged_;
    TrackHeaderDragHost dragHost_;
    bool headerDragInProgress_ = false;
    /// Set when the current gesture started on the record-arm control so we do not start header-drag.
    bool mousedownBeganOnArm_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
