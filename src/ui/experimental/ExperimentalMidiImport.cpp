// =============================================================================
// ExperimentalMidiImport.cpp — MIDI file → TimelineMidiNote (GUI thread import)
//
// `startTick` values are scaled from Standard MIDI PPQ timestamps into internal PPQ (**no trim** of leading
// silence): tick 0 in the file remains tick 0 for the clip, so drums can land late inside the MIDI event
// while `clip.startSamples` stays anchored with the mixdown timeline.
//
// SMPTE‑encoded MIDI (`getTimeFormat() <= 0`) is refused with an explicit message.
//
// Tempo meta events are ignored entirely: imported clips always play at the project tempo (ticks are
// musical positions, so bar/beat placement is preserved). Multi-tempo files surface `warningMessage`.
// =============================================================================

#include "ExperimentalMidiImport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "ExperimentalMidiPatternPlayer.h"

namespace
{
    struct FlatEvent
    {
        double fileTick = 0.0;
        int trackIndex = 0;
        int evInTrack = 0;
        juce::MidiMessage message;
    };

    struct PendingOn
    {
        std::int64_t internalTick = 0;
        int velocity = 100;
        int midiNote = 0;
        std::uint8_t channel = 1;
    };

    [[nodiscard]] std::int64_t rescaleTicks(const double fileTick,
                                            const double fileTpq,
                                            const int internalTpq) noexcept
    {
        if (internalTpq <= 0 || fileTpq <= 0.0)
        {
            return 0;
        }
        return (std::int64_t)std::llround(fileTick * (double)internalTpq / fileTpq);
    }
} // namespace

ExperimentalMidiImportResult experimentalImportMidiFile(const juce::File& file, const int internalTpqIn)
{
    ExperimentalMidiImportResult out;
    const int internalTpq = juce::jmax(1, internalTpqIn);

    if (!file.existsAsFile())
    {
        out.errorMessage = "The MIDI file could not be read (missing on disk).";
        return out;
    }

    const std::unique_ptr<juce::FileInputStream> in(file.createInputStream());
    if (in == nullptr || !in->openedOk())
    {
        out.errorMessage = "Could not open MIDI file for reading.";
        return out;
    }

    juce::MidiFile midiFile;
    if (!midiFile.readFrom(*in))
    {
        out.errorMessage = "This file is not recognized as valid Standard MIDI.";
        return out;
    }

    const auto timeFmt = midiFile.getTimeFormat();
    if (timeFmt <= 0)
    {
        out.errorMessage = "This MIDI file uses SMPTE time format. MiniDAWLab (this build) only imports "
                           "PPQ/tick‑based MIDI — re‑export from Cubase with pulses‑per‑quarter timing.";
        return out;
    }

    const double fileTpq = (double)(int)timeFmt;

    std::vector<FlatEvent> flat;
    const int nt = midiFile.getNumTracks();
    for (int trackIndex = 0; trackIndex < nt; ++trackIndex)
    {
        auto* seq = midiFile.getTrack(trackIndex);
        if (seq == nullptr)
        {
            continue;
        }
        const int ne = seq->getNumEvents();
        for (int ev = 0; ev < ne; ++ev)
        {
            flat.push_back({ seq->getEventTime(ev), trackIndex, ev, seq->getEventPointer(ev)->message });
        }
    }

    std::stable_sort(flat.begin(), flat.end(),
                     [](const FlatEvent& a, const FlatEvent& b) noexcept {
                         if (a.fileTick != b.fileTick)
                         {
                             return a.fileTick < b.fileTick;
                         }
                         if (a.trackIndex != b.trackIndex)
                         {
                             return a.trackIndex < b.trackIndex;
                         }
                         return a.evInTrack < b.evInTrack;
                     });

    // Tempo markers are ignored for playback (clips adopt the project tempo); multi-tempo files still
    // get a user-visible note because expressive tempo ramps from the source are not reproduced.
    std::vector<std::pair<double, double>> temposSorted;
    temposSorted.reserve(static_cast<size_t>(flat.size()));
    for (const FlatEvent& fe : flat)
    {
        if (!fe.message.isTempoMetaEvent())
        {
            continue;
        }
        const double spq = fe.message.getTempoSecondsPerQuarterNote();
        if (spq <= 1e-9 || !std::isfinite(spq) || spq > 900.0)
        {
            continue;
        }
        temposSorted.push_back({ fe.fileTick, spq });
    }
    std::stable_sort(temposSorted.begin(), temposSorted.end(),
                     [](const auto& u, const auto& v) { return u.first < v.first; });
    temposSorted.erase(std::unique(temposSorted.begin(), temposSorted.end(),
                                   [](const auto& u, const auto& v) {
                                       return std::fabs(u.first - v.first) < 1e-9
                                              && std::fabs(u.second - v.second) < 1e-12;
                                   }),
                       temposSorted.end());

    if ((int) temposSorted.size() > 1)
    {
        out.warningMessage
            = "This MIDI file contains multiple tempo changes. MiniDAWLab plays imported notes at the "
              "project tempo — the file's tempo changes are ignored (notes keep their bar/beat positions).";
    }

    // Note-on / note-off pairing with per-(channel,key) stacks (handles overlapping voices crudely LIFO).
    std::unordered_map<int, std::vector<PendingOn>> openStacks;
    constexpr auto stackKeyFn = [](const std::uint8_t chMidi, const int kk) noexcept {
        const int cz = juce::jlimit(0, 15, (int)chMidi - 1);
        const int nk = juce::jlimit(0, 127, kk);
        return (cz << 8) | nk;
    };

    for (const FlatEvent& fe : flat)
    {
        const juce::MidiMessage& mm = fe.message;

        bool isOn = mm.isNoteOn();
        bool isOff = mm.isNoteOff();
        const int velZeroAsOff = mm.isNoteOn() && mm.getVelocity() == 0;
        if (!isOff && !(isOn || velZeroAsOff))
        {
            continue;
        }

        const std::uint8_t chMidi = static_cast<std::uint8_t>(mm.getChannel());
        const int noteNum = juce::jlimit(0, 127, mm.getNoteNumber());
        const int stk = stackKeyFn(chMidi, noteNum);
        const std::int64_t itick = rescaleTicks(fe.fileTick, fileTpq, internalTpq);

        if (mm.isNoteOn() && mm.getVelocity() > 0)
        {
            PendingOn on;
            on.internalTick = itick;
            on.velocity = mm.getVelocity();
            on.midiNote = noteNum;
            on.channel = chMidi;
            openStacks[stk].push_back(std::move(on));
            continue;
        }

        auto& stkVec = openStacks[stk];
        if (stkVec.empty())
        {
            continue;
        }
        PendingOn from = stkVec.back();
        stkVec.pop_back();

        TimelineMidiNote tn;
        tn.midiNote = from.midiNote;
        tn.velocity = juce::jlimit(1, 127, from.velocity);
        tn.channel = from.channel == 0 ? chMidi : from.channel;
        tn.startTick = from.internalTick;
        const std::int64_t len = juce::jmax<std::int64_t>(1, itick - from.internalTick);
        tn.durationTicks = len;
        out.notes.push_back(tn);
    }

    constexpr std::int64_t danglingDefaultDur = 240;
    for (auto& kv : openStacks)
    {
        auto& stk = kv.second;
        while (!stk.empty())
        {
            PendingOn from = stk.back();
            stk.pop_back();
            TimelineMidiNote tn;
            tn.midiNote = from.midiNote;
            tn.velocity = juce::jlimit(1, 127, from.velocity);
            tn.channel = from.channel;
            tn.startTick = from.internalTick;
            tn.durationTicks = danglingDefaultDur;
            out.notes.push_back(tn);
        }
    }

    std::sort(out.notes.begin(), out.notes.end(),
              [](const TimelineMidiNote& a, const TimelineMidiNote& b) noexcept {
                  if (a.startTick != b.startTick)
                  {
                      return a.startTick < b.startTick;
                  }
                  if (a.midiNote != b.midiNote)
                  {
                      return a.midiNote < b.midiNote;
                  }
                  return a.channel < b.channel;
              });

    if (out.notes.empty())
    {
        out.warningMessage.clear();
        out.errorMessage
            = "No note events were found in this MIDI file (only controller/meta data, or an empty export).";
        return out;
    }

    ExperimentalMidiPatternPlayer::writeMidiEditorLogLine("midi-import: ok notes="
                                                        + juce::String((int)out.notes.size()) + " file=\""
                                                        + file.getFullPathName().substring(0, 120) + "\"");
    out.ok = true;
    return out;
}