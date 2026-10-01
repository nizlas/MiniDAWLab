#include "ui/ChannelFaderComponent.h"

#include <cmath>

namespace
{
    constexpr int kKnobHeightPx = 22;
    constexpr int kKnobWidthPx = 18;
    constexpr int kTopPadPx = 6;
    constexpr int kGapTravelToFieldPx = 4;

    constexpr juce::uint32 kTroughArgb = 0xff26292d;
    constexpr juce::uint32 kTrackLineArgb = 0xff0f1012;
    constexpr juce::uint32 kKnobArgb = 0xff7fb8e6;
    constexpr juce::uint32 kKnobLineArgb = 0xff1f3542;
    constexpr juce::uint32 kScaleTextArgb = 0xffb8bcc2;
    constexpr juce::uint32 kZeroMarkArgb = 0xffe6e6e6;

    using Scale = channel_fader_scale::Scale;
} // namespace

ChannelFaderComponent::ChannelFaderComponent()
{
    valueField_ = std::make_unique<juce::TextEditor>();
    valueField_->setMultiLine(false);
    valueField_->setReturnKeyStartsNewLine(false);
    valueField_->setFont(juce::FontOptions(11.0f));
    valueField_->setJustification(juce::Justification::centred);
    valueField_->setIndents(0, 3);
    valueField_->setSelectAllWhenFocused(true);
    valueField_->setTooltip("Channel volume (gain, dB). Type a value and press Return; \"-inf\" mutes. Ctrl/Cmd+click the fader resets to 0 dB.");
    valueField_->addListener(this);
    addAndMakeVisible(*valueField_);
    syncValueField();
}

ChannelFaderComponent::~ChannelFaderComponent()
{
    valueField_->removeListener(this);
}

bool ChannelFaderComponent::isValueFieldBeingEdited() const noexcept
{
    return valueField_ != nullptr && valueField_->hasKeyboardFocus(false);
}

void ChannelFaderComponent::setLinearGain(const float linearGain, const juce::NotificationType notify)
{
    if (dragging_)
    {
        return;
    }
    applyGain(linearGain, notify != juce::dontSendNotification);
}

void ChannelFaderComponent::applyGain(const float linearGain, const bool notify)
{
    const float g = juce::jlimit(0.0f, static_cast<float>(std::pow(10.0, Scale::kMaxDb / 20.0)), std::isfinite(linearGain) ? linearGain : 1.0f);
    const bool changed = std::abs(g - linearGain_) > 1.0e-7f;
    linearGain_ = g;
    if (!isValueFieldBeingEdited())
    {
        syncValueField();
    }
    repaint();
    if (changed && notify && onGainChanged != nullptr)
    {
        onGainChanged(linearGain_);
    }
}

void ChannelFaderComponent::applyPosition(const double position01, const bool notify)
{
    applyGain(Scale::linearGainForPosition(juce::jlimit(0.0, 1.0, position01)), notify);
}

void ChannelFaderComponent::syncValueField()
{
    valueFieldGuard_ = true;
    valueField_->setText(Scale::gainText(linearGain_), juce::dontSendNotification);
    valueFieldGuard_ = false;
}

juce::String ChannelFaderComponent::getValueFieldText() const
{
    return valueField_->getText();
}

juce::Rectangle<int> ChannelFaderComponent::getValueFieldBounds() const noexcept
{
    return { 0, getHeight() - kValueFieldHeightPx, getWidth(), kValueFieldHeightPx };
}

juce::Rectangle<int> ChannelFaderComponent::getTravelBounds() const noexcept
{
    // Travel = the y range the knob CENTRE moves over; the knob extends half its height beyond.
    const int top = kTopPadPx + kKnobHeightPx / 2;
    const int bottom = getHeight() - kValueFieldHeightPx - kGapTravelToFieldPx - kKnobHeightPx / 2;
    return { 2, top, kTravelColumnWidthPx, juce::jmax(1, bottom - top) };
}

juce::Rectangle<int> ChannelFaderComponent::getKnobBounds() const noexcept
{
    const juce::Rectangle<int> travel = getTravelBounds();
    const double pos = Scale::positionForLinearGain(linearGain_);
    const int cy = travel.getBottom() - static_cast<int>(std::lround(pos * travel.getHeight()));
    return { travel.getCentreX() - kKnobWidthPx / 2, cy - kKnobHeightPx / 2, kKnobWidthPx, kKnobHeightPx };
}

double ChannelFaderComponent::positionForY(const float y) const noexcept
{
    const juce::Rectangle<int> travel = getTravelBounds();
    if (travel.getHeight() <= 0)
    {
        return 0.0;
    }
    return juce::jlimit(0.0, 1.0, (static_cast<double>(travel.getBottom()) - static_cast<double>(y)) / travel.getHeight());
}

void ChannelFaderComponent::resized()
{
    valueField_->setBounds(getValueFieldBounds());
}

void ChannelFaderComponent::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> travel = getTravelBounds();
    // Trough + centre line.
    g.setColour(juce::Colour(kTroughArgb));
    g.fillRoundedRectangle(juce::Rectangle<float>((float)travel.getX(), (float)(travel.getY() - kKnobHeightPx / 2),
                                                  (float)travel.getWidth(), (float)(travel.getHeight() + kKnobHeightPx)),
                           3.0f);
    g.setColour(juce::Colour(kTrackLineArgb));
    g.fillRect(juce::Rectangle<float>((float)travel.getCentreX() - 1.0f, (float)travel.getY(), 2.0f, (float)travel.getHeight()));

    // Scale: ticks at every printed mark, 0 dB emphasised; labels only where they do not collide
    // (short travels drop the crowded low-end numbers, never the ticks).
    const int scaleX = travel.getRight() + 3;
    const bool labels = getWidth() >= kTravelColumnWidthPx + kScaleColumnWidthPx;
    constexpr int kLabelMinSpacingPx = 11;
    g.setFont(juce::FontOptions(9.0f));
    float lastLabelY = -1000.0f;
    for (const Scale::Mark& m : Scale::kMarks)
    {
        if (m.db <= Scale::kMinDb)
        {
            continue;
        }
        const float y = (float)travel.getBottom() - (float)(m.position * travel.getHeight());
        const bool zero = std::abs(m.db) < 1.0e-9;
        g.setColour(juce::Colour(zero ? kZeroMarkArgb : kScaleTextArgb));
        g.fillRect(juce::Rectangle<float>((float)scaleX, y, zero ? 6.0f : 4.0f, 1.0f));
        // Marks run top → bottom, so y grows; a label needs `kLabelMinSpacingPx` below the previous one.
        if (labels && (y - lastLabelY) >= (float)kLabelMinSpacingPx)
        {
            juce::String label = zero ? "0" : juce::String(static_cast<int>(std::lround(std::abs(m.db))));
            if (m.db > 0.0)
            {
                label = "+" + label;
            }
            g.drawText(label, juce::Rectangle<int>(scaleX + 7, (int)std::lround(y) - 6, kScaleColumnWidthPx - 7, 12),
                       juce::Justification::centredLeft, false);
            lastLabelY = y;
        }
    }
    // The −∞ mark at the very bottom (label only when it has room under the last number).
    g.setColour(juce::Colour(kScaleTextArgb));
    g.fillRect(juce::Rectangle<float>((float)scaleX, (float)travel.getBottom(), 4.0f, 1.0f));
    if (labels && ((float)travel.getBottom() - lastLabelY) >= (float)kLabelMinSpacingPx)
    {
        g.drawText(juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e")),
                   juce::Rectangle<int>(scaleX + 7, travel.getBottom() - 6, kScaleColumnWidthPx - 7, 12),
                   juce::Justification::centredLeft, false);
    }

    // Knob.
    const juce::Rectangle<int> knob = getKnobBounds();
    g.setColour(juce::Colour(kKnobArgb));
    g.fillRoundedRectangle(knob.toFloat(), 3.0f);
    g.setColour(juce::Colour(kKnobLineArgb));
    g.fillRect(juce::Rectangle<float>((float)knob.getX() + 2.0f, (float)knob.getCentreY() - 0.5f, (float)knob.getWidth() - 4.0f, 1.5f));
}

void ChannelFaderComponent::mouseDown(const juce::MouseEvent& e)
{
    if (getValueFieldBounds().contains(e.getPosition()))
    {
        return;
    }
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        resetToUnityGain(); // the established reset gesture: 0 dB
        return;
    }
    dragging_ = true;
    if (!getKnobBounds().expanded(2).contains(e.getPosition()))
    {
        applyPosition(positionForY(e.position.y), true); // click in the travel jumps there
    }
    dragAnchorPosition_ = Scale::positionForLinearGain(linearGain_);
    dragAnchorY_ = e.position.y;
}

void ChannelFaderComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_)
    {
        return;
    }
    const juce::Rectangle<int> travel = getTravelBounds();
    if (travel.getHeight() <= 0)
    {
        return;
    }
    // Shift = fine (1/4 speed).
    const double scale = e.mods.isShiftDown() ? 0.25 : 1.0;
    const double delta = (static_cast<double>(dragAnchorY_) - static_cast<double>(e.position.y)) * scale / travel.getHeight();
    applyPosition(dragAnchorPosition_ + delta, true);
}

void ChannelFaderComponent::mouseUp(const juce::MouseEvent&)
{
    dragging_ = false;
    syncValueField();
}

void ChannelFaderComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f)
    {
        return;
    }
    const double stepDb = e.mods.isShiftDown() ? 0.1 : 0.5;
    const double currentDb = Scale::dbForLinearGain(linearGain_);
    double next = (currentDb <= Scale::kMinDb ? -60.0 : currentDb) + (wheel.deltaY > 0.0f ? stepDb : -stepDb);
    next = juce::jlimit(Scale::kMinDb, Scale::kMaxDb, next);
    applyGain(next <= Scale::kMinDb ? 0.0f : static_cast<float>(std::pow(10.0, next / 20.0)), true);
}

void ChannelFaderComponent::resetToUnityGain()
{
    dragging_ = false;
    applyGain(1.0f, true);
}

void ChannelFaderComponent::commitTypedValue(const juce::String& text)
{
    float g = linearGain_;
    if (Scale::parseGainText(text, g))
    {
        applyGain(g, true);
    }
    syncValueField(); // unparsable → shows the unchanged value again
}

void ChannelFaderComponent::textEditorReturnKeyPressed(juce::TextEditor& ed)
{
    if (valueFieldGuard_)
    {
        return;
    }
    commitTypedValue(ed.getText());
    ed.giveAwayKeyboardFocus();
}

void ChannelFaderComponent::textEditorEscapeKeyPressed(juce::TextEditor& ed)
{
    syncValueField();
    ed.giveAwayKeyboardFocus();
}

void ChannelFaderComponent::textEditorFocusLost(juce::TextEditor& ed)
{
    if (valueFieldGuard_)
    {
        return;
    }
    commitTypedValue(ed.getText());
}
