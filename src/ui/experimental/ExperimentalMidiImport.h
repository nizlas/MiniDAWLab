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
// Timing for this MVP: scan all tempo markers; adopt the **first** chronologically only; BPM =
// `60 / seconds-per-quarter`; if multiple markers exist append a plain-language warning — no silent
// multi-tempo emulation.
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
    double firstTempoBpm = 0.0;

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
/// Sets `firstTempoBpm > 0` when at least one tempo meta event existed.
[[nodiscard]] ExperimentalMidiImportResult experimentalImportMidiFile(
    const juce::File& file,
    int internalTicksPerQuarter = kDefaultExperimentalTicksPerQuarter);
