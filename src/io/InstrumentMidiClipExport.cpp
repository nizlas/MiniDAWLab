// =============================================================================
// InstrumentMidiClipExport.cpp — SMF1 writer for a single `InstrumentMidiClip`
// =============================================================================

#include "io/InstrumentMidiClipExport.h"

#include "instruments/InstrumentTrackController.h"
#include "ui/experimental/ExperimentalMidiChannelDiagnostics.h"
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
        /// Release velocity written on the note's true Note Off event.
        int offVelocity = kDefaultMidiNoteOffVelocity;
        int channel = 1;
    };

    /// Collect timeline notes with transport-matched inclusion and minimum one-tick length.
    /// Stage C: the exported channel is the **effective** channel of the source track — the note's
    /// native channel under `Any (Preserve)`, the fixed 1…16 otherwise — so the file represents
    /// what the user heard. The stored notes are never touched (this builds copies).
    void collectNotesForExport(const InstrumentMidiClip& clip,
                               const int trackOutputChannel,
                               const int tpq,
                               const double bpm,
                               const double sampleRate,
                               std::vector<ExportNoteTick>& out)
    {
        juce::ignoreUnused(tpq, bpm);
        out.clear();
        const auto& pat = clip.pattern;
        const bool filterToClipSamples = clip.lengthSamples > 0;
        const std::int64_t clipEndEx = clip.startSamples + clip.lengthSamples;

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
            e.offVelocity = sanitizeMidiNoteOffVelocity(tn.offVelocity);
            e.channel = midi_channel_diag::effectiveChannel((int)tn.channel, trackOutputChannel);
            e.startTick = tn.startTick;
            std::int64_t offTick = e.startTick + juce::jmax<std::int64_t>(1, tn.durationTicks);
            if (offTick <= e.startTick)
            {
                offTick = e.startTick + 1;
            }
            e.endTick = offTick;
            out.push_back(e);
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

InstrumentMidiClipExportResult buildInstrumentMidiClipMidiFile(const InstrumentMidiClip& clip,
                                                                const int sourceTrackMidiOutputChannel,
                                                                const double deviceSampleRate,
                                                                juce::MidiFile& outMidiFile)
{
    InstrumentMidiClipExportResult result;

    double sr = deviceSampleRate;
    if (sr <= 0.0 || !std::isfinite(sr))
    {
        sr = 48000.0;
    }

    const auto& pat = clip.pattern;
    const int tpq = experimentalEffectiveTicksPerQuarter(pat);
    const double bpm = pat.bpm > 0.0 && std::isfinite(pat.bpm) ? pat.bpm : 120.0;

    std::vector<ExportNoteTick> notes;
    collectNotesForExport(clip, sourceTrackMidiOutputChannel, tpq, bpm, sr, notes);

    juce::MidiFile& midiFile = outMidiFile;
    midiFile.clear();
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

        // Stage D: CC automation as standard Control Change events. The evaluator emits the
        // bounded discrete set (sparse endpoints exact; Linear ramps produce at most one event
        // per crossed integer value). Channel contract matches notes: native under
        // `Any (Preserve)`, the effective fixed channel otherwise. CC events are added BEFORE
        // the notes so `MidiMessageSequence::sort` (stable) keeps a CC at a note's start tick
        // ahead of that Note On. The stored points are read-only here.
        {
            std::vector<MidiCcPoint> pts = clip.pattern.ccPoints;
            (void)midi_cc::normalizePoints(pts);
            std::int64_t lastTick = 0;
            for (const auto& p : pts)
            {
                lastTick = juce::jmax(lastTick, p.startTick);
            }
            int ccCount = 0;
            for (const auto& key : midi_cc::distinctStreams(pts))
            {
                std::vector<midi_cc::MidiCcEvent> evs;
                midi_cc::collectCcEventsInTickRange(pts, key.controller, key.channel, 0,
                                                    lastTick + 1, std::nullopt, evs);
                const int effCh = midi_channel_diag::effectiveChannel(
                    key.channel, sourceTrackMidiOutputChannel);
                for (const auto& e : evs)
                {
                    auto cc = juce::MidiMessage::controllerEvent(effCh, (int)e.controller,
                                                                 (int)e.value)
                                  .withTimeStamp((double)e.tick);
                    noteTrack.addEvent(cc);
                    ++ccCount;
                }
            }
            result.ccEventsExported = ccCount;
        }

        for (const auto& n : notes)
        {
            const int ch = juce::jlimit(1, 16, n.channel);
            auto on =
                juce::MidiMessage::noteOn(ch, n.midiNote, (juce::uint8)n.velocity).withTimeStamp(
                    (double)n.startTick);
            noteTrack.addEvent(on);
            // True Note Off (status 0x8n) with the stored release velocity — never the
            // "Note On velocity 0" shorthand, which cannot carry off velocity.
            auto off = juce::MidiMessage::noteOff(ch, n.midiNote, (juce::uint8)n.offVelocity)
                           .withTimeStamp((double)n.endTick);
            noteTrack.addEvent(off);
        }
    }
    noteTrack.updateMatchedPairs();
    noteTrack.sort();
    midiFile.addTrack(noteTrack);

    result.ok = true;
    result.notesExported = (int)notes.size();
    return result;
}

InstrumentMidiClipExportResult exportInstrumentMidiClipToMidiFile(const InstrumentMidiClip& clip,
                                                                   const int sourceTrackMidiOutputChannel,
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

    juce::MidiFile midiFile;
    result = buildInstrumentMidiClipMidiFile(clip, sourceTrackMidiOutputChannel, deviceSampleRate, midiFile);
    if (!result.ok)
    {
        return result;
    }
    result.ok = false; // proven again below once the bytes are on disk

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
    return result;
}
