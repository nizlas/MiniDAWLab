#pragma once

// =============================================================================
// TimelineClipEventChrome — outer chrome for timeline clip/event rectangles
// =============================================================================
// Mirrors the anonymous-namespace event constants and paint *sequence* used for
// placed audio clips in ClipWaveformView (fill, border, selection overlay).
// Internal waveform / MIDI note preview are not drawn here.
// =============================================================================

#include <juce_graphics/juce_graphics.h>

namespace mini_daw::timeline_clip_chrome
{

inline constexpr float kEventCorner = 2.5f;
inline constexpr float kEventVerticalMargin = 4.0f;
inline constexpr float kEventStroke = 1.0f;
inline constexpr float kSelectionOverlayStroke = 1.2f;

[[nodiscard]] inline juce::Colour audioEventBodyFill()
{
    return juce::Colour(0xff343c4d);
}

/// Light neutral body for MIDI lane events (audio may keep audioEventBodyFill until a palette flip).
[[nodiscard]] inline juce::Colour midiLaneEventBodyFill()
{
    return juce::Colour(0xffdadfe6);
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

/// Inset for label-only content; vertical term matches ClipWaveformView `1.0f + kWaveInset * 0.5f`.
[[nodiscard]] inline juce::Rectangle<float> clipEventLabelBounds(juce::Rectangle<float> eventRect)
{
    constexpr float kWaveInset = 2.0f;
    return eventRect.reduced(4.0f, 1.0f + kWaveInset * 0.5f);
}

} // namespace mini_daw::timeline_clip_chrome
