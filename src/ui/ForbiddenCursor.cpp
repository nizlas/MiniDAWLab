// =============================================================================
// ForbiddenCursor.cpp
// =============================================================================

#include "ui/ForbiddenCursor.h"

#include <juce_core/juce_core.h>

juce::MouseCursor getForbiddenNoDropMouseCursor() noexcept
{
    static const juce::MouseCursor cursor = [] {
        // 32x32, transparent background; JUCE may downscale on some systems per `MouseCursor`
        // constructor docs.
        constexpr int kSize = 32;
        juce::Image img(juce::Image::ARGB, kSize, kSize, true);
        juce::Graphics g(img);
        g.fillAll(juce::Colours::transparentBlack);
        const float cx = kSize * 0.5f;
        const float cy = kSize * 0.5f;
        const float radius = 12.0f;
        g.setColour(juce::Colours::white);
        g.fillEllipse(cx - radius, cy - radius, 2.0f * radius, 2.0f * radius);
        g.setColour(juce::Colour(0xffd42828));
        g.drawEllipse(cx - radius, cy - radius, 2.0f * radius, 2.0f * radius, 2.2f);
        const float inset = radius * 0.68f;
        g.setColour(juce::Colour(0xffc01010));
        g.drawLine(cx - inset, cy - inset, cx + inset, cy + inset, 3.0f);
        return juce::MouseCursor(img, kSize / 2, kSize / 2);
    }();
    return cursor;
}
