#pragma once

// =============================================================================
// InstrumentMidiClipExport — write one I3 instrument clip to Standard MIDI File 1
// =============================================================================
//
// Message-thread helper: converts the bound `InstrumentMidiClip` pattern into a **Type 1**
// multi-track SMF suitable for Cubase (tick/PPQ timing, track 0 = conductor, track 1 = named
// note data). Does **not** touch session `ProjectFile`, playback engine, or import paths.
//
// **Inclusion rule (timeline):** when `clip.lengthSamples > 0`, only note-ons inside the same sample
// window as `InstrumentTrackController::publishRenderSnapshot` are exported. **Legacy step patterns**
// match that snapshot’s step path: valid steps export with no extra sample window filter.
// When `lengthSamples <= 0` on a timeline clip, no sample filter is applied.
//
// JUCE note: `MidiFile::writeTo` encodes each message’s `MidiMessage::getTimeStamp()` as **delta
// ticks**; stamps must be quarter-note ticks matching `MidiFile::setTicksPerQuarterNote` (the clip’s
// `pattern.ticksPerQuarter`, usually 960).
// =============================================================================

#include <juce_core/juce_core.h>

struct InstrumentMidiClip;

/// Result of `exportInstrumentMidiClipToMidiFile` (paths, counts, single-line error for UI/log).
struct InstrumentMidiClipExportResult
{
    bool ok = false;
    juce::String errorMessage;
    int notesExported = 0;
};

/// Write `clip` to `outputFile` (.mid). `deviceSampleRate` should match timeline conversion
/// (`AudioDeviceManager::getCurrentAudioDevice()->getCurrentSampleRate()` or controller default).
[[nodiscard]] InstrumentMidiClipExportResult exportInstrumentMidiClipToMidiFile(
    const InstrumentMidiClip& clip,
    const juce::File& outputFile,
    double deviceSampleRate);
