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

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

struct InstrumentMidiClip;

/// Result of `exportInstrumentMidiClipToMidiFile` (paths, counts, single-line error for UI/log).
struct InstrumentMidiClipExportResult
{
    bool ok = false;
    juce::String errorMessage;
    int notesExported = 0;
    int ccEventsExported = 0;
};

/// Builds the SMF1 in memory (Stage C seam: unit tests parse the produced `juce::MidiFile`
/// without touching disk). Applies the **export channel contract** to every channel-voice event:
/// `sourceTrackMidiOutputChannel == kTrackMidiOutputChannelAny` exports each event's native
/// channel; a fixed 1…16 exports that effective channel. The channel belongs to the **MIDI source
/// track** (Instrument or Midi row) — never to a `MIDI To` destination. Meta events (tempo, time
/// signature, track name) are never remapped. Reads only; the clip is never mutated.
[[nodiscard]] InstrumentMidiClipExportResult buildInstrumentMidiClipMidiFile(
    const InstrumentMidiClip& clip,
    int sourceTrackMidiOutputChannel,
    double deviceSampleRate,
    juce::MidiFile& outMidiFile);

/// Write `clip` to `outputFile` (.mid). `deviceSampleRate` should match timeline conversion
/// (`AudioDeviceManager::getCurrentAudioDevice()->getCurrentSampleRate()` or controller default).
/// `sourceTrackMidiOutputChannel` is the **source** track's `MIDI Channel` (`Any` or fixed 1…16);
/// see `buildInstrumentMidiClipMidiFile` for the channel contract.
[[nodiscard]] InstrumentMidiClipExportResult exportInstrumentMidiClipToMidiFile(
    const InstrumentMidiClip& clip,
    int sourceTrackMidiOutputChannel,
    const juce::File& outputFile,
    double deviceSampleRate);
