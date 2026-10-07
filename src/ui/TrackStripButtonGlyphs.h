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

#include <cmath>

namespace track_strip_glyphs
{

inline constexpr juce::uint32 kPowerOnArgb = 0xff2d9d53;
inline constexpr juce::uint32 kPowerStandbyArgb = 0xff5a5858;
inline constexpr juce::uint32 kMuteOnArgb = 0xffc6a42a;
/// Solo: explicit S on = red (distinct from Record-arm's kArmOnArgb so both can light together).
inline constexpr juce::uint32 kSoloOnArgb = 0xffd2402e;
/// M face while a track is SILENCED BY SOLO without being stored-muted: a clearly distinct,
/// dimmed mute tint (darker desaturated gold — "effectively muted, but not your Mute flag").
inline constexpr juce::uint32 kMuteSoloSilencedFaceArgb = 0xff80702f;
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

// --- the track header plate (arrangement headers AND mixer strip headers) -----------------------
/// Active / inactive header fill, the 4 px active stripe at the left edge, the name text.
inline constexpr juce::uint32 kHeaderActiveFillArgb = 0xff2a4a5a;
inline constexpr juce::uint32 kHeaderInactiveFillArgb = 0xff333333;
inline constexpr int kHeaderActiveStripeWidthPx = 4;
inline constexpr float kHeaderNameFontHeight = 14.0f;
/// Neutral edge stroke the header strip hands to `drawStripButton`.
inline constexpr juce::uint32 kCtlNeutralEdgeArgb = 0xd0161616;

[[nodiscard]] inline juce::Colour headerActiveStripeColour() noexcept { return juce::Colours::deepskyblue; }
[[nodiscard]] inline juce::Colour headerNameColour() noexcept { return juce::Colours::whitesmoke; }
[[nodiscard]] inline juce::Colour headerFillColour(const bool active) noexcept
{
    return juce::Colour(active ? kHeaderActiveFillArgb : kHeaderInactiveFillArgb);
}

/// Paint the header plate exactly like `TrackHeaderView` does: fill + the active stripe.
inline void drawHeaderPlate(juce::Graphics& g, const juce::Rectangle<int> bounds, const bool active)
{
    g.setColour(headerFillColour(active));
    g.fillRect(bounds);
    if (active)
    {
        g.setColour(headerActiveStripeColour());
        g.fillRect(bounds.getX(), bounds.getY(), kHeaderActiveStripeWidthPx, bounds.getHeight());
    }
}

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

// --- track type icons (the header's colour segment, compact-header slice 2026-10-07) -----------------------------------
/// The arrangement row kinds as the header shows them in front of the order number.
enum class TrackTypeIcon
{
    Audio,      ///< waveform
    Instrument, ///< keyboard
    Midi,       ///< 5-pin MIDI connector (distinct from the instrument keyboard)
    Group,      ///< bus: three lines merging into one
    Master,     ///< Stereo Out: two overlapping circles (L / R)
};

/// Small vector icon in DAL's flat style, `ink`-coloured on the caller's segment background.
/// Degrades to nothing below ~8 px.
inline void drawTrackTypeIcon(juce::Graphics& g,
                              const juce::Rectangle<float> area,
                              const TrackTypeIcon icon,
                              const juce::Colour ink)
{
    const float side = juce::jmin(area.getWidth(), area.getHeight());
    if (side < 8.0f)
    {
        return;
    }
    const juce::Rectangle<float> a(area.getCentreX() - side * 0.5f, area.getCentreY() - side * 0.5f, side, side);
    const auto x = [&](const float nx) { return a.getX() + nx * side; };
    const auto y = [&](const float ny) { return a.getY() + ny * side; };
    const float stroke = juce::jlimit(1.0f, 1.6f, side * 0.1f);
    g.setColour(ink);
    switch (icon)
    {
    case TrackTypeIcon::Audio:
    {
        // Mirrored waveform: seven bars of varying height around the centre line.
        constexpr float heights[7] = { 0.30f, 0.62f, 0.90f, 0.50f, 0.78f, 0.40f, 0.22f };
        const float barW = side / 7.0f * 0.62f;
        for (int i = 0; i < 7; ++i)
        {
            const float cx = x((static_cast<float>(i) + 0.5f) / 7.0f);
            const float h = side * heights[i];
            g.fillRoundedRectangle(cx - barW * 0.5f, a.getCentreY() - h * 0.5f, barW, h, barW * 0.4f);
        }
        break;
    }
    case TrackTypeIcon::Instrument:
    {
        // Keyboard: three light keys with two black keys (outline in the ink colour).
        const juce::Rectangle<float> kb(x(0.06f), y(0.16f), side * 0.88f, side * 0.68f);
        g.drawRoundedRectangle(kb, 1.2f, stroke);
        const float kx1 = kb.getX() + kb.getWidth() / 3.0f;
        const float kx2 = kb.getX() + 2.0f * kb.getWidth() / 3.0f;
        g.drawLine(kx1, kb.getCentreY(), kx1, kb.getBottom(), stroke);
        g.drawLine(kx2, kb.getCentreY(), kx2, kb.getBottom(), stroke);
        const float bkW = juce::jmax(2.0f, kb.getWidth() * 0.2f);
        const float bkH = kb.getHeight() * 0.52f;
        g.fillRect(juce::Rectangle<float>(kx1 - bkW * 0.5f, kb.getY(), bkW, bkH));
        g.fillRect(juce::Rectangle<float>(kx2 - bkW * 0.5f, kb.getY(), bkW, bkH));
        break;
    }
    case TrackTypeIcon::Midi:
    {
        // DIN connector: ring + five pins.
        const juce::Rectangle<float> ring = a.reduced(side * 0.08f);
        g.drawEllipse(ring, stroke);
        const float pr = juce::jmax(0.9f, side * 0.09f);
        const float r = ring.getWidth() * 0.30f;
        const float cx = ring.getCentreX();
        const float cy = ring.getCentreY() + side * 0.02f;
        for (const float deg : { -60.0f, -30.0f, 0.0f, 30.0f, 60.0f })
        {
            const float rad = juce::degreesToRadians(deg);
            const float px = cx + std::sin(rad) * r;
            const float py = cy - std::cos(rad) * r + side * 0.08f;
            g.fillEllipse(px - pr, py - pr, pr * 2.0f, pr * 2.0f);
        }
        break;
    }
    case TrackTypeIcon::Group:
    {
        // Bus: three lines from the left merging into one line to the right.
        juce::Path p;
        for (const float ny : { 0.22f, 0.5f, 0.78f })
        {
            p.startNewSubPath(x(0.08f), y(ny));
            p.lineTo(x(0.42f), y(ny));
            p.lineTo(x(0.6f), y(0.5f));
        }
        p.startNewSubPath(x(0.6f), y(0.5f));
        p.lineTo(x(0.94f), y(0.5f));
        g.strokePath(p, juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        break;
    }
    case TrackTypeIcon::Master:
    {
        // Stereo out: two overlapping rings (L / R).
        const float r = side * 0.30f;
        g.drawEllipse(x(0.34f) - r, a.getCentreY() - r, r * 2.0f, r * 2.0f, stroke);
        g.drawEllipse(x(0.66f) - r, a.getCentreY() - r, r * 2.0f, r * 2.0f, stroke);
        break;
    }
    }
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
    Solo,
};

/// Complete state of one cell as the two views describe it.
struct StripButtonState
{
    StripButtonKind kind = StripButtonKind::Mute;
    bool enabled = true;      ///< clickable (false = dimmed, non-interactive face)
    bool active = false;      ///< mute on / monitor on / armed / explicit solo; for Power: row ON
    bool hovered = false;
    /// Mute cell only: the row is silenced BY SOLO while not stored-muted — the face uses the
    /// distinct dimmed tint (`kMuteSoloSilencedFaceArgb`) instead of the neutral face.
    bool soloSilenced = false;
    /// Mute cell only: draw the small lock marking (solo active ⇒ mute changes are locked).
    /// The effective state colours stay readable; the lock never obscures the "M".
    bool lockMarked = false;
};

/// Small drawn padlock in the cell's top-right corner (no image assets). Sized so the centred
/// letter stays fully readable even in the minimum 22 px cell.
inline void drawSmallLockGlyph(juce::Graphics& g, const juce::Rectangle<int> bodyPx)
{
    const float side = juce::jmin((float)bodyPx.getWidth(), (float)bodyPx.getHeight());
    if (side < 12.0f)
    {
        return;
    }
    const float lockW = juce::jlimit(4.0f, 6.0f, side * 0.30f);
    const float bodyH = lockW * 0.72f;
    const float shackleR = lockW * 0.32f;
    const float x1 = (float)bodyPx.getRight() - lockW - 1.5f;
    const float yBody = (float)bodyPx.getY() + 1.5f + shackleR;
    const juce::Colour lockCol(0xe6101010);
    g.setColour(lockCol);
    juce::Path shackle;
    shackle.addCentredArc(x1 + lockW * 0.5f, yBody, shackleR, shackleR, 0.0f,
                          juce::degreesToRadians(-90.0f), juce::degreesToRadians(90.0f), true);
    g.strokePath(shackle, juce::PathStrokeType(1.1f));
    g.fillRoundedRectangle(x1, yBody, lockW, bodyH, 0.8f);
}

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
        if (s.lockMarked)
        {
            // Solo active: Mute is locked but the cell still shows the EFFECTIVE state — stored
            // mute keeps its colour, solo-silenced rows get the distinct dimmed tint, audible rows
            // the neutral face. Never the generic disabled gray (the state must stay readable).
            const juce::Colour face(s.active ? kMuteOnArgb
                                             : (s.soloSilenced ? kMuteSoloSilencedFaceArgb : kNeutralFaceArgb));
            drawStandardStripButtonFace(g, rf, face, edgeInactive, false);
            drawStripLetter(g, bodyPx, "M", juce::Colour(s.active ? kGlyphOnLetterDarkArgb : kGlyphOffLetterArgb));
            drawSmallLockGlyph(g, bodyPx);
        }
        else if (s.enabled)
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
    case StripButtonKind::Solo:
        if (s.enabled)
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(s.active ? kSoloOnArgb : kNeutralFaceArgb), ctlEdgeNeutral, hoverBrighten);
            drawStripLetter(g, bodyPx, "S", juce::Colour(s.active ? 0xfff8f8ff : kGlyphOffLetterArgb));
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(kDisabledFaceArgb), edgeInactive, false);
            drawStripLetter(g, bodyPx, "S", juce::Colour(kGlyphDisabledArgb));
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
