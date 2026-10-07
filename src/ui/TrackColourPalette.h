#pragma once

// =============================================================================
// TrackColourPalette — the ONE place DAL's muted track colours are defined (UI side)
// =============================================================================
// Maps a `TrackColourKey` to the matte, slightly dark colours used for (a) the header's type-icon
// / number segment and (b) the body fill of that track's audio and MIDI events. Deliberately
// darker and less saturated than the light event details (waveform, note bars, labels, selection
// strokes, mute / record markings), so those stay readable on every colour. No neon.
// =============================================================================

#include "domain/TrackColour.h"

#include <juce_graphics/juce_graphics.h>

namespace track_colour_palette
{

/// Header segment fill (type icon + number) — a touch lighter than the event body so the segment
/// reads against both the inactive (0xff333333) and the active (0xff2a4a5a) header plate.
[[nodiscard]] inline juce::Colour headerSegmentFill(const TrackColourKey key) noexcept
{
    switch (key)
    {
    case TrackColourKey::Blue:
        return juce::Colour(0xff3e5a85);
    case TrackColourKey::Teal:
        return juce::Colour(0xff377572);
    case TrackColourKey::Green:
        return juce::Colour(0xff4a7445);
    case TrackColourKey::Ochre:
        return juce::Colour(0xff857436);
    case TrackColourKey::Orange:
        return juce::Colour(0xff945d30);
    case TrackColourKey::Red:
        return juce::Colour(0xff8a3f3f);
    case TrackColourKey::Purple:
        return juce::Colour(0xff684c82);
    case TrackColourKey::DefaultGrey:
    default:
        return juce::Colour(0xff484c54);
    }
}

/// Event body fill for the track's audio clips and MIDI clips. `DefaultGrey` returns the
/// historical unified body (`0xff343c4d`) byte-for-byte, so uncoloured projects look exactly as
/// before; the other keys are darker, desaturated variants of the segment colour.
[[nodiscard]] inline juce::Colour eventBodyFill(const TrackColourKey key) noexcept
{
    switch (key)
    {
    case TrackColourKey::Blue:
        return juce::Colour(0xff2f4466);
    case TrackColourKey::Teal:
        return juce::Colour(0xff2b5553);
    case TrackColourKey::Green:
        return juce::Colour(0xff385735);
    case TrackColourKey::Ochre:
        return juce::Colour(0xff5e532b);
    case TrackColourKey::Orange:
        return juce::Colour(0xff664427);
    case TrackColourKey::Red:
        return juce::Colour(0xff5e2f2f);
    case TrackColourKey::Purple:
        return juce::Colour(0xff4a385e);
    case TrackColourKey::DefaultGrey:
    default:
        return juce::Colour(0xff343c4d);
    }
}

/// The swatch shown in the palette menu (the segment colour).
[[nodiscard]] inline juce::Colour menuSwatch(const TrackColourKey key) noexcept
{
    return headerSegmentFill(key);
}

[[nodiscard]] inline juce::String displayName(const TrackColourKey key)
{
    switch (key)
    {
    case TrackColourKey::Blue:
        return "Blue";
    case TrackColourKey::Teal:
        return "Teal";
    case TrackColourKey::Green:
        return "Green";
    case TrackColourKey::Ochre:
        return "Ochre";
    case TrackColourKey::Orange:
        return "Orange";
    case TrackColourKey::Red:
        return "Red";
    case TrackColourKey::Purple:
        return "Purple";
    case TrackColourKey::DefaultGrey:
    default:
        return "Default grey";
    }
}

/// Icon / number ink on the segment: light on every palette colour (all segment fills are dark).
[[nodiscard]] inline juce::Colour segmentInk() noexcept
{
    return juce::Colour(0xffe8ebf0);
}

} // namespace track_colour_palette
