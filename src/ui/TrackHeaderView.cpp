// =============================================================================
// TrackHeaderView.cpp  —  `mouseDown` activates track; drag after threshold → `TrackLanesView` host
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "ui/ForbiddenCursor.h"
#include "domain/Session.h"
#include "engine/RecorderService.h"
#include "transport/Transport.h"

#include <juce_core/juce_core.h>

namespace
{
    constexpr float kHeaderDragThresholdPx = 3.0f;
    constexpr int kTrackControlCellWidth = 22;
    constexpr int kRightControlsWidth = kTrackControlCellWidth * 3;
    constexpr int kArmVisualDiameter = 18;

    [[nodiscard]] juce::Rectangle<int> circleGlyphBoundsInCell(const juce::Rectangle<int>& cell) noexcept
    {
        const int d = juce::jmin(
            kArmVisualDiameter,
            juce::jmax(4, cell.getWidth() - 2),
            juce::jmax(4, cell.getHeight() - 4));
        return {
            cell.getCentreX() - d / 2,
            cell.getCentreY() - d / 2,
            d,
            d};
    }

    void fillCircleLetterButton(juce::Graphics& g,
                                const juce::Rectangle<int>& circ,
                                const juce::String& letter,
                                const juce::Colour fill,
                                const juce::Colour letterColour)
    {
        juce::Graphics::ScopedSaveState guard(g);
        g.reduceClipRegion(circ);
        g.setColour(fill);
        g.fillEllipse(circ.toFloat());
        g.setColour(letterColour);
        g.setFont(11.0f);
        g.drawFittedText(letter, circ, juce::Justification::centred, 1);
    }

    /** Standby/power glyph: normalized coords mapped into `icon`; ring + short stem. */
    static void drawPowerGlyphInSquare(juce::Graphics& g,
                                       juce::Rectangle<float> icon,
                                       const juce::Colour glyphColour)
    {
        float side = juce::jmin(icon.getWidth(), icon.getHeight());
        if (side <= 4.0f)
            return;

        icon = juce::Rectangle<float>(icon.getCentreX() - side * 0.5f,
                                      icon.getCentreY() - side * 0.5f,
                                      side,
                                      side);

        const auto x = [&](float nx) { return icon.getX() + nx * side; };
        const auto y = [&](float ny) { return icon.getY() + ny * side; };

        const float stroke = juce::jlimit(1.6f, 2.4f, side * 0.14f);

        g.setColour(glyphColour);

        constexpr float ringCx = 0.5f;
        constexpr float ringCy = 0.54f;
        constexpr float ringR = 0.33f;
        constexpr float arcFromDeg = 35.0f;
        constexpr float arcToDeg = 325.0f;

        juce::Path ring;
        ring.addCentredArc(x(ringCx),
                           y(ringCy),
                           side * ringR,
                           side * ringR,
                           0.0f,
                           juce::degreesToRadians(arcFromDeg),
                           juce::degreesToRadians(arcToDeg),
                           true);

        g.strokePath(
            ring,
            juce::PathStrokeType(stroke,
                                  juce::PathStrokeType::mitered,
                                  juce::PathStrokeType::butt));

        juce::Path stem;
        stem.startNewSubPath(x(0.5f), y(0.12f));
        stem.lineTo(x(0.5f), y(0.38f));

        g.strokePath(
            stem,
            juce::PathStrokeType(stroke,
                                  juce::PathStrokeType::mitered,
                                  juce::PathStrokeType::butt));
    }

    /// Power lane toggle (**`M`** / **`R`** footprint): grey = **`trackOff`** (**skipped**),
    /// green = **processed**; light IEC-style standby symbol inside the circle.
    void paintPowerCircularButton(juce::Graphics& g,
                                  const juce::Rectangle<int>& powerVis,
                                  const bool trackOff)
    {
        juce::Graphics::ScopedSaveState clipGuard(g);
        g.reduceClipRegion(powerVis);

        g.setColour(trackOff ? juce::Colour(0xff555555) : juce::Colour(0xff2d9d53));
        g.fillEllipse(powerVis.toFloat());

        drawPowerGlyphInSquare(g,
                               powerVis.toFloat().reduced(2.0f),
                               juce::Colour(0xfff3f4f5));
    }
} // namespace

juce::Rectangle<int> TrackHeaderView::getRightControlsStripBounds() const noexcept
{
    return getLocalBounds().removeFromRight(kRightControlsWidth).reduced(2, 4);
}

juce::Rectangle<int> TrackHeaderView::getArmButtonBounds() const noexcept
{
    juce::Rectangle<int> s = getRightControlsStripBounds();
    return s.removeFromRight(kTrackControlCellWidth);
}

juce::Rectangle<int> TrackHeaderView::getMuteButtonBounds() const noexcept
{
    juce::Rectangle<int> s = getRightControlsStripBounds();
    s.removeFromRight(kTrackControlCellWidth);
    return s.removeFromRight(kTrackControlCellWidth);
}

juce::Rectangle<int> TrackHeaderView::getPowerButtonBounds() const noexcept
{
    juce::Rectangle<int> s = getRightControlsStripBounds();
    s.removeFromRight(kTrackControlCellWidth * 2);
    return s;
}

juce::Rectangle<int> TrackHeaderView::getPowerVisualCircleBounds() const noexcept
{
    return circleGlyphBoundsInCell(getPowerButtonBounds());
}

juce::Rectangle<int> TrackHeaderView::getMuteVisualCircleBounds() const noexcept
{
    return circleGlyphBoundsInCell(getMuteButtonBounds());
}

juce::Rectangle<int> TrackHeaderView::getArmVisualCircleBounds() const noexcept
{
    return circleGlyphBoundsInCell(getArmButtonBounds());
}

TrackHeaderView::TrackHeaderView(
    Session& session,
    RecorderService& recorder,
    Transport& transport,
    const TrackId trackId,
    std::function<void()> onActiveChanged,
    std::function<void()> onArmStateChanged,
    TrackHeaderDragHost dragHost) noexcept
    : session_(session)
    , recorder_(recorder)
    , transport_(transport)
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
    bool trackOff = false;
    bool trackMuted = false;
    juce::String label;
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        const int idx = snap->findTrackIndexById(trackId_);
        if (idx >= 0)
        {
            const Track& tr = snap->getTrack(idx);
            label = tr.getName();
            trackOff = tr.isTrackOff();
            trackMuted = tr.isMuted();
        }
    }

    const auto b = getLocalBounds();
    g.setColour(active ? juce::Colour(0xff2a4a5a) : juce::Colour(0xff333333));
    g.fillRect(b);
    if (active)
    {
        g.setColour(juce::Colours::deepskyblue);
        g.fillRect(b.getX(), b.getY(), 4, b.getHeight());
    }

    g.setColour(juce::Colours::whitesmoke);
    g.setFont(14.0f);
    g.drawFittedText(
        label,
        b.withTrimmedRight(kRightControlsWidth).reduced(8, 0).withTrimmedLeft(active ? 6 : 4),
        juce::Justification::centredLeft,
        1);

    const juce::Rectangle<int> muteCirc = getMuteVisualCircleBounds();
    fillCircleLetterButton(g,
                            muteCirc,
                            "M",
                            trackMuted ? juce::Colour(0xffbbaa33) : juce::Colour(0xff555555),
                            trackMuted ? juce::Colour(0xff000000) : juce::Colour(0xffcccccc));

    const juce::Rectangle<int> armCirc = getArmVisualCircleBounds();
    fillCircleLetterButton(g,
                            armCirc,
                            "R",
                            armed ? juce::Colour(0xffcc2222) : juce::Colour(0xff555555),
                            armed ? juce::Colour(0xffffffff) : juce::Colour(0xffcccccc));

    const juce::Rectangle<int> powerVis = getPowerVisualCircleBounds();
    if (powerVis.getWidth() > 4 && powerVis.getHeight() > 4)
    {
        paintPowerCircularButton(g, powerVis, trackOff);
    }
}

void TrackHeaderView::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    const juce::Rectangle<int> armR = getArmButtonBounds();
    if (armR.contains(p))
    {
        dragBlocker_ = DragBlocker::Arm;
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
        session_.setActiveTrack(trackId_);
        if (onActiveChanged_ != nullptr)
        {
            onActiveChanged_();
        }
        return;
    }

    const juce::Rectangle<int> muteR = getMuteButtonBounds();
    if (muteR.contains(p))
    {
        dragBlocker_ = DragBlocker::Mute;
        bool nowMuted = true;
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = snap->findTrackIndexById(trackId_);
            if (idx >= 0)
            {
                nowMuted = !snap->getTrack(idx).isMuted();
            }
        }
        session_.setTrackMuted(trackId_, nowMuted);
        if (onArmStateChanged_ != nullptr)
        {
            onArmStateChanged_();
        }
        repaint();
        session_.setActiveTrack(trackId_);
        if (onActiveChanged_ != nullptr)
        {
            onActiveChanged_();
        }
        return;
    }

    const juce::Rectangle<int> powerR = getPowerButtonBounds();
    if (powerR.contains(p))
    {
        if (transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing || recorder_.isRecording())
        {
            return;
        }
        dragBlocker_ = DragBlocker::Power;
        bool nowOff = true;
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = snap->findTrackIndexById(trackId_);
            if (idx >= 0)
            {
                nowOff = !snap->getTrack(idx).isTrackOff();
            }
        }
        session_.setTrackOff(trackId_, nowOff);
        if (onArmStateChanged_ != nullptr)
        {
            onArmStateChanged_();
        }
        repaint();
        session_.setActiveTrack(trackId_);
        if (onActiveChanged_ != nullptr)
        {
            onActiveChanged_();
        }
        return;
    }

    dragBlocker_ = DragBlocker::None;
    headerDragInProgress_ = false;
    session_.setActiveTrack(trackId_);
    if (onActiveChanged_ != nullptr)
    {
        onActiveChanged_();
    }
}

void TrackHeaderView::mouseDrag(const juce::MouseEvent& e)
{
    if (dragBlocker_ != DragBlocker::None)
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
        dragBlocker_ = DragBlocker::None;
        return;
    }
    dragBlocker_ = DragBlocker::None;
    juce::ignoreUnused(e);
}

void TrackHeaderView::setSourceForbiddenForHeaderDrag() noexcept
{
    setMouseCursor(getForbiddenNoDropMouseCursor());
}

void TrackHeaderView::restoreSourceCursorAfterHeaderDrag() noexcept
{
    setMouseCursor(juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
}
