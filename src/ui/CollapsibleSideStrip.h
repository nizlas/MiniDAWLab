#pragma once

// =============================================================================
// CollapsibleSideStrip — shared inspector-style left strip chrome
// =============================================================================
// Extracted from MainWindow's `TransportControlsContent` inspector layout:
// same splitter (invisible hit target), same collapsed knob (paint + drag + click-to-default),
// same width semantics: strip width 0 = fully collapsed; else total width including splitter.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace collapsible_side_strip
{

inline constexpr int kSplitterWidth = 6;
inline constexpr int kCollapsedKnobWidth = 6;
inline constexpr int kCollapsedKnobHeight = 25;

class Host
{
public:
    virtual ~Host() = default;

    /// Total strip width from the left edge including `kSplitterWidth` when expanded; **0** = collapsed.
    [[nodiscard]] virtual int getSideStripWidth() const noexcept = 0;
    virtual void setSideStripWidth(int w) noexcept = 0;
    [[nodiscard]] virtual int getSideStripMaxWidth() const noexcept = 0;
    [[nodiscard]] virtual int getSideStripDefaultWidth() const noexcept = 0;
    virtual void sideStripLayoutChanged() = 0;
};

class ResizeSplitter final : public juce::Component
{
public:
    explicit ResizeSplitter(Host& host) noexcept
        : host_(host)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void paint(juce::Graphics& g) override;

private:
    Host& host_;
    int anchorW_ = 0;
};

class CollapsedKnob final : public juce::Component
{
public:
    explicit CollapsedKnob(Host& host) noexcept
        : host_(host)
    {
        setInterceptsMouseClicks(true, true);
    }

    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void paint(juce::Graphics& g) override;

private:
    Host& host_;
    int dragAnchorW_ = 0;
};

} // namespace collapsible_side_strip
