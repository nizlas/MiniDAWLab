#include "ui/InspectorPanControl.h"

#include "domain/TrackStereoPan.h"

#include <cmath>
#include <optional>

namespace
{
    constexpr float kFieldCornerRadius = 3.5f;

    constexpr float kContentPad = 2.0f;

    constexpr float kTextRowH = 15.0f;

    constexpr float kStickTravelMargin = 3.0f;

    constexpr float kMarkerStripPadY = 2.0f;

    constexpr float kBlueVerticalBreathingRoom = 0.75f;

    constexpr float kMarkerHitExpandX = 7.0f;

    constexpr float kMarkerHitExpandY = 4.0f;

    [[nodiscard]] float blueIndicatorHeight(const float markerStripHeight) noexcept
    {
        const float inner = juce::jmax(0.f, markerStripHeight - 2.f * kBlueVerticalBreathingRoom);
        const float legacyThin = juce::jmax(0.f, markerStripHeight - 6.f);
        const float prevGen = juce::jmin(inner, legacyThin * 2.f);
        return juce::jmin(inner, prevGen * 2.f);
    }

    [[nodiscard]] juce::Colour fieldBackgroundColour() noexcept
    {
        return juce::Colour(0xff232328);
    }

    [[nodiscard]] juce::Colour fieldBorderColour(const bool hovered) noexcept
    {
        const float a = hovered ? 0.72f : 0.48f;
        return juce::Colour(0xff6a6f78).withAlpha(a);
    }

    /// Hue matches selected-track stripe (`TrackHeaderView`: `juce::Colours::deepskyblue`);
    /// darker + alpha so a solid fill does not read as neon vs the thin header accent.
    [[nodiscard]] juce::Colour panAccentFillColour() noexcept
    {
        return juce::Colours::deepskyblue.darker(0.2f).withAlpha(0.66f);
    }

    [[nodiscard]] juce::Colour centerLineColour() noexcept
    {
        return juce::Colours::white.withAlpha(0.48f);
    }

    [[nodiscard]] juce::Colour stickColour() noexcept
    {
        return juce::Colour(0xfff8f8f8).withAlpha(0.95f);
    }

    [[nodiscard]] juce::Colour labelColour() noexcept
    {
        return juce::Colour(0xffe8eaef).withAlpha(0.92f);
    }

    /// Slim vertical stick (~2–2.5 px at 1×); hit zone remains expanded separately.
    [[nodiscard]] float stickWidthForComponent(const juce::Component& c) noexcept
    {
        const float scale = (float)c.getDesktopScaleFactor();
        return juce::jlimit(2.f, 3.f, std::round(2.25f * scale));
    }

    void hideCursorOnMouseInputSources()
    {
        auto mainMs = juce::Desktop::getInstance().getMainMouseSource();
        if (mainMs.isMouse())
        {
            mainMs.hideCursor();
        }

        auto& desk = juce::Desktop::getInstance();
        for (int i = 0; i < desk.getNumMouseSources(); ++i)
        {
            if (auto* ms = desk.getMouseSource(i))
            {
                if (ms->isMouse())
                {
                    ms->hideCursor();
                }
            }
        }
    }

    void revealCursorOnMouseInputSources()
    {
        auto mainMs = juce::Desktop::getInstance().getMainMouseSource();
        if (mainMs.isMouse())
        {
            mainMs.revealCursor();
        }

        auto& desk = juce::Desktop::getInstance();
        for (int i = 0; i < desk.getNumMouseSources(); ++i)
        {
            if (auto* ms = desk.getMouseSource(i))
            {
                if (ms->isMouse())
                {
                    ms->revealCursor();
                }
            }
        }
    }

    [[nodiscard]] bool isUnsignedDigitSuffix(const juce::String& rest) noexcept
    {
        if (rest.isEmpty())
        {
            return false;
        }
        for (int i = 0; i < rest.length(); ++i)
        {
            if (!juce::CharacterFunctions::isDigit(rest[i]))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<float> parsePanDisplayReadout(juce::String s)
    {
        s = s.trim();
        if (s.isEmpty())
        {
            return std::nullopt;
        }

        const juce::String u = s.toUpperCase();

        if (u == "C")
        {
            return 0.f;
        }
        if (u == "L")
        {
            return -1.f;
        }
        if (u == "R")
        {
            return 1.f;
        }

        if (u.startsWithChar('L'))
        {
            const juce::String rest = u.substring(1);
            if (!isUnsignedDigitSuffix(rest))
            {
                return std::nullopt;
            }
            const int n = rest.getIntValue();
            if (n <= 0)
            {
                return std::nullopt;
            }
            if (n >= 100)
            {
                return -1.f;
            }
            return -static_cast<float>(n) / 100.f;
        }

        if (u.startsWithChar('R'))
        {
            const juce::String rest = u.substring(1);
            if (!isUnsignedDigitSuffix(rest))
            {
                return std::nullopt;
            }
            const int n = rest.getIntValue();
            if (n <= 0)
            {
                return std::nullopt;
            }
            if (n >= 100)
            {
                return 1.f;
            }
            return static_cast<float>(n) / 100.f;
        }

        return std::nullopt;
    }
} // namespace

float InspectorPanControl::Layout::markerXForPan(const float normalizedPan) const noexcept
{
    const float p = juce::jlimit(-1.f, 1.f, normalizedPan);
    if (!(travelRight > travelLeft))
    {
        return centerX;
    }
    return juce::jlimit(travelLeft, travelRight, juce::jmap(p, -1.f, 1.f, travelLeft, travelRight));
}

InspectorPanControl::InspectorPanControl()
{
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);
    setRepaintsOnMouseActivity(true);
}

InspectorPanControl::~InspectorPanControl()
{
    if (panDragActive_)
    {
        panDragActive_ = false;
        revealCursorOnMouseInputSources();
    }
}

bool InspectorPanControl::hitTest(int x, int y)
{
    return getPanBoxBounds().contains(x, y);
}

juce::Rectangle<int> InspectorPanControl::getPanBoxBounds() const noexcept
{
    return getLocalBounds();
}

InspectorPanControl::Layout InspectorPanControl::computeLayout() const
{
    Layout L;
    L.field = getLocalBounds().toFloat();
    auto inner = L.field.reduced(kContentPad);
    L.textArea = inner.removeFromBottom(kTextRowH);
    L.markerArea = inner;
    L.centerX = L.markerArea.getCentreX();

    L.stickWidth = stickWidthForComponent(*this);
    const float inset = L.stickWidth * 0.5f + kStickTravelMargin;
    L.travelLeft = L.markerArea.getX() + inset;
    L.travelRight = L.markerArea.getRight() - inset;
    if (L.travelRight < L.travelLeft)
    {
        const float mid = L.markerArea.getCentreX();
        L.travelLeft = mid;
        L.travelRight = mid;
    }
    return L;
}

juce::Rectangle<int> InspectorPanControl::getTextArea() const
{
    return computeLayout().textArea.toNearestIntEdges();
}

juce::Rectangle<int> InspectorPanControl::getMarkerHitRect() const
{
    const Layout lay = computeLayout();
    const float markerX = lay.markerXForPan(pan_);
    const float stickTop = lay.markerArea.getY() + kMarkerStripPadY;
    const float stickBottom = lay.markerArea.getBottom() - kMarkerStripPadY;
    if (stickBottom <= stickTop)
    {
        return {};
    }

    const float half = lay.stickWidth * 0.5f;
    juce::Rectangle<float> stick(markerX - half, stickTop, lay.stickWidth, stickBottom - stickTop);
    auto hit = stick.expanded(kMarkerHitExpandX, kMarkerHitExpandY);
    hit = hit.getIntersection(lay.markerArea);
    return hit.toNearestIntEdges();
}

bool InspectorPanControl::isInMarkerHitZone(const juce::Point<int> p) const noexcept
{
    return getMarkerHitRect().contains(p);
}

void InspectorPanControl::setPan(const float pan, const juce::NotificationType notify)
{
    // Session refresh calls `setPan` frequently; never tear down the inline pan editor here.
    if (panEditor_ != nullptr)
    {
        return;
    }

    const float next = sanitizeTrackStereoPan(pan);
    if (next == pan_)
    {
        return;
    }
    pan_ = next;
    repaint();
    if (notify != juce::dontSendNotification && onPanChanged != nullptr)
    {
        onPanChanged(pan_);
    }
}

void InspectorPanControl::commitPan(const float pan, const juce::NotificationType notify)
{
    const float next = sanitizeTrackStereoPan(pan);
    if (next == pan_)
    {
        if (notify != juce::dontSendNotification && onPanChanged != nullptr)
        {
            onPanChanged(pan_);
        }
        return;
    }
    pan_ = next;
    repaint();
    if (notify != juce::dontSendNotification && onPanChanged != nullptr)
    {
        onPanChanged(pan_);
    }
}

void InspectorPanControl::applyPanFromLocalX(const float localX, const juce::NotificationType notify)
{
    const Layout lay = computeLayout();
    if (!(lay.travelRight > lay.travelLeft))
    {
        return;
    }
    const float t = juce::jlimit(0.f, 1.f, (localX - lay.travelLeft) / (lay.travelRight - lay.travelLeft));
    commitPan(t * 2.f - 1.f, notify);
}

void InspectorPanControl::resized()
{
    if (panEditor_ != nullptr)
    {
        panEditor_->setBounds(getTextArea());
    }
}

void InspectorPanControl::paint(juce::Graphics& g)
{
    const Layout lay = computeLayout();
    const float markerX = lay.markerXForPan(pan_);

    g.fillAll(fieldBackgroundColour());
    g.setColour(fieldBackgroundColour());
    g.fillRoundedRectangle(lay.field, kFieldCornerRadius);

    g.setColour(fieldBorderColour(hoveredComponent_));
    g.drawRoundedRectangle(lay.field.reduced(0.5f), kFieldCornerRadius, 1.0f);

    {
        const float lineTop = lay.markerArea.getY() + kMarkerStripPadY;
        const float lineBottom = lay.markerArea.getBottom() - kMarkerStripPadY;
        g.setColour(centerLineColour());
        g.drawLine(lay.centerX, lineTop, lay.centerX, lineBottom, pan_ == 0.0f ? 1.35f : 1.05f);
    }

    const float markerStripH = lay.markerArea.getHeight();
    const float fillH = blueIndicatorHeight(markerStripH);
    const float cy = lay.markerArea.getCentreY();
    float fillTop = cy - fillH * 0.5f;
    float fillBottom = cy + fillH * 0.5f;
    fillTop = juce::jmax(fillTop, lay.markerArea.getY() + kBlueVerticalBreathingRoom);
    fillBottom = juce::jmin(fillBottom, lay.markerArea.getBottom() - kBlueVerticalBreathingRoom);
    if (fillBottom > fillTop && std::fabs(pan_) > 1.0e-5f && lay.travelRight > lay.travelLeft)
    {
        const float x0 = juce::jmin(lay.centerX, markerX);
        float x1 = juce::jmax(lay.centerX, markerX);
        if (x1 - x0 < 1.0f)
        {
            x1 = x0 + 1.0f;
        }
        auto fill = juce::Rectangle<float>(x0, fillTop, x1 - x0, fillBottom - fillTop);
        fill = fill.getIntersection(lay.markerArea.reduced(2.0f, 0.f));
        if (fill.getWidth() > 0.f && fill.getHeight() > 0.f)
        {
            g.setColour(panAccentFillColour());
            // Integer pixels + axis-aligned rect — sharp 90° corners (no rounded blue shape).
            g.fillRect(fill.toNearestIntEdges());
        }
    }

    {
        const float stickTop = lay.markerArea.getY() + kMarkerStripPadY;
        const float stickBottom = lay.markerArea.getBottom() - kMarkerStripPadY;
        if (stickBottom > stickTop)
        {
            const float half = lay.stickWidth * 0.5f;
            juce::Rectangle<float> stick(markerX - half, stickTop, lay.stickWidth, stickBottom - stickTop);
            stick = stick.getIntersection(lay.markerArea);
            if (stick.getWidth() > 0.f && stick.getHeight() > 0.f)
            {
                g.setColour(stickColour());
                g.fillRect(stick);
            }
        }
    }

    if (panEditor_ == nullptr)
    {
        const juce::String text = formatPanForDisplay(pan_);
        g.setColour(labelColour());
        g.setFont(juce::Font(juce::FontOptions(11.5f)).boldened());
        g.drawText(text, lay.textArea.toNearestIntEdges(), juce::Justification::centred);
    }
}

void InspectorPanControl::mouseDown(const juce::MouseEvent& e)
{
    if (panEditor_ != nullptr)
    {
        return;
    }

    grabKeyboardFocus();

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        commitPan(0.0f, juce::sendNotificationSync);
        return;
    }

    if (e.getNumberOfClicks() > 1)
    {
        return;
    }

    if (!isInMarkerHitZone(e.position.toInt()))
    {
        return;
    }

    panDragActive_ = true;
    hideCursorOnMouseInputSources();
    applyPanFromLocalX((float)e.position.x, juce::sendNotificationSync);
}

void InspectorPanControl::mouseDrag(const juce::MouseEvent& e)
{
    if (panEditor_ != nullptr)
    {
        return;
    }
    if (!panDragActive_)
    {
        return;
    }
    hideCursorOnMouseInputSources();
    applyPanFromLocalX((float)e.position.x, juce::sendNotificationSync);
}

void InspectorPanControl::mouseUp(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    if (!panDragActive_)
    {
        return;
    }
    panDragActive_ = false;
    revealCursorOnMouseInputSources();
}

void InspectorPanControl::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (panEditor_ != nullptr)
    {
        return;
    }

    if (getTextArea().contains(e.position.toInt()))
    {
        startPanTextEdit();
    }
}

void InspectorPanControl::mouseEnter(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    hoveredComponent_ = true;
    repaint();
}

void InspectorPanControl::mouseExit(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    hoveredComponent_ = false;
    repaint();
}

juce::MouseCursor InspectorPanControl::getMouseCursor()
{
    return juce::MouseCursor::NormalCursor;
}

void InspectorPanControl::startPanTextEdit()
{
    if (panEditor_ != nullptr)
    {
        return;
    }

    juce::Component::SafePointer<InspectorPanControl> safe(this);
    juce::MessageManager::callAsync([safe]() {
        if (safe != nullptr)
        {
            safe->attachPanEditorFromAsync();
        }
    });
}

void InspectorPanControl::attachPanEditorFromAsync()
{
    if (panEditor_ != nullptr)
    {
        return;
    }

    suppressPanEditorFocusLoss_ = true;

    auto ed = std::make_unique<juce::TextEditor>();
    ed->setMultiLine(false);
    ed->setReturnKeyStartsNewLine(false);
    ed->setScrollbarsShown(false);
    ed->setCaretVisible(true);
    ed->setReadOnly(false);
    ed->setJustification(juce::Justification::centred);
    ed->setIndents(0, 3);
    ed->setFont(juce::Font(juce::FontOptions(11.5f)).boldened());
    ed->setColour(juce::TextEditor::textColourId, labelColour());
    ed->setColour(juce::TextEditor::backgroundColourId, fieldBackgroundColour());
    ed->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    ed->setColour(juce::TextEditor::focusedOutlineColourId,
                  juce::Colours::deepskyblue.withAlpha(0.65f));
    ed->setBounds(getTextArea());
    ed->setText(formatPanForDisplay(pan_), juce::dontSendNotification);
    ed->selectAll();
    ed->addListener(this);

    addAndMakeVisible(*ed);
    panEditor_ = std::move(ed);
    panEditor_->toFront(false);
    panEditor_->grabKeyboardFocus();

    juce::Component::SafePointer<InspectorPanControl> safe(this);
    juce::MessageManager::callAsync([safe]() {
        if (safe == nullptr)
        {
            return;
        }
        safe->suppressPanEditorFocusLoss_ = false;
        if (safe->panEditor_ != nullptr)
        {
            safe->panEditor_->grabKeyboardFocus();
        }
    });

    repaint();
}

void InspectorPanControl::textEditorReturnKeyPressed(juce::TextEditor& ed)
{
    if (panEditor_.get() != &ed)
    {
        return;
    }

    const auto parsed = parsePanDisplayReadout(ed.getText());
    if (!parsed.has_value())
    {
        return;
    }

    ed.removeListener(this);
    commitPan(*parsed, juce::sendNotificationSync);
    removeChildComponent(&ed);
    panEditor_.reset();
    repaint();
}

void InspectorPanControl::textEditorEscapeKeyPressed(juce::TextEditor& ed)
{
    if (panEditor_.get() != &ed)
    {
        return;
    }

    ed.removeListener(this);
    removeChildComponent(&ed);
    panEditor_.reset();
    repaint();
}

void InspectorPanControl::textEditorFocusLost(juce::TextEditor& ed)
{
    if (panEditor_.get() != &ed)
    {
        return;
    }

    if (suppressPanEditorFocusLoss_)
    {
        return;
    }

    const auto parsed = parsePanDisplayReadout(ed.getText());
    ed.removeListener(this);
    if (parsed.has_value())
    {
        commitPan(*parsed, juce::sendNotificationSync);
    }
    removeChildComponent(&ed);
    panEditor_.reset();
    repaint();
}
