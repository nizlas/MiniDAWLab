#pragma once

// =============================================================================
// ChannelFaderComponent — vertical Channel Volume fader (−∞ … +6 dB) with graded scale + value field
// =============================================================================
//
// ROLE
//   Inspector channel-panel control for the track's EXISTING `channelFaderGain`. It is not a new
//   gain stage: `onGainChanged(linear)` is wired by the owner to the same Session setter the old
//   Channel-volume text field used. Shows gain in dB (0.00 = unchanged), never a signal level.
//
// GESTURES
//   * Drag the knob (or click in the travel) — continuous updates while dragging.
//   * Ctrl/Cmd+click — reset to 0 dB (DAL's established reset gesture, same as pan / pre-gain).
//   * Mouse wheel — ±0.5 dB per notch (Shift: ±0.1 dB).
//   * Value field under the fader — type "-6", "+3 dB", "-inf"; Return commits, Escape reverts.
//
// SCALE
//   `channel_fader_scale::Scale`: graded travel (0 … −20 dB gets most of the length), ticks at the
//   printed marks so the knob always sits on the printed value.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "ui/ChannelFaderScale.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

class ChannelFaderComponent final : public juce::Component,
                                    private juce::TextEditor::Listener
{
public:
    ChannelFaderComponent();
    ~ChannelFaderComponent() override;

    /// Programmatic update from the session (no notification). Ignored while the user drags.
    void setLinearGain(float linearGain, juce::NotificationType notify);
    [[nodiscard]] float getLinearGain() const noexcept { return linearGain_; }
    [[nodiscard]] bool isDragging() const noexcept { return dragging_; }
    [[nodiscard]] bool isValueFieldBeingEdited() const noexcept;

    /// Fired on every user change (drag, wheel, reset, typed value) with the new linear gain.
    std::function<void(float linearGain)> onGainChanged;

    /// Scale-column width (ticks + labels) the fader uses beside its travel.
    static constexpr int kScaleColumnWidthPx = 26;
    static constexpr int kTravelColumnWidthPx = 22;
    static constexpr int kValueFieldHeightPx = 20;
    [[nodiscard]] static int preferredWidth() noexcept { return kTravelColumnWidthPx + kScaleColumnWidthPx + 4; }

    /// Geometry for tests / layout verification.
    [[nodiscard]] juce::Rectangle<int> getTravelBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getKnobBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getValueFieldBounds() const noexcept;
    [[nodiscard]] juce::String getValueFieldText() const;

    /// Applies typed text exactly as the value field does on Return (for tests).
    void commitTypedValue(const juce::String& text);
    /// The Ctrl/Cmd+click reset: back to 0 dB (unity), notifying like a user gesture.
    void resetToUnityGain();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    void textEditorEscapeKeyPressed(juce::TextEditor&) override;
    void textEditorFocusLost(juce::TextEditor&) override;

    void applyPosition(double position01, bool notify);
    void applyGain(float linearGain, bool notify);
    void syncValueField();
    [[nodiscard]] double positionForY(float y) const noexcept;

    float linearGain_ = 1.0f;
    bool dragging_ = false;
    double dragAnchorPosition_ = 0.0;
    float dragAnchorY_ = 0.0f;
    bool valueFieldGuard_ = false;
    std::unique_ptr<juce::TextEditor> valueField_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelFaderComponent)
};
