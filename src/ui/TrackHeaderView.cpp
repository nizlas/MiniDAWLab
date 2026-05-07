// =============================================================================
// TrackHeaderView.cpp — model/callback-driven header; optional drag host
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "ui/ForbiddenCursor.h"

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

    static void drawPowerGlyphInSquare(juce::Graphics& g,
                                       juce::Rectangle<float> icon,
                                       const juce::Colour glyphColour)
    {
        float side = juce::jmin(icon.getWidth(), icon.getHeight());
        if (side <= 4.0f)
        {
            return;
        }

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

    void paintPowerCircularButtonDisabled(juce::Graphics& g, const juce::Rectangle<int>& powerVis)
    {
        juce::Graphics::ScopedSaveState clipGuard(g);
        g.reduceClipRegion(powerVis);
        g.setColour(juce::Colour(0xff444444));
        g.fillEllipse(powerVis.toFloat());
        drawPowerGlyphInSquare(g,
                               powerVis.toFloat().reduced(2.0f),
                               juce::Colour(0xff888888));
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

TrackHeaderView::TrackHeaderView(TrackHeaderModelProvider modelProvider,
                                 TrackHeaderCallbacks callbacks,
                                 TrackId dragTrackId,
                                 std::optional<TrackHeaderDragHost> dragHost) noexcept
    : modelProvider_(std::move(modelProvider))
    , callbacks_(std::move(callbacks))
    , dragTrackId_(dragTrackId)
    , dragHost_(std::move(dragHost))
{
    if (dragHost_.has_value())
    {
        jassert(dragTrackId_ != kInvalidTrackId);
        jassert(dragHost_->onHeaderDragBegan != nullptr);
        jassert(dragHost_->onHeaderDragMoved != nullptr);
        jassert(dragHost_->onHeaderDragEnded != nullptr);
    }
}

void TrackHeaderView::paint(juce::Graphics& g)
{
    const auto m = modelProvider_();
    const bool active = m.active;

    const auto b = getLocalBounds();
    g.setColour(active ? juce::Colour(0xff2a4a5a) : juce::Colour(0xff333333));
    g.fillRect(b);
    if (active)
    {
        g.setColour(juce::Colours::deepskyblue);
        g.fillRect(b.getX(), b.getY(), 4, b.getHeight());
    }

    auto nameArea = b.withTrimmedRight(kRightControlsWidth).reduced(8, 0).withTrimmedLeft(active ? 6 : 4);
    g.setColour(juce::Colours::whitesmoke);
    g.setFont(14.0f);
    if (m.subtitle.isEmpty())
    {
        g.drawFittedText(m.name, nameArea, juce::Justification::centredLeft, 1);
    }
    else
    {
        auto r = nameArea;
        g.drawText(m.name, r.removeFromTop(16), juce::Justification::centredLeft, true);
        g.setColour(juce::Colour(0xffcccccc));
        g.setFont(11.0f);
        g.drawFittedText(m.subtitle, r, juce::Justification::topLeft, 2);
    }

    const juce::Rectangle<int> muteCirc = getMuteVisualCircleBounds();
    if (m.muteInteractable)
    {
        fillCircleLetterButton(g,
                               muteCirc,
                               "M",
                               m.muted ? juce::Colour(0xffbbaa33) : juce::Colour(0xff555555),
                               m.muted ? juce::Colour(0xff000000) : juce::Colour(0xffcccccc));
    }
    else
    {
        fillCircleLetterButton(
            g, muteCirc, "M", juce::Colour(0xff444444), juce::Colour(0xff888888));
    }

    const juce::Rectangle<int> armCirc = getArmVisualCircleBounds();
    if (m.armInteractable)
    {
        fillCircleLetterButton(g,
                               armCirc,
                               "R",
                               m.armed ? juce::Colour(0xffcc2222) : juce::Colour(0xff555555),
                               m.armed ? juce::Colour(0xffffffff) : juce::Colour(0xffcccccc));
    }
    else
    {
        fillCircleLetterButton(
            g, armCirc, "R", juce::Colour(0xff444444), juce::Colour(0xff888888));
    }

    const juce::Rectangle<int> powerVis = getPowerVisualCircleBounds();
    if (powerVis.getWidth() > 4 && powerVis.getHeight() > 4)
    {
        if (m.powerInteractable)
        {
            paintPowerCircularButton(g, powerVis, m.off);
        }
        else
        {
            paintPowerCircularButtonDisabled(g, powerVis);
        }
    }
}

void TrackHeaderView::mouseDown(const juce::MouseEvent& e)
{
    const auto m = modelProvider_();
    const auto p = e.getPosition();

    if (e.mods.isPopupMenu())
    {
        if (callbacks_.onShowContextMenu != nullptr)
        {
            dragBlocker_ = DragBlocker::None;
            headerDragInProgress_ = false;
            callbacks_.onShowContextMenu(*this, e);
        }
        return;
    }

    const juce::Rectangle<int> armR = getArmButtonBounds();
    if (armR.contains(p))
    {
        if (!m.armInteractable || callbacks_.onToggleArm == nullptr)
        {
            return;
        }
        dragBlocker_ = DragBlocker::Arm;
        callbacks_.onToggleArm();
        return;
    }

    const juce::Rectangle<int> muteR = getMuteButtonBounds();
    if (muteR.contains(p))
    {
        if (!m.muteInteractable || callbacks_.onToggleMute == nullptr)
        {
            return;
        }
        dragBlocker_ = DragBlocker::Mute;
        callbacks_.onToggleMute();
        return;
    }

    const juce::Rectangle<int> powerR = getPowerButtonBounds();
    if (powerR.contains(p))
    {
        if (!m.powerInteractable || callbacks_.onTogglePower == nullptr)
        {
            return;
        }
        if (callbacks_.onTogglePower())
        {
            dragBlocker_ = DragBlocker::Power;
        }
        return;
    }

    dragBlocker_ = DragBlocker::None;
    headerDragInProgress_ = false;
    if (callbacks_.onActivateName != nullptr)
    {
        callbacks_.onActivateName();
    }
}

void TrackHeaderView::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragHost_.has_value())
    {
        return;
    }
    if (!e.mods.isLeftButtonDown())
    {
        return;
    }
    if (dragBlocker_ != DragBlocker::None)
    {
        return;
    }
    if (e.getDistanceFromDragStart() > kHeaderDragThresholdPx)
    {
        if (!headerDragInProgress_)
        {
            headerDragInProgress_ = true;
            dragHost_->onHeaderDragBegan(dragTrackId_, this);
        }
        const juce::Point<int> screen(e.getScreenX(), e.getScreenY());
        dragHost_->onHeaderDragMoved(dragTrackId_, screen);
    }
}

void TrackHeaderView::mouseUp(const juce::MouseEvent& e)
{
    if (headerDragInProgress_ && dragHost_.has_value())
    {
        dragHost_->onHeaderDragEnded(dragTrackId_);
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
