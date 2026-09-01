#pragma once

// =============================================================================
// MidiEditorTitleStatus — window title + instrument/destination status, as text
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Pure text builders behind the MIDI editor's window title and toolbar status label
//   (Phase B.1). No components, no session access: callers pass what they already resolved
//   (track name, kind, destination state) and get back the exact user-facing strings, so the
//   wording is unit-testable without a window (`tests/selftest/`).
//
// WORDING RULES (from the Phase B.1 steer)
//   * Title: `MIDI Editor — <track name>` for Instrument and MIDI tracks alike. No internal
//     labels ("I2"), no legacy "(Drum hits)", no Piano/Drum row-mode description.
//   * Instrument-track status: the track's *own* plugin state ("Instrument: VB3-II" /
//     "No instrument loaded").
//   * MIDI-track status: the resolved `MIDI To` destination, never the source's (nonexistent)
//     plugin: "MIDI To: Organ — VB3-II", "No MIDI destination", or "Organ: No instrument loaded".
//
// THREADING
//   Pure functions; used from the [Message thread] only in practice.
// =============================================================================

#include <juce_core/juce_core.h>

namespace midi_editor_text
{
    /// `"MIDI Editor — Organ Pedal"`; bare `"MIDI Editor"` when no track is bound (scratch mode).
    [[nodiscard]] inline juce::String buildWindowTitle(const juce::String& trackName)
    {
        const juce::String emDash = juce::String::fromUTF8(" \xe2\x80\x94 ");
        return trackName.isNotEmpty() ? juce::String("MIDI Editor") + emDash + trackName
                                      : juce::String("MIDI Editor");
    }

    /// What the status line needs to know about the editor's bound row, pre-resolved by the
    /// caller on the message thread (TrackId-backed; never names or row indices).
    struct BoundTrackStatus
    {
        /// True for a `TrackKind::Midi` row (no plugin of its own; status follows `MIDI To`).
        bool isMidiTrack = false;
        /// Instrument rows: this row's plugin. Midi rows: the routed *destination's* plugin.
        bool instrumentLoaded = false;
        /// Display name of that plugin (empty when unknown even though loaded).
        juce::String instrumentName;
        /// Midi rows only: destination row resolved from the stored `MIDI To` TrackId.
        bool hasDestination = false;
        juce::String destinationTrackName;
    };

    /// The status label's first line. Power/mute overrides are appended by the caller, which
    /// owns those checks.
    [[nodiscard]] inline juce::String buildInstrumentStatusLine(const BoundTrackStatus& s)
    {
        const juce::String emDash = juce::String::fromUTF8(" \xe2\x80\x94 ");
        if (!s.isMidiTrack)
        {
            if (!s.instrumentLoaded)
            {
                return "No instrument loaded";
            }
            return s.instrumentName.isNotEmpty() ? juce::String("Instrument: ") + s.instrumentName
                                                 : juce::String("Instrument: (loaded)");
        }
        if (!s.hasDestination)
        {
            return "No MIDI destination";
        }
        if (!s.instrumentLoaded)
        {
            // The destination row exists but owns no plugin — name the destination so this can
            // never read as "the MIDI track should load an instrument".
            return s.destinationTrackName.isNotEmpty()
                       ? s.destinationTrackName + ": No instrument loaded"
                       : juce::String("MIDI destination: No instrument loaded");
        }
        const juce::String inst = s.instrumentName.isNotEmpty() ? s.instrumentName
                                                                : juce::String("(loaded)");
        return s.destinationTrackName.isNotEmpty()
                   ? juce::String("MIDI To: ") + s.destinationTrackName + emDash + inst
                   : juce::String("MIDI To: ") + inst;
    }
} // namespace midi_editor_text
