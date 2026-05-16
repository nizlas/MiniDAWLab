#pragma once

// =============================================================================
// TimelineClipEventChrome — outer chrome for timeline clip/event rectangles
// =============================================================================
// Shared by placed audio clips (`ClipWaveformView`) and instrument MIDI lane events
// (`MidiEventLane`). Internal waveform / MIDI note preview are not drawn here.
// =============================================================================

#include <juce_graphics/juce_graphics.h>

namespace mini_daw::timeline_clip_chrome
{

inline constexpr float kEventCorner = 2.5f;
inline constexpr float kEventVerticalMargin = 4.0f;
inline constexpr float kEventStroke = 1.0f;
inline constexpr float kSelectionOverlayStroke = 1.2f;

/// Cubase-style trim: small square at bottom-left / bottom-right (same as audio lane).
inline constexpr float kTrimHandleSquarePx = 5.0f;
inline constexpr float kMinEventWidthForTrimHandlePx = 12.0f;
inline constexpr float kTrimHandleMarginPx = 1.5f;
/// Full-height hit zone width on left/right edge (ClipWaveformView hit-test geometry).
inline constexpr float kTrimHitLeftEdgeBandPx = 6.0f;
inline constexpr float kTrimHitRightEdgeBandPx = 6.0f;

[[nodiscard]] inline juce::Colour unifiedClipEventBodyFill()
{
    return juce::Colour(0xff343c4d);
}

[[nodiscard]] inline juce::Colour audioEventBodyFill()
{
    return unifiedClipEventBodyFill();
}

[[nodiscard]] inline juce::Colour midiLaneEventBodyFill()
{
    return unifiedClipEventBodyFill();
}

[[nodiscard]] inline juce::Colour eventBodyBorderColour()
{
    return juce::Colour(0xff7a8aa0).withAlpha(0.9f);
}

[[nodiscard]] inline juce::Colour eventSelectionOverlayColour()
{
    return juce::Colour(0xff9eb8d8).withAlpha(0.95f);
}

inline void paintEventChromeBody(juce::Graphics& g,
                                 juce::Rectangle<float> eventRect,
                                 juce::Colour bodyFill)
{
    g.setColour(bodyFill);
    g.fillRoundedRectangle(eventRect, kEventCorner);
    g.setColour(eventBodyBorderColour());
    g.drawRoundedRectangle(eventRect, kEventCorner, kEventStroke);
}

inline void paintEventChromeSelectionOverlay(juce::Graphics& g, juce::Rectangle<float> eventRect)
{
    g.setColour(eventSelectionOverlayColour());
    g.drawRoundedRectangle(eventRect, kEventCorner, kSelectionOverlayStroke);
}

/// Stronger outline for the keyboard-focused / active clip among a multi-selection.
inline void paintEventChromeActiveSelectionOutline(juce::Graphics& g, juce::Rectangle<float> eventRect)
{
    g.setColour(juce::Colour(0xff3d7dd9));
    g.drawRoundedRectangle(eventRect, kEventCorner, kSelectionOverlayStroke + 2.0f);
}

/// Lower-corner trim affordance (filled square + stroke). Matches audio lane visuals.
inline void paintEventChromeTrimHandle(juce::Graphics& g,
                                       juce::Rectangle<float> eventRect,
                                       bool isLeftEdge)
{
    if (eventRect.getWidth() < kMinEventWidthForTrimHandlePx
        || eventRect.getHeight() < kTrimHandleSquarePx + 2.0f)
    {
        return;
    }
    const float hsz = juce::jmin(
        kTrimHandleSquarePx, eventRect.getWidth() * 0.4f, eventRect.getHeight() * 0.4f);
    if (hsz < 2.0f)
    {
        return;
    }
    const float hLeft = isLeftEdge
        ? juce::jmin(eventRect.getX() + kTrimHandleMarginPx, eventRect.getRight() - hsz - 0.5f)
        : juce::jmax(eventRect.getX() + 0.5f, eventRect.getRight() - kTrimHandleMarginPx - hsz);
    const float hTop = juce::jmax(
        eventRect.getY() + 0.5f, eventRect.getBottom() - kTrimHandleMarginPx - hsz);
    const juce::Rectangle<float> h{ hLeft, hTop, hsz, hsz };
    g.setColour(juce::Colour(0xff3d4a5a).brighter(0.25f).withAlpha(0.88f));
    g.fillRoundedRectangle(h, 1.0f);
    g.setColour(juce::Colour(0xff9eb0c8).withAlpha(0.95f));
    g.drawRoundedRectangle(h, 1.0f, 0.75f);
}

/// Inset for label-only content; vertical term matches ClipWaveformView `1.0f + kWaveInset * 0.5f`.
[[nodiscard]] inline juce::Rectangle<float> clipEventLabelBounds(juce::Rectangle<float> eventRect)
{
    constexpr float kWaveInset = 2.0f;
    return eventRect.reduced(4.0f, 1.0f + kWaveInset * 0.5f);
}

} // namespace mini_daw::timeline_clip_chrome
