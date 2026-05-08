#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
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

/// Musical duration of the step grid in **seconds** (all steps at current BPM / denom).
[[nodiscard]] inline double experimentalPatternMusicalDurationSec(
    const ExperimentalMidiPattern& p) noexcept
{
    return p.stepDurationMs() * (double)juce::jmax(1, p.numSteps) / 1000.0;
}

/// Clip length in samples from the musical grid (**numSteps**, **stepDenom**, **bpm**) at `sampleRate`.
/// Used when creating clips, after step/denom edits, and when loading legacy projects with missing length.
/// Do **not** call this on BPM-only changes if `lengthSamples` must stay locked (I3d1).
[[nodiscard]] inline std::int64_t experimentalPatternMusicalLengthSamples(
    const ExperimentalMidiPattern& p,
    double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || !std::isfinite(sampleRate))
    {
        return 0;
    }
    const double sec = experimentalPatternMusicalDurationSec(p);
    if (!std::isfinite(sec) || sec <= 0.0)
    {
        return 0;
    }
    return (std::int64_t)std::llround(sec * sampleRate);
}

/// Map a step index to a **sample offset inside the clip**, assuming the clip span is divided evenly
/// into `numSteps` time slices (timeline-locked length; independent of BPM once length is set).
[[nodiscard]] inline std::int64_t clipRelativeSampleAtStepCenter(
    const int step,
    const int numSteps,
    const std::int64_t lengthSamples) noexcept
{
    const int ns = juce::jmax(1, numSteps);
    const std::int64_t len = juce::jmax(std::int64_t{1}, lengthSamples);
    const double t = (double)step + 0.5;
    return (std::int64_t)std::llround(t * (double)len / (double)ns);
}

[[nodiscard]] inline std::int64_t absoluteSampleForNoteInClip(
    const std::int64_t clipStartSamples,
    const int step,
    const int numSteps,
    const std::int64_t lengthSamples) noexcept
{
    return clipStartSamples + clipRelativeSampleAtStepCenter(step, numSteps, lengthSamples);
}
