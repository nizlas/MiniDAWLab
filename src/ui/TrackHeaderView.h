#pragma once

// =============================================================================
// TrackHeaderView  —  minimal label + active highlight; header-drag for track reorder (message)
// =============================================================================
// `mouseDown` calls `Session::setActiveTrack` (no snapshot republish). Drag past a threshold is
// coordinated by `TrackLanesView` (insert line, `Session::moveTrack`). The
// event lane to the right is not a header-drag target. Invalid drop uses
// `getForbiddenNoDropMouseCursor` (`ForbiddenCursor.h`).
// =============================================================================

#include "domain/Track.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

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
        TrackId trackId,
        std::function<void()> onActiveChanged,
        TrackHeaderDragHost dragHost) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // [Message thread] Only `TrackLanesView` calls these on the source header during a drag.
    void setSourceForbiddenForHeaderDrag() noexcept;
    void restoreSourceCursorAfterHeaderDrag() noexcept;

private:
    Session& session_;
    const TrackId trackId_;
    std::function<void()> onActiveChanged_;
    TrackHeaderDragHost dragHost_;
    bool headerDragInProgress_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
