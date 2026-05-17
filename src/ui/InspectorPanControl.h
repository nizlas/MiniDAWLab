#pragma once

// Compact Cubase-style pan field for the Inspector (message thread UI only).

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

class InspectorPanControl final : public juce::Component,
                                   private juce::TextEditor::Listener
{
public:
    InspectorPanControl();
    ~InspectorPanControl() override;

    /// Normalized pan [-1, +1]; sanitized via `sanitizeTrackStereoPan`.
    void setPan(float pan, juce::NotificationType notify = juce::sendNotificationSync);

    [[nodiscard]] float getPan() const noexcept { return pan_; }

    /// Fired while dragging / programmatic commits when `notify != dontSend`.
    std::function<void(float normalizedPan)> onPanChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    [[nodiscard]] bool hitTest(int x, int y) override;
    [[nodiscard]] juce::MouseCursor getMouseCursor() override;

private:
    struct Layout final
    {
        juce::Rectangle<float> field;
        juce::Rectangle<float> markerArea;
        juce::Rectangle<float> textArea;
        float centerX = 0.f;
        float travelLeft = 0.f;
        float travelRight = 0.f;
        float stickWidth = 3.f;

        [[nodiscard]] float markerXForPan(float normalizedPan) const noexcept;
    };

    [[nodiscard]] Layout computeLayout() const;

    [[nodiscard]] juce::Rectangle<int> getPanBoxBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getTextArea() const;
    [[nodiscard]] juce::Rectangle<int> getMarkerHitRect() const;

    [[nodiscard]] bool isInMarkerHitZone(juce::Point<int> p) const noexcept;

    void applyPanFromLocalX(float localX, juce::NotificationType notify);
    void commitPan(float pan, juce::NotificationType notify);

    void startPanTextEdit();
    void attachPanEditorFromAsync();

    void textEditorReturnKeyPressed(juce::TextEditor& ed) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& ed) override;
    void textEditorFocusLost(juce::TextEditor& ed) override;

    float pan_ = 0.f;
    bool hoveredComponent_ = false;
    bool panDragActive_ = false;
    bool suppressPanEditorFocusLoss_ = false;

    std::unique_ptr<juce::TextEditor> panEditor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorPanControl)
};
