#pragma once

// =============================================================================
// ExperimentalMidiPattern — dual representation for drum clips (prototype + timeline)
// =============================================================================
//
// **Prototype path (legacy):** `notes` keyed by discrete `step`; transport maps steps onto
// `clip.lengthSamples` evenly. Used for older projects and scratch patterns.
//
// **Timeline path (I3f):** `timelineNotes` store **ticks since the clip’s MIDI time zero** (tick 0).
// Timing is authoritative when `timelineNotes` is non-empty: transport, paint, and editing use
// tick→sample conversion with `bpm`, `ticksPerQuarter`, and the device timeline sample rate.
// Cubase‑exported files often have **silent space before the first drum hit**: tick positions are
// **not shifted** toward the earliest note—we preserve file‑relative ticks so syncing against a full
// mixdown remains straightforward. On the session timeline, tick 0 maps to
// `InstrumentMidiClip::timelineAnchorSamples`; `startSamples` / `lengthSamples` are only the visible window.
//
// Internal resolution is **`kDefaultExperimentalTicksPerQuarter` (960)**; imported file PPQ from the MIDI
// header is rescaled into this domain (rounded with `std::llround`).
//
// Threading: message thread unless stated otherwise — not realtime.
// =============================================================================

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

/// I3f: editable note in PPQ‑tick clock time **relative to the clip’s MIDI file position 0** (song start).
/// `startTick` may be arbitrarily large — leading silence before the first hit is intentional and preserved.
/// `durationTicks` is stored for future editing; drums still use fixed transport gate samples.
struct TimelineMidiNote
{
    int midiNote = 60;
    /// 1 … 127 stored for realism; clipped at playback scheduling.
    int velocity = 100;
    /// 1 … 16 (shown on channel messages in the MIDI file).
    std::uint8_t channel = 1;
    /// Ticks since MIDI time zero for this clip, in `ticksPerQuarter` domain (usually 960).
    std::int64_t startTick = 0;
    /// Held note length for future MIDI editing; drums ignore fixed gate separately.
    std::int64_t durationTicks = 240;
};

/// Default internal Musical Time resolution (ticks per quarter note).
inline constexpr int kDefaultExperimentalTicksPerQuarter = 960;

/// Step sequencer pattern (in-memory).
struct ExperimentalMidiPattern
{
    int numSteps = 16;
    int stepDenom = 16;
    double bpm = 110.0;
    bool loop = true;

    std::vector<PrototypeMidiNote> notes;

    /// When non-empty this list is authoritative: transport/editing/paint skip `notes`.
    std::vector<TimelineMidiNote> timelineNotes;
    /// Ticks per quarter for `timelineNotes` (normally 960; saved with clip).
    int ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
    /// Explicit timeline-note mode for clips created empty (no notes yet). Keeps lane trim/move and
    /// piano-roll timeline editing available before the first note exists. Persisted with the clip.
    bool timelineMode = false;

    [[nodiscard]] bool usesTimelineNotes() const noexcept { return timelineMode || !timelineNotes.empty(); }

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

[[nodiscard]] inline int experimentalEffectiveTicksPerQuarter(const ExperimentalMidiPattern& p) noexcept
{
    return juce::jmax(1, p.ticksPerQuarter);
}

/// Clip length for **timeline** clips: wall‑clock extent from tick 0 to the end of the last note + padTicks.
[[nodiscard]] inline std::int64_t timelinePatternExtentInclusiveEndTick(
    const ExperimentalMidiPattern& p,
    const std::int64_t padTicks) noexcept
{
    std::int64_t endTick = 0;
    for (const auto& n : p.timelineNotes)
    {
        const std::int64_t hi = n.startTick + juce::jmax<std::int64_t>(1, n.durationTicks);
        endTick = juce::jmax(endTick, hi);
    }
    endTick += juce::jmax<std::int64_t>(0, padTicks);
    return juce::jmax<std::int64_t>(1, endTick);
}

[[nodiscard]] inline std::int64_t ticksToRelativeSamples(
    const std::int64_t ticks,
    const double bpm,
    const int ticksPerQuarter,
    const double sampleRate) noexcept
{
    const double beats = static_cast<double>(ticks) / (double)juce::jmax(1, ticksPerQuarter);
    if (bpm <= 0.0 || !std::isfinite(bpm) || sampleRate <= 0.0 || !std::isfinite(sampleRate))
    {
        return 0;
    }
    const double secondsPerBeat = 60.0 / bpm;
    const double sec = beats * secondsPerBeat;
    if (!std::isfinite(sec) || sec <= 0.0)
    {
        return 0;
    }
    return (std::int64_t)std::llround(sec * sampleRate);
}

/// Signed sample offset for bidirectional tick→time mapping (negative ticks = before tick zero).
/// `ticksToRelativeSamples` clamps non-positive **seconds** to 0 and cannot represent pre‑tick‑zero grid lines.
[[nodiscard]] inline std::int64_t ticksToSignedSamples(const std::int64_t ticks,
                                                       const double bpm,
                                                       const int ticksPerQuarter,
                                                       const double sampleRate) noexcept
{
    const double beats = static_cast<double>(ticks) / (double)juce::jmax(1, ticksPerQuarter);
    if (bpm <= 0.0 || !std::isfinite(bpm) || sampleRate <= 0.0 || !std::isfinite(sampleRate))
    {
        return 0;
    }
    const double secondsPerBeat = 60.0 / bpm;
    const double sec = beats * secondsPerBeat;
    if (!std::isfinite(sec))
    {
        return 0;
    }
    return (std::int64_t)std::llround(sec * sampleRate);
}

[[nodiscard]] inline std::int64_t relativeSamplesToTicks(const std::int64_t relativeSamples,
                                                       const double bpm,
                                                       const int ticksPerQuarter,
                                                       const double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || !std::isfinite(sampleRate) || bpm <= 0.0 || !std::isfinite(bpm))
    {
        return 0;
    }
    const double beats = relativeSamples / sampleRate / (60.0 / bpm);
    const double ticksD = beats * (double)juce::jmax(1, ticksPerQuarter);
    return (std::int64_t)std::llround(ticksD);
}

/// Approximate milliseconds spanned by `ticks` under constant tempo.
[[nodiscard]] inline double timelineTicksToMilliseconds(const std::int64_t ticks,
                                                        const double bpm,
                                                        const int ticksPerQuarter) noexcept
{
    const double beats = static_cast<double>(ticks) / (double)juce::jmax(1, ticksPerQuarter);
    if (bpm <= 0.0 || !std::isfinite(bpm))
    {
        return 0.0;
    }
    return beats * (60000.0 / bpm);
}

/// Snapshot duration for timeline preview (~last note tail + minimal pad bar).
[[nodiscard]] inline double experimentalTimelinePreviewDurationMs(
    const ExperimentalMidiPattern& p) noexcept
{
    if (p.timelineNotes.empty())
    {
        return p.loopDurationMs();
    }
    const std::int64_t tpq = experimentalEffectiveTicksPerQuarter(p);
    const std::int64_t extent = timelinePatternExtentInclusiveEndTick(
        p, juce::jmax<std::int64_t>(1, tpq / 8));
    const double dur = timelineTicksToMilliseconds(extent, p.bpm > 0.0 ? p.bpm : 120.0, (int)tpq);
    return juce::jmax(80.0, dur);
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

[[nodiscard]] inline std::int64_t timelinePatternLengthSamples(
    const ExperimentalMidiPattern& p,
    const double sampleRate) noexcept
{
    const int tpq = experimentalEffectiveTicksPerQuarter(p);
    const std::int64_t extent = timelinePatternExtentInclusiveEndTick(
        p,
        /// ~1 beat tail so the scheduler still sees `[start, endExclusive)` cleanly at end of groove.
        (std::int64_t)(tpq / 16));
    return ticksToRelativeSamples(extent, p.bpm > 0.0 ? p.bpm : 120.0, tpq, sampleRate);
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

[[nodiscard]] inline std::int64_t absoluteSampleForTimelineNote(const std::int64_t timelineAnchorSamplesForTickZero,
                                                                const TimelineMidiNote& n,
                                                                const ExperimentalMidiPattern& p,
                                                                double sampleRate) noexcept
{
    const int tpq = experimentalEffectiveTicksPerQuarter(p);
    return timelineAnchorSamplesForTickZero
           + ticksToRelativeSamples(n.startTick, p.bpm > 0.0 ? p.bpm : 120.0, tpq, sampleRate);
}
