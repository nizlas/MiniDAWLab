#pragma once

// =============================================================================
// ExperimentalMidiImport — .mid/.midi parsing for InstrumentMidiClip timeline notes (message thread)
// =============================================================================
//
// Reads Standard MIDI Files into `TimelineMidiNote` using `juce::MidiFile`:
// PPQ/time division from file header (`getTimeFormat() > 0`); SMPTE (`<= 0`) is rejected loudly.
//
// **`startTick` preserves file timeline position from tick zero onward** — we never subtract an
// “earliest drum hit offset”, so introductory silence exported from Cubase stays aligned against a
// full-length audio mixdown (`clip.startSamples` still aligns the MIDI event shell on the timeline).
//
// Timing: the file's tempo meta events are **ignored** — imported clips always play at the project
// tempo (note ticks are musical positions, so bar/beat placement is preserved exactly). Files with
// multiple tempo changes get a plain-language warning that expressive tempo ramps are not reproduced.
//
// Threading: call from the GUI thread during import dialogs only — not realtime.
// =============================================================================

#include <cstdint>

#include <vector>

#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_core/juce_core.h>

/// Outcome bundle for MIDI import diagnostics (shown in UI + optional experimental log append).
struct ExperimentalMidiImportResult
{
    bool ok = false;
    juce::String errorMessage;
    /// Secondary message (typically multi-tempo or minor parse caveats).
    juce::String warningMessage;

    std::vector<TimelineMidiNote> notes;

    [[nodiscard]] juce::String combinedUserMessageLine() const
    {
        juce::String s;
        if (errorMessage.isNotEmpty())
        {
            s << errorMessage;
        }
        if (warningMessage.isNotEmpty())
        {
            if (s.isNotEmpty())
            {
                s << "\n\n";
            }
            s << warningMessage;
        }
        return s;
    }
};

/// Parses `file` Standard MIDI (.mid/.midi).
/// SMPTE‑encoded files (`timeFormat<=0`): error set, notes empty.
/// Tempo meta events are ignored (clips adopt the project tempo); multi-tempo files set `warningMessage`.
[[nodiscard]] ExperimentalMidiImportResult experimentalImportMidiFile(
    const juce::File& file,
    int internalTicksPerQuarter = kDefaultExperimentalTicksPerQuarter);
