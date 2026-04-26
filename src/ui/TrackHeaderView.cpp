// =============================================================================
// TrackHeaderView.cpp  —  `mouseDown` activates track; drag after threshold → `TrackLanesView` host
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "ui/ForbiddenCursor.h"
#include "domain/Session.h"
#include "engine/RecorderService.h"

#include <juce_core/juce_core.h>

namespace
{
    // Same order-of-magnitude as `ClipWaveformView` drag start so click vs drag stays consistent.
    constexpr float kHeaderDragThresholdPx = 3.0f;
    /// Narrow strip for record-arm (hit test + label trim).
    constexpr int kArmButtonWidth = 22;
    /// Diameter of the visible R control (drawn in a square → circle).
    constexpr int kArmVisualDiameter = 18;
} // namespace

juce::Rectangle<int> TrackHeaderView::getArmButtonBounds() const noexcept
{
    return getLocalBounds().removeFromRight(kArmButtonWidth).reduced(2, 4);
}

juce::Rectangle<int> TrackHeaderView::getArmVisualCircleBounds() const noexcept
{
    juce::Rectangle<int> b = getLocalBounds();
    const juce::Rectangle<int> strip = b.removeFromRight(kArmButtonWidth);
    const int d = juce::jmin(
        kArmVisualDiameter,
        juce::jmax(4, strip.getWidth() - 2),
        juce::jmax(4, b.getHeight() - 4));
    const int x = strip.getCentreX() - d / 2;
    const int y = b.getCentreY() - d / 2;
    return { x, y, d, d };
}

TrackHeaderView::TrackHeaderView(
    Session& session,
    RecorderService& recorder,
    const TrackId trackId,
    std::function<void()> onActiveChanged,
    std::function<void()> onArmStateChanged,
    TrackHeaderDragHost dragHost) noexcept
    : session_(session)
    , recorder_(recorder)
    , trackId_(trackId)
    , onActiveChanged_(std::move(onActiveChanged))
    , onArmStateChanged_(std::move(onArmStateChanged))
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
    const bool armed = (recorder_.getArmedTrackId() == trackId_);
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
        label,
        b.withTrimmedRight(kArmButtonWidth).reduced(8, 0).withTrimmedLeft(active ? 6 : 4),
        juce::Justification::centredLeft,
        1);

    const juce::Rectangle<int> visR = getArmVisualCircleBounds();
    g.setColour(armed ? juce::Colour(0xffcc2222) : juce::Colour(0xff555555));
    g.fillEllipse(visR.toFloat());
    g.setColour(armed ? juce::Colours::white : juce::Colour(0xffcccccc));
    g.setFont(11.0f);
    g.drawFittedText("R", visR, juce::Justification::centred, 1);
}

void TrackHeaderView::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    if (getArmButtonBounds().contains(p))
    {
        mousedownBeganOnArm_ = true;
        if (recorder_.getArmedTrackId() == trackId_)
        {
            recorder_.disarm();
        }
        else
        {
            recorder_.armForRecording(trackId_);
        }
        if (onArmStateChanged_ != nullptr)
        {
            onArmStateChanged_();
        }
        // Same as the name strip: the pressed track becomes active.
        session_.setActiveTrack(trackId_);
        if (onActiveChanged_ != nullptr)
        {
            onActiveChanged_();
        }
        return;
    }

    mousedownBeganOnArm_ = false;
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
    if (mousedownBeganOnArm_)
    {
        return;
    }
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
        mousedownBeganOnArm_ = false;
        return;
    }
    mousedownBeganOnArm_ = false;
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
