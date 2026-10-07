#pragma once

// =============================================================================
// SoloMemoryStrip — the four Solo-memory toggle buttons above the track-header column
// =============================================================================
//
// ROLE (spec §3)
//   Main window only: a small "SOLO 1 2 3 4" row sitting in the toolbar band directly above the
//   track-header column (same height as the Pointer/Split tool row, aligned with the header
//   column, following its width). At most ONE memory is selected; all four may be off (the
//   temporary solo set is current then). The strip owns NO solo state: the active index comes
//   from the provider (SoloCoordinator → Session) on every paint, and a click only reports the
//   button index — activate / switch / back-to-temporary semantics live in the coordinator.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "ui/TrackStripButtonGlyphs.h"

#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

class SoloMemoryStrip final : public juce::Component
{
public:
    static constexpr int kMemoryCount = 4;
    /// Same square as the header strip cells so the row reads as family.
    static constexpr int kButtonSidePx = 22;
    static constexpr int kButtonGapPx = 4;
    static constexpr int kCaptionWidthPx = 34;
    /// Width at which caption + four buttons fit; below it the caption is dropped first.
    static constexpr int kPreferredWidthPx
        = kCaptionWidthPx + kButtonGapPx + kMemoryCount * kButtonSidePx + (kMemoryCount - 1) * kButtonGapPx;

    /// −1 = no memory selected (temporary set current); 0 … 3 = that memory is current.
    std::function<int()> activeMemoryIndexProvider;
    /// Click on button `index` (0 … 3). The coordinator applies the toggle semantics.
    std::function<void(int)> onMemoryButtonClick;

    SoloMemoryStrip() { setRepaintsOnMouseActivity(true); }

    void paint(juce::Graphics& g) override
    {
        using namespace track_strip_glyphs;
        const int active = activeMemoryIndexProvider != nullptr ? activeMemoryIndexProvider() : -1;
        const juce::Colour edge(kCtlNeutralEdgeArgb);
        const auto caption = captionBounds();
        if (!caption.isEmpty())
        {
            g.setColour(juce::Colour(0xffb8bec6));
            g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
            g.drawFittedText("SOLO", caption, juce::Justification::centredLeft, 1);
        }
        const juce::Point<int> mouse = getMouseXYRelative();
        for (int i = 0; i < kMemoryCount; ++i)
        {
            const auto cell = buttonBounds(i);
            if (cell.isEmpty())
            {
                continue;
            }
            const bool isActive = (i == active);
            const bool hovered = isMouseOverOrDragging() && cell.contains(mouse);
            drawStandardStripButtonFace(g,
                                        cell.toFloat(),
                                        juce::Colour(isActive ? kSoloOnArgb : kNeutralFaceArgb),
                                        edge,
                                        hovered);
            drawStripLetter(g, cell, juce::String(i + 1),
                            juce::Colour(isActive ? 0xfff8f8ff : kGlyphOffLetterArgb));
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || onMemoryButtonClick == nullptr)
        {
            return;
        }
        for (int i = 0; i < kMemoryCount; ++i)
        {
            if (buttonBounds(i).contains(e.getPosition()))
            {
                onMemoryButtonClick(i);
                repaint();
                return;
            }
        }
    }

    /// Geometry for tests: the clickable cell of memory `index` (empty when clipped away).
    [[nodiscard]] juce::Rectangle<int> buttonBounds(const int index) const noexcept
    {
        if (index < 0 || index >= kMemoryCount)
        {
            return {};
        }
        auto b = getLocalBounds();
        const bool showCaption = b.getWidth() >= kPreferredWidthPx;
        const int side = juce::jmin(kButtonSidePx, b.getHeight());
        if (side < 8)
        {
            return {};
        }
        const int x0 = b.getX() + (showCaption ? kCaptionWidthPx + kButtonGapPx : 0)
                       + index * (kButtonSidePx + kButtonGapPx);
        const juce::Rectangle<int> cell(x0, b.getCentreY() - side / 2, kButtonSidePx, side);
        return cell.getIntersection(b);
    }

private:
    [[nodiscard]] juce::Rectangle<int> captionBounds() const noexcept
    {
        auto b = getLocalBounds();
        if (b.getWidth() < kPreferredWidthPx)
        {
            return {};
        }
        return b.removeFromLeft(kCaptionWidthPx);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoloMemoryStrip)
};
