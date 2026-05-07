#pragma once

#include <algorithm>
#include <vector>

#include <juce_core/juce_core.h>

/// I2 prototype: one MIDI hit in the step grid. Real identity is midiNote + step.
/// lengthSteps is reserved for a future Piano mode (duration bars); Drum hits mode ignores it.
struct PrototypeMidiNote
{
    int midiNote = 60;
    int step = 0;
    int lengthSteps = 1;
    int velocity = 100;
};

/// Step sequencer pattern (in-memory only; no Session / ProjectFile).
struct ExperimentalMidiPattern
{
    int numSteps = 16;
    int stepDenom = 16;
    double bpm = 110.0;
    bool loop = true;
    std::vector<PrototypeMidiNote> notes;

    [[nodiscard]] double stepDurationMs() const noexcept
    {
        if (bpm <= 0.0 || stepDenom <= 0)
        {
            return 136.0;
        }
        return 60000.0 / bpm * (4.0 / (double)stepDenom);
    }

    [[nodiscard]] double loopDurationMs() const noexcept
    {
        return stepDurationMs() * (double)juce::jmax(1, numSteps);
    }

    [[nodiscard]] std::vector<PrototypeMidiNote>::iterator findHit(const int midiNote, const int step)
    {
        return std::find_if(
            notes.begin(), notes.end(), [midiNote, step](const PrototypeMidiNote& n) {
                return n.midiNote == midiNote && n.step == step;
            });
    }

    [[nodiscard]] std::vector<PrototypeMidiNote>::const_iterator findHit(const int midiNote,
                                                                         const int step) const
    {
        return std::find_if(
            notes.cbegin(), notes.cend(), [midiNote, step](const PrototypeMidiNote& n) {
                return n.midiNote == midiNote && n.step == step;
            });
    }

    void toggleHit(const int midiNote, const int step)
    {
        const auto it = findHit(midiNote, step);
        if (it != notes.end())
        {
            notes.erase(it);
            return;
        }
        PrototypeMidiNote n;
        n.midiNote = midiNote;
        n.step = step;
        n.lengthSteps = 1;
        n.velocity = 100;
        notes.push_back(n);
    }
};
