#pragma once

// =============================================================================
// TrackColour — the per-track colour IDENTITY (domain / persistence side)
// =============================================================================
// A track carries one key from a fixed, compact palette (UI metadata on `Session`, outside
// `SessionSnapshot` like the Solo sets and the visual groups: changing a colour never publishes a
// snapshot and never touches the audio model). The actual ARGB values live in
// `ui/TrackColourPalette.h`; this header only names the keys and their project-file spelling.
// =============================================================================

#include <juce_core/juce_core.h>

#include <cstdint>

/// Fixed palette keys. `DefaultGrey` is the explicit "no colour" / reset choice and the value of
/// every track that has no stored colour (all projects saved before v28).
enum class TrackColourKey : std::uint8_t
{
    DefaultGrey = 0,
    Blue,
    Teal,
    Green,
    Ochre,
    Orange,
    Red,
    Purple,
};

inline constexpr int kTrackColourKeyCount = 8;

/// Project-file spelling (`tracks[].colour`, v28). The default is never written.
[[nodiscard]] inline juce::String trackColourPersistenceKey(const TrackColourKey key)
{
    switch (key)
    {
    case TrackColourKey::Blue:
        return "blue";
    case TrackColourKey::Teal:
        return "teal";
    case TrackColourKey::Green:
        return "green";
    case TrackColourKey::Ochre:
        return "ochre";
    case TrackColourKey::Orange:
        return "orange";
    case TrackColourKey::Red:
        return "red";
    case TrackColourKey::Purple:
        return "purple";
    case TrackColourKey::DefaultGrey:
    default:
        return "grey";
    }
}

/// Absent / empty / unknown spelling loads as `DefaultGrey` — never a read failure.
[[nodiscard]] inline TrackColourKey trackColourKeyFromPersistenceKey(const juce::String& key)
{
    const juce::String k = key.trim().toLowerCase();
    if (k == "blue")
    {
        return TrackColourKey::Blue;
    }
    if (k == "teal")
    {
        return TrackColourKey::Teal;
    }
    if (k == "green")
    {
        return TrackColourKey::Green;
    }
    if (k == "ochre")
    {
        return TrackColourKey::Ochre;
    }
    if (k == "orange")
    {
        return TrackColourKey::Orange;
    }
    if (k == "red")
    {
        return TrackColourKey::Red;
    }
    if (k == "purple")
    {
        return TrackColourKey::Purple;
    }
    return TrackColourKey::DefaultGrey;
}

[[nodiscard]] inline TrackColourKey trackColourKeyFromIndex(const int index) noexcept
{
    return (index >= 0 && index < kTrackColourKeyCount) ? static_cast<TrackColourKey>(index)
                                                        : TrackColourKey::DefaultGrey;
}
