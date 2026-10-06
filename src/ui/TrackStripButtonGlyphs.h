#pragma once

// =============================================================================
// TrackStripButtonGlyphs — the one drawing vocabulary of DAL's square strip buttons
// =============================================================================
//
// ROLE
//   Track headers (`TrackHeaderView`) and the mixer's channel strips (`MixerChannelStrip`) show
//   the same Power / Mute / Monitor / Record-arm / Instrument-editor / Alternatives cells. The
//   face, glyphs and state colours live here once so both views stay visually identical; a
//   change to a glyph or a state colour is made in this header only.
//
// STATE COLOURS (identical to the header strip since 1.1.x)
//   power on `0xff2d9d53` / standby `0xff5a5858`; mute on `0xffc6a42a`; monitor on `0xffe07b18`;
//   arm on `0xffd01818`; neutral (clickable, off) `0xff5a5858`; disabled face `0xff3e3e3e`.
//
// THREADING
//   [Message thread] paint helpers only; pure functions of the Graphics context.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace track_strip_glyphs
{

inline constexpr juce::uint32 kPowerOnArgb = 0xff2d9d53;
inline constexpr juce::uint32 kPowerStandbyArgb = 0xff5a5858;
inline constexpr juce::uint32 kMuteOnArgb = 0xffc6a42a;
inline constexpr juce::uint32 kMonitorOnArgb = 0xffe07b18;
inline constexpr juce::uint32 kArmOnArgb = 0xffd01818;
inline constexpr juce::uint32 kNeutralFaceArgb = 0xff5a5858;
inline constexpr juce::uint32 kDisabledFaceArgb = 0xff3e3e3e;
inline constexpr juce::uint32 kInstrumentEditorFaceArgb = 0xff5c5f66;
inline constexpr juce::uint32 kAlternativesFaceArgb = 0xff4f545c;
inline constexpr juce::uint32 kGlyphLightArgb = 0xfff2f6f9;
inline constexpr juce::uint32 kGlyphOnLetterDarkArgb = 0xff0a0a0a;
inline constexpr juce::uint32 kGlyphOffLetterArgb = 0xffeaeaea;
inline constexpr juce::uint32 kGlyphDisabledArgb = 0xff7a7a7a;
inline constexpr juce::uint32 kEdgeInactiveStrokeArgb = 0xc0222222;
inline constexpr float kCubaseCtlCornerRadMax = 2.85f;

[[nodiscard]] inline float cubaseCornerRadiusForSquare(const float side) noexcept
{
    return juce::jlimit(1.4f, kCubaseCtlCornerRadMax, side * 0.16f);
}

[[nodiscard]] inline float stripStandardGlyphInsetForSquareBody(const float squareSidePx) noexcept
{
    return juce::jlimit(2.0f, 3.5f, squareSidePx * 0.11f);
}

/// One shared inset for the non-letter glyphs (power ring, speaker, piano, layers) in every state.
[[nodiscard]] inline juce::Rectangle<float> nonLetterGlyphAreaFromSquareBodyPx(const juce::Rectangle<int> squareBodyPx) noexcept
{
    const float side = static_cast<float>(juce::jmin(squareBodyPx.getWidth(), squareBodyPx.getHeight()));
    if (side < 6.5f)
    {
        return squareBodyPx.toFloat();
    }
    const float pad = stripStandardGlyphInsetForSquareBody(side);
    return squareBodyPx.toFloat().reduced(pad);
}

inline void drawStandardStripButtonFace(juce::Graphics& g,
                                        const juce::Rectangle<float> body,
                                        juce::Colour fill,
                                        juce::Colour edge,
                                        const bool hovered)
{
    const float rad = cubaseCornerRadiusForSquare(juce::jmin(body.getWidth(), body.getHeight()));
    if (hovered)
    {
        fill = fill.brighter(0.12f);
        edge = edge.brighter(0.28f);
    }
    g.setColour(fill);
    g.fillRoundedRectangle(body, rad);
    g.setColour(edge);
    g.drawRoundedRectangle(body, rad, 1.0f);
}

inline void drawPowerGlyphInSquare(juce::Graphics& g, juce::Rectangle<float> icon, const juce::Colour glyphColour)
{
    const float side = juce::jmin(icon.getWidth(), icon.getHeight());
    if (side <= 4.0f)
    {
        return;
    }
    icon = juce::Rectangle<float>(icon.getCentreX() - side * 0.5f, icon.getCentreY() - side * 0.5f, side, side);
    const auto x = [&](const float nx) { return icon.getX() + nx * side; };
    const auto y = [&](const float ny) { return icon.getY() + ny * side; };
    const float stroke = juce::jlimit(1.75f, 2.5f, side * 0.15f);
    g.setColour(glyphColour);

    constexpr float ringCx = 0.5f;
    constexpr float ringCy = 0.54f;
    constexpr float ringR = 0.36f;
    constexpr float arcFromDeg = 35.0f;
    constexpr float arcToDeg = 325.0f;
    juce::Path ring;
    ring.addCentredArc(x(ringCx), y(ringCy), side * ringR, side * ringR, 0.0f,
                       juce::degreesToRadians(arcFromDeg), juce::degreesToRadians(arcToDeg), true);
    g.strokePath(ring, juce::PathStrokeType(stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    juce::Path stem;
    stem.startNewSubPath(x(0.5f), y(0.1f));
    stem.lineTo(x(0.5f), y(0.38f));
    g.strokePath(stem, juce::PathStrokeType(stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
}

/// Three vertical white keys + two black keys; separators / black keys as near-black strokes.
inline void drawInstrumentPianoGlyphCubaseSimple(juce::Graphics& g, const juce::Rectangle<float> glyphArea)
{
    if (glyphArea.getWidth() < 5.0f || glyphArea.getHeight() < 6.0f)
    {
        return;
    }
    const auto kb = glyphArea.reduced(1.2f);
    const float w = kb.getWidth();
    const float h = kb.getHeight();
    if (w < 3.5f || h < 4.5f)
    {
        return;
    }
    const float rad = juce::jlimit(1.0f, 1.35f, juce::jmin(w, h) * 0.11f);
    g.setColour(juce::Colour(0xfffafafa));
    g.fillRoundedRectangle(kb, rad);
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(kb, rad, 1.0f);

    const float bx1 = kb.getX() + w / 3.0f;
    const float bx2 = kb.getX() + 2.0f * w / 3.0f;
    constexpr float sepThick = 1.35f;
    g.setColour(juce::Colours::black);
    g.drawLine(bx1, kb.getY() + 1.0f + rad * 0.15f, bx1, kb.getBottom() - 1.0f, sepThick);
    g.drawLine(bx2, kb.getY() + 1.0f + rad * 0.15f, bx2, kb.getBottom() - 1.0f, sepThick);

    const float bkH = juce::jlimit(h * 0.45f, h * 0.55f, h * 0.52f);
    const float bkW = juce::jmax(2.8f, w * 0.22f);
    const float yb = kb.getY() + 0.85f;
    g.fillRect(juce::Rectangle<float>(bx1 - bkW * 0.5f, yb, bkW, bkH));
    g.fillRect(juce::Rectangle<float>(bx2 - bkW * 0.5f, yb, bkW, bkH));
}

/// "Instrument alternatives" glyph: two overlapping rounded rectangles (layers).
inline void drawAlternativesLayersGlyph(juce::Graphics& g, const juce::Rectangle<float> glyphArea)
{
    if (glyphArea.getWidth() < 5.0f || glyphArea.getHeight() < 6.0f)
    {
        return;
    }
    const auto a = glyphArea.reduced(1.0f);
    const float w = a.getWidth() * 0.70f;
    const float h = a.getHeight() * 0.70f;
    if (w < 3.0f || h < 3.0f)
    {
        return;
    }
    const float rad = juce::jlimit(0.8f, 1.4f, juce::jmin(w, h) * 0.22f);
    const juce::Rectangle<float> back(a.getX(), a.getY(), w, h);
    const juce::Rectangle<float> front(a.getRight() - w, a.getBottom() - h, w, h);
    g.setColour(juce::Colour(0xffb9c2cc));
    g.drawRoundedRectangle(back, rad, 1.1f);
    g.setColour(juce::Colour(0xff31363d));
    g.fillRoundedRectangle(front, rad);
    g.setColour(juce::Colour(0xfff2f6f9));
    g.drawRoundedRectangle(front, rad, 1.1f);
}

/// Input-monitoring speaker glyph: cabinet + cone + one sound arc, tinted by the caller.
inline void drawMonitorSpeakerGlyph(juce::Graphics& g, const juce::Rectangle<float> glyphArea, const juce::Colour glyphColour)
{
    if (glyphArea.getWidth() < 5.0f || glyphArea.getHeight() < 6.0f)
    {
        return;
    }
    const auto a = glyphArea.reduced(1.0f);
    const float w = a.getWidth();
    const float h = a.getHeight();
    if (w < 4.0f || h < 4.0f)
    {
        return;
    }
    g.setColour(glyphColour);
    const float boxW = w * 0.24f;
    const float boxH = h * 0.38f;
    const float boxX = a.getX();
    const float boxY = a.getCentreY() - boxH * 0.5f;
    const float coneX = boxX + boxW;
    const float coneRight = a.getX() + w * 0.62f;
    const float coneHalf = h * 0.42f;
    juce::Path speaker;
    speaker.addRectangle(boxX, boxY, boxW, boxH);
    speaker.startNewSubPath(coneX, boxY);
    speaker.lineTo(coneRight, a.getCentreY() - coneHalf);
    speaker.lineTo(coneRight, a.getCentreY() + coneHalf);
    speaker.lineTo(coneX, boxY + boxH);
    speaker.closeSubPath();
    g.fillPath(speaker);
    const float stroke = juce::jlimit(1.1f, 1.6f, w * 0.12f);
    juce::Path arc;
    arc.addCentredArc(coneRight + w * 0.10f, a.getCentreY(), w * 0.22f, h * 0.34f, 0.0f,
                      juce::degreesToRadians(20.0f), juce::degreesToRadians(160.0f), true);
    g.strokePath(arc, juce::PathStrokeType(stroke));
}

/// Letter cells ("M", "R"): font height follows the square body like the header strip.
inline void drawStripLetter(juce::Graphics& g, const juce::Rectangle<int> bodyPx, const juce::String& letter, const juce::Colour colour)
{
    juce::Graphics::ScopedSaveState gs(g);
    g.reduceClipRegion(bodyPx);
    const float fontH = juce::jlimit(8.5f, 11.5f,
                                     juce::jmin(static_cast<float>(bodyPx.getWidth()), static_cast<float>(bodyPx.getHeight())) * 0.52f);
    g.setFont(juce::Font(juce::FontOptions().withHeight(fontH)));
    g.setColour(colour);
    g.drawFittedText(letter, bodyPx, juce::Justification::centred, 1);
}

/// The kinds of cell both views draw.
enum class StripButtonKind
{
    InstrumentEditor,
    Power,
    Mute,
    Monitor,
    Arm,
    Alternatives,
};

/// Complete state of one cell as the two views describe it.
struct StripButtonState
{
    StripButtonKind kind = StripButtonKind::Mute;
    bool enabled = true;      ///< clickable (false = dimmed, non-interactive face)
    bool active = false;      ///< mute on / monitor on / armed; for Power: the row is ON
    bool hovered = false;
};

/// Draw one complete cell exactly like the track header strip does. `bodyPx` is the square body
/// (already inset from the cell); `ctlEdgeNeutral` is the neutral edge stroke of the host view.
inline void drawStripButton(juce::Graphics& g, const juce::Rectangle<int> bodyPx, const StripButtonState& s, const juce::Colour ctlEdgeNeutral)
{
    if (bodyPx.isEmpty())
    {
        return;
    }
    const auto rf = bodyPx.toFloat();
    const bool hoverBrighten = s.hovered && s.enabled;
    const juce::Colour edgeInactive(kEdgeInactiveStrokeArgb);
    switch (s.kind)
    {
    case StripButtonKind::InstrumentEditor:
        drawStandardStripButtonFace(g, rf, juce::Colour(s.enabled ? kInstrumentEditorFaceArgb : kDisabledFaceArgb),
                                    s.enabled ? ctlEdgeNeutral : edgeInactive, hoverBrighten);
        drawInstrumentPianoGlyphCubaseSimple(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx));
        break;
    case StripButtonKind::Power:
        drawStandardStripButtonFace(g, rf, juce::Colour(s.active ? kPowerOnArgb : kPowerStandbyArgb), ctlEdgeNeutral, hoverBrighten);
        drawPowerGlyphInSquare(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx), juce::Colour(kGlyphLightArgb));
        break;
    case StripButtonKind::Mute:
        if (s.enabled)
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(s.active ? kMuteOnArgb : kNeutralFaceArgb), ctlEdgeNeutral, hoverBrighten);
            drawStripLetter(g, bodyPx, "M", juce::Colour(s.active ? kGlyphOnLetterDarkArgb : kGlyphOffLetterArgb));
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(kDisabledFaceArgb), edgeInactive, false);
            drawStripLetter(g, bodyPx, "M", juce::Colour(kGlyphDisabledArgb));
        }
        break;
    case StripButtonKind::Monitor:
        if (s.enabled)
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(s.active ? kMonitorOnArgb : kNeutralFaceArgb), ctlEdgeNeutral, hoverBrighten);
            drawMonitorSpeakerGlyph(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx),
                                    juce::Colour(s.active ? 0xff141414 : kGlyphOffLetterArgb));
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(kDisabledFaceArgb), edgeInactive, false);
            drawMonitorSpeakerGlyph(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx), juce::Colour(kGlyphDisabledArgb));
        }
        break;
    case StripButtonKind::Arm:
        if (s.enabled)
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(s.active ? kArmOnArgb : kNeutralFaceArgb), ctlEdgeNeutral, hoverBrighten);
            drawStripLetter(g, bodyPx, "R", juce::Colour(s.active ? 0xfff8f8ff : kGlyphOffLetterArgb));
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(kDisabledFaceArgb), edgeInactive, false);
            drawStripLetter(g, bodyPx, "R", juce::Colour(0xff888888));
        }
        break;
    case StripButtonKind::Alternatives:
        drawStandardStripButtonFace(g, rf, juce::Colour(kAlternativesFaceArgb), ctlEdgeNeutral, hoverBrighten);
        drawAlternativesLayersGlyph(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx));
        break;
    }
}

} // namespace track_strip_glyphs
