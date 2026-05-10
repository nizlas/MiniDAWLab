#pragma once

// =============================================================================
// DrumNoteNames — General MIDI percussion names (display only)
// =============================================================================
// MIDI note numbers 35–81 per the GM level 1 percussion map. Returns an empty
// string outside that range.
// =============================================================================

#include <juce_core/juce_core.h>

namespace drum_note_names
{

[[nodiscard]] inline juce::String gmPercussionName(const int midiNote) noexcept
{
    // Index 0 = MIDI 35 … index 46 = MIDI 81
    static const char* const kNames[47] = {
        "Acoustic Bass Drum",
        "Bass Drum 1",
        "Side Stick",
        "Acoustic Snare",
        "Hand Clap",
        "Electric Snare",
        "Low Floor Tom",
        "Closed Hi-Hat",
        "High Floor Tom",
        "Pedal Hi-Hat",
        "Low Tom",
        "Open Hi-Hat",
        "Low-Mid Tom",
        "Hi-Mid Tom",
        "Crash Cymbal 1",
        "High Tom",
        "Ride Cymbal 1",
        "Chinese Cymbal",
        "Ride Bell",
        "Tambourine",
        "Splash Cymbal",
        "Cowbell",
        "Crash Cymbal 2",
        "Vibraslap",
        "Ride Cymbal 2",
        "Hi Bongo",
        "Low Bongo",
        "Mute Hi Conga",
        "Open Hi Conga",
        "Low Conga",
        "High Timbale",
        "Low Timbale",
        "High Agogo",
        "Low Agogo",
        "Cabasa",
        "Maracas",
        "Short Whistle",
        "Long Whistle",
        "Short Guiro",
        "Long Guiro",
        "Claves",
        "Hi Wood Block",
        "Low Wood Block",
        "Mute Cuica",
        "Open Cuica",
        "Mute Triangle",
        "Open Triangle",
    };
    if (midiNote < 35 || midiNote > 81)
    {
        return {};
    }
    return juce::String(kNames[midiNote - 35]);
}

} // namespace drum_note_names
