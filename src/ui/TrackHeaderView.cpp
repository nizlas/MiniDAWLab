// =============================================================================
// TrackHeaderView.cpp  —  `mouseDown` activates track; drag after threshold → `TrackLanesView` host
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "ui/ForbiddenCursor.h"
#include "domain/Session.h"

#include <juce_core/juce_core.h>

namespace
{
    // Same order-of-magnitude as `ClipWaveformView` drag start so click vs drag stays consistent.
    constexpr float kHeaderDragThresholdPx = 3.0f;
} // namespace

TrackHeaderView::TrackHeaderView(
    Session& session, const TrackId trackId, std::function<void()> onActiveChanged, TrackHeaderDragHost dragHost) noexcept
    : session_(session)
    , trackId_(trackId)
    , onActiveChanged_(std::move(onActiveChanged))
    , dragHost_(std::move(dragHost))
{
    jassert(trackId_ != kInvalidTrackId);
    jassert(dragHost_.onHeaderDragBegan != nullptr);
    jassert(dragHost_.onHeaderDragMoved != nullptr);
    jassert(dragHost_.onHeaderDragEnded != nullptr);
}

void TrackHeaderView::paint(juce::Graphics& g)
{
    const bool active = (session_.getActiveTrackId() == trackId_);
    const auto b = getLocalBounds();
    g.setColour(active ? juce::Colour(0xff2a4a5a) : juce::Colour(0xff333333));
    g.fillRect(b);
    if (active)
    {
        g.setColour(juce::Colours::deepskyblue);
        g.fillRect(b.getX(), b.getY(), 4, b.getHeight());
    }

    juce::String label;
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        const int idx = snap->findTrackIndexById(trackId_);
        if (idx >= 0)
        {
            label = snap->getTrack(idx).getName();
        }
    }
    g.setColour(juce::Colours::whitesmoke);
    g.setFont(14.0f);
    g.drawFittedText(
        label, b.reduced(8, 0).withTrimmedLeft(active ? 6 : 4), juce::Justification::centredLeft, 1);
}

void TrackHeaderView::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    headerDragInProgress_ = false;
    // DAW-like: the pressed header’s track becomes active immediately (before any drag threshold).
    session_.setActiveTrack(trackId_);
    if (onActiveChanged_ != nullptr)
    {
        onActiveChanged_();
    }
}

void TrackHeaderView::mouseDrag(const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > kHeaderDragThresholdPx)
    {
        if (!headerDragInProgress_)
        {
            headerDragInProgress_ = true;
            dragHost_.onHeaderDragBegan(trackId_, this);
        }
        const juce::Point<int> screen(e.getScreenX(), e.getScreenY());
        dragHost_.onHeaderDragMoved(trackId_, screen);
    }
}

void TrackHeaderView::mouseUp(const juce::MouseEvent& e)
{
    if (headerDragInProgress_)
    {
        dragHost_.onHeaderDragEnded(trackId_);
        headerDragInProgress_ = false;
        return;
    }
    juce::ignoreUnused(e);
    // Active track was set on `mouseDown`; no separate click-up activation.
}

void TrackHeaderView::setSourceForbiddenForHeaderDrag() noexcept
{
    setMouseCursor(getForbiddenNoDropMouseCursor());
}

void TrackHeaderView::restoreSourceCursorAfterHeaderDrag() noexcept
{
    setMouseCursor(juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
}
