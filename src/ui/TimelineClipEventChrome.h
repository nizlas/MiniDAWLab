#pragma once

// =============================================================================
// TimelineClipEventChrome — outer chrome for timeline clip/event rectangles
// =============================================================================
// Shared by placed audio clips (`ClipWaveformView`) and instrument MIDI lane events
// (`MidiEventLane`). Internal waveform / MIDI note preview are not drawn here.
// =============================================================================

#include <juce_graphics/juce_graphics.h>

#include <cstdint>

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

/// Top-edge fade handles and the centred event-gain bar. Hidden when the event is too
/// short to keep them off the bottom trim corners.
inline constexpr float kFadeHandlePx = 7.0f;
inline constexpr float kGainHandleW = 18.0f;
inline constexpr float kGainHandleH = 6.0f;
inline constexpr float kGainHandleDropPx = 10.0f;
inline constexpr float kMinEventHeightForEnvelopeHandlesPx = 30.0f;
inline constexpr float kMinEventWidthForEnvelopeHandlesPx = 36.0f;

[[nodiscard]] inline bool envelopeHandlesFit(juce::Rectangle<float> eventRect) noexcept
{
    return eventRect.getWidth() >= kMinEventWidthForEnvelopeHandlesPx
        && eventRect.getHeight() >= kMinEventHeightForEnvelopeHandlesPx;
}

[[nodiscard]] inline float fadeInEndX(juce::Rectangle<float> eventRect,
                                      std::int64_t fadeInSamples,
                                      std::int64_t lengthSamples) noexcept
{
    if (lengthSamples <= 0 || fadeInSamples <= 0)
    {
        return eventRect.getX();
    }
    const double frac = juce::jlimit(0.0, 1.0, static_cast<double>(fadeInSamples) / static_cast<double>(lengthSamples));
    return eventRect.getX() + static_cast<float>(frac) * eventRect.getWidth();
}

[[nodiscard]] inline float fadeOutStartX(juce::Rectangle<float> eventRect,
                                         std::int64_t fadeOutSamples,
                                         std::int64_t lengthSamples) noexcept
{
    if (lengthSamples <= 0 || fadeOutSamples <= 0)
    {
        return eventRect.getRight();
    }
    const double frac = juce::jlimit(0.0, 1.0, static_cast<double>(fadeOutSamples) / static_cast<double>(lengthSamples));
    return eventRect.getRight() - static_cast<float>(frac) * eventRect.getWidth();
}

[[nodiscard]] inline juce::Rectangle<float> squareHandleAt(juce::Rectangle<float> eventRect, float centreX) noexcept
{
    juce::Rectangle<float> r {
        centreX - kFadeHandlePx * 0.5f, eventRect.getY(), kFadeHandlePx, kFadeHandlePx
    };
    if (r.getX() < eventRect.getX())
    {
        r.setX(eventRect.getX());
    }
    if (r.getRight() > eventRect.getRight())
    {
        r.setX(eventRect.getRight() - r.getWidth());
    }
    return r;
}

[[nodiscard]] inline juce::Rectangle<float> fadeInHandleRect(juce::Rectangle<float> eventRect,
                                                             std::int64_t fadeInSamples,
                                                             std::int64_t lengthSamples) noexcept
{
    return squareHandleAt(eventRect, fadeInEndX(eventRect, fadeInSamples, lengthSamples));
}

[[nodiscard]] inline juce::Rectangle<float> fadeOutHandleRect(juce::Rectangle<float> eventRect,
                                                              std::int64_t fadeOutSamples,
                                                              std::int64_t lengthSamples) noexcept
{
    return squareHandleAt(eventRect, fadeOutStartX(eventRect, fadeOutSamples, lengthSamples));
}

/// Centred on the top edge. Drops inside the event when a fade handle would cover it,
/// including when a fade reaches the middle. The fade length itself is not shortened.
[[nodiscard]] inline juce::Rectangle<float> gainHandleRect(juce::Rectangle<float> eventRect,
                                                           juce::Rectangle<float> fadeInHandle,
                                                           juce::Rectangle<float> fadeOutHandle) noexcept
{
    juce::Rectangle<float> g {
        eventRect.getCentreX() - kGainHandleW * 0.5f, eventRect.getY(), kGainHandleW, kGainHandleH
    };
    if (g.getX() < eventRect.getX())
    {
        g.setX(eventRect.getX());
    }
    if (g.getRight() > eventRect.getRight())
    {
        g.setX(juce::jmax(eventRect.getX(), eventRect.getRight() - g.getWidth()));
    }
    if (g.intersects(fadeInHandle) || g.intersects(fadeOutHandle))
    {
        g.setY(eventRect.getY() + kGainHandleDropPx);
    }
    if (g.getBottom() > eventRect.getBottom())
    {
        g.setY(juce::jmax(eventRect.getY(), eventRect.getBottom() - g.getHeight()));
    }
    return g;
}

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

inline constexpr float kEventNameLabelFontPx = 11.5f;
inline constexpr float kEventNameLabelHeightPx = 14.0f;
inline constexpr float kEventNameLabelMaxWidthPx = 180.0f;
/// Explorer-style rename: delay after a click-on-selected-label before the inline editor opens,
/// so a fast double-click (open editor / other double-click action) never triggers rename.
inline constexpr int kClipRenameSecondClickDelayMs = 400;

/// Top-left name strip (Cubase / file-explorer style) — shared by audio clips and MIDI events for
/// both painting and rename-click hit-testing. Clamped inside `eventRect`; may be empty for tiny clips.
[[nodiscard]] inline juce::Rectangle<float> clipEventTopLeftNameBounds(juce::Rectangle<float> eventRect)
{
    const float w = juce::jmin(kEventNameLabelMaxWidthPx, eventRect.getWidth() - 8.0f);
    const float h = juce::jmin(kEventNameLabelHeightPx, eventRect.getHeight() - 4.0f);
    if (w <= 4.0f || h <= 6.0f)
    {
        return {};
    }
    return { eventRect.getX() + 4.0f, eventRect.getY() + 2.0f, w, h };
}

/// Draw `name` in the top-left strip, clipped to the event rect. No-op for empty text or tiny clips.
inline void paintEventTopLeftNameLabel(juce::Graphics& g,
                                       juce::Rectangle<float> eventRect,
                                       const juce::String& name)
{
    const juce::String label = name.trim();
    if (label.isEmpty())
    {
        return;
    }
    const juce::Rectangle<float> r = clipEventTopLeftNameBounds(eventRect);
    if (r.isEmpty())
    {
        return;
    }
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(eventRect.toNearestInt());
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.setFont(kEventNameLabelFontPx);
    g.drawFittedText(label, r.toNearestInt(), juce::Justification::centredLeft, 1);
}

/// Cheap overlay. The diagonal is the same linear envelope the audio path multiplies by.
/// The tinted triangle is the attenuated part above that line. Handles are optional.
inline void paintEventAmplitudeOverlay(juce::Graphics& g,
                                       juce::Rectangle<float> eventRect,
                                       std::int64_t fadeInSamples,
                                       std::int64_t fadeOutSamples,
                                       std::int64_t lengthSamples,
                                       bool showHandles)
{
    if (eventRect.isEmpty() || lengthSamples <= 0)
    {
        return;
    }
    const float left = eventRect.getX();
    const float right = eventRect.getRight();
    const float top = eventRect.getY();
    const float bottom = eventRect.getBottom();
    const float inX = fadeInEndX(eventRect, fadeInSamples, lengthSamples);
    const float outX = fadeOutStartX(eventRect, fadeOutSamples, lengthSamples);
    const juce::Colour damp = juce::Colour(0xff0e1624).withAlpha(0.42f);
    const juce::Colour line = juce::Colour(0xffd7e3f4).withAlpha(0.92f);

    if (fadeInSamples > 0)
    {
        juce::Path tri;
        tri.startNewSubPath(left, bottom);
        tri.lineTo(left, top);
        tri.lineTo(inX, top);
        tri.closeSubPath();
        g.setColour(damp);
        g.fillPath(tri);
        g.setColour(line);
        g.drawLine(left, bottom, inX, top, 1.0f);
    }
    if (fadeOutSamples > 0)
    {
        juce::Path tri;
        tri.startNewSubPath(outX, top);
        tri.lineTo(right, top);
        tri.lineTo(right, bottom);
        tri.closeSubPath();
        g.setColour(damp);
        g.fillPath(tri);
        g.setColour(line);
        g.drawLine(outX, top, right, bottom, 1.0f);
    }
    if (!showHandles || !envelopeHandlesFit(eventRect))
    {
        return;
    }
    const auto paintHandle = [&g](juce::Rectangle<float> h) {
        g.setColour(juce::Colour(0xff3d4a5a).brighter(0.35f).withAlpha(0.95f));
        g.fillRoundedRectangle(h, 1.0f);
        g.setColour(juce::Colour(0xffd7e3f4).withAlpha(0.95f));
        g.drawRoundedRectangle(h, 1.0f, 0.75f);
    };
    paintHandle(fadeInHandleRect(eventRect, fadeInSamples, lengthSamples));
    paintHandle(fadeOutHandleRect(eventRect, fadeOutSamples, lengthSamples));
    paintHandle(gainHandleRect(eventRect,
                               fadeInHandleRect(eventRect, fadeInSamples, lengthSamples),
                               fadeOutHandleRect(eventRect, fadeOutSamples, lengthSamples)));
}

} // namespace mini_daw::timeline_clip_chrome
