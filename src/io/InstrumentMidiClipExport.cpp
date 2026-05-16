// =============================================================================
// InstrumentMidiClipExport.cpp — SMF1 writer for a single `InstrumentMidiClip`
// =============================================================================

#include "io/InstrumentMidiClipExport.h"

#include "instruments/InstrumentTrackController.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
    /// One note worth of absolute PPQ ticks (clip time origin = tick 0).
    struct ExportNoteTick
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int midiNote = 60;
        int velocity = 100;
        int channel = 1;
    };

    /// Groove-Agent transport gate expressed as samples — kept in sync with
    /// `InstrumentTrackController::publishRenderSnapshot` for step/legacy export duration.
    [[nodiscard]] int controllerStyleGateSamples(const double sampleRate) noexcept
    {
        double sr = sampleRate;
        if (sr <= 0.0 || !std::isfinite(sr))
        {
            sr = 48000.0;
        }
        return juce::jmax(1, (int)std::llround(0.001 * 100.0 * sr));
    }

    /// Collect timeline or step notes with transport-matched inclusion and minimum one-tick length.
    void collectNotesForExport(const InstrumentMidiClip& clip,
                               const int tpq,
                               const double bpm,
                               const double sampleRate,
                               std::vector<ExportNoteTick>& out)
    {
        out.clear();
        const auto& pat = clip.pattern;
        const bool filterToClipSamples = clip.lengthSamples > 0;
        const std::int64_t clipEndEx = clip.startSamples + clip.lengthSamples;

        if (pat.usesTimelineNotes())
        {
            for (const auto& tn : pat.timelineNotes)
            {
                if (tn.startTick < 0)
                {
                    continue;
                }
                if (filterToClipSamples)
                {
                    const std::int64_t absS =
                        absoluteSampleForTimelineNote(clip.timelineAnchorSamples, tn, pat, sampleRate);
                    if (absS < clip.startSamples || absS >= clipEndEx)
                    {
                        continue;
                    }
                }

                ExportNoteTick e;
                e.midiNote = juce::jlimit(0, 127, tn.midiNote);
                e.velocity = juce::jlimit(1, 127, tn.velocity);
                e.channel = juce::jlimit(1, 16, (int)tn.channel);
                e.startTick = tn.startTick;
                std::int64_t offTick = e.startTick + juce::jmax<std::int64_t>(1, tn.durationTicks);
                if (offTick <= e.startTick)
                {
                    offTick = e.startTick + 1;
                }
                e.endTick = offTick;
                out.push_back(e);
            }
        }
        else
        {
            const int ns = juce::jmax(1, pat.numSteps);
            const std::int64_t lengthForStepPlacement =
                clip.lengthSamples > 0
                    ? clip.lengthSamples
                    : juce::jmax<std::int64_t>(1, experimentalPatternMusicalLengthSamples(pat, sampleRate));

            const int gateSamples = controllerStyleGateSamples(sampleRate);
            const std::int64_t durationTicks =
                juce::jmax<std::int64_t>(1, relativeSamplesToTicks(gateSamples, bpm, tpq, sampleRate));

            for (const auto& n : pat.notes)
            {
                if (n.step < 0 || n.step >= pat.numSteps)
                {
                    continue;
                }
                const std::int64_t absS =
                    absoluteSampleForNoteInClip(clip.startSamples, n.step, ns, lengthForStepPlacement);
                // Legacy step grid matches `publishRenderSnapshot`: no sample-window cull; only invalid
                // steps are skipped above.

                const std::int64_t relS = absS - clip.startSamples;
                const std::int64_t startTick = relativeSamplesToTicks(relS, bpm, tpq, sampleRate);

                ExportNoteTick e;
                e.midiNote = juce::jlimit(0, 127, n.midiNote);
                e.velocity = juce::jlimit(1, 127, n.velocity);
                e.channel = 10;
                e.startTick = startTick;
                e.endTick = startTick + durationTicks;
                if (e.endTick <= e.startTick)
                {
                    e.endTick = e.startTick + 1;
                }
                out.push_back(e);
            }
        }

        std::sort(out.begin(), out.end(), [](const ExportNoteTick& a, const ExportNoteTick& b) noexcept {
            if (a.startTick != b.startTick)
            {
                return a.startTick < b.startTick;
            }
            if (a.channel != b.channel)
            {
                return a.channel < b.channel;
            }
            return a.midiNote < b.midiNote;
        });
    }
} // namespace

InstrumentMidiClipExportResult exportInstrumentMidiClipToMidiFile(const InstrumentMidiClip& clip,
                                                                   const juce::File& outputFile,
                                                                   const double deviceSampleRate)
{
    InstrumentMidiClipExportResult result;

    juce::File dest = outputFile;
    if (dest.getFileExtension().isEmpty())
    {
        dest = dest.withFileExtension("mid");
    }

    juce::File parent = dest.getParentDirectory();
    if (!parent.exists())
    {
        result.errorMessage = "The save folder does not exist.";
        return result;
    }

    double sr = deviceSampleRate;
    if (sr <= 0.0 || !std::isfinite(sr))
    {
        sr = 48000.0;
    }

    const auto& pat = clip.pattern;
    const int tpq = experimentalEffectiveTicksPerQuarter(pat);
    const double bpm = pat.bpm > 0.0 && std::isfinite(pat.bpm) ? pat.bpm : 120.0;

    std::vector<ExportNoteTick> notes;
    collectNotesForExport(clip, tpq, bpm, sr, notes);

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(tpq);

    // Track 0: conductor — tempo + simple 4/4 for DAWs that expect a time signature.
    juce::MidiMessageSequence conductor;
    {
        const int usPerQuarter =
            juce::jlimit(1, 0x00ffffff, (int)std::llround(60000000.0 / bpm));
        auto tempo = juce::MidiMessage::tempoMetaEvent(usPerQuarter);
        tempo.setTimeStamp(0);
        conductor.addEvent(tempo);

        auto ts = juce::MidiMessage::timeSignatureMetaEvent(4, 4);
        ts.setTimeStamp(0);
        conductor.addEvent(ts);
    }
    conductor.updateMatchedPairs();
    conductor.sort();
    midiFile.addTrack(conductor);

    // Track 1: SMF meta type **0x03** track name + polyphonic note stream.
    juce::MidiMessageSequence noteTrack;
    {
        const juce::String trackName = clip.name.isNotEmpty() ? clip.name : juce::String("MIDI 1");
        // JUCE: `textMetaEvent(3, …)` emits the Standard MIDI **track name** meta (same type
        // byte Cubase and other hosts expect in track 1).
        auto trackNameMeta = juce::MidiMessage::textMetaEvent(3, trackName);
        trackNameMeta.setTimeStamp(0);
        noteTrack.addEvent(trackNameMeta);

        for (const auto& n : notes)
        {
            const int ch = juce::jlimit(1, 16, n.channel);
            auto on =
                juce::MidiMessage::noteOn(ch, n.midiNote, (juce::uint8)n.velocity).withTimeStamp(
                    (double)n.startTick);
            noteTrack.addEvent(on);
            auto off = juce::MidiMessage::noteOff(ch, n.midiNote, 0.0f).withTimeStamp((double)n.endTick);
            noteTrack.addEvent(off);
        }
    }
    noteTrack.updateMatchedPairs();
    noteTrack.sort();
    midiFile.addTrack(noteTrack);

    const std::unique_ptr<juce::OutputStream> stream(dest.createOutputStream());
    // `File::createOutputStream` returns null when the path cannot be opened for write (permissions,
    // bad path, etc.). The base `OutputStream` type has no `openedOk()` — that lives on `FileOutputStream`.
    if (stream == nullptr)
    {
        result.errorMessage = "Could not create the MIDI file for writing.";
        return result;
    }

    if (!midiFile.writeTo(*stream, 1))
    {
        result.errorMessage = "Writing Standard MIDI data failed (disk full or I/O error).";
        return result;
    }

    stream->flush();
    result.ok = true;
    result.notesExported = (int)notes.size();
    return result;
}
