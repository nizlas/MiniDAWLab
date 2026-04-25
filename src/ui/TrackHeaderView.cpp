// =============================================================================
// TrackHeaderView.cpp
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "domain/Session.h"

#include <juce_core/juce_core.h>

namespace
{
    // Ignore obvious drags so lane drag logic is unchanged.
    constexpr float kMaxClickMovePx = 8.0f;
}

TrackHeaderView::TrackHeaderView(
    Session& session, const TrackId trackId, std::function<void()> onActiveChanged) noexcept
    : session_(session)
    , trackId_(trackId)
    , onActiveChanged_(std::move(onActiveChanged))
{
    jassert(trackId_ != kInvalidTrackId);
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

void TrackHeaderView::mouseUp(const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > kMaxClickMovePx)
    {
        return;
    }
    session_.setActiveTrack(trackId_);
    if (onActiveChanged_ != nullptr)
    {
        onActiveChanged_();
    }
}
