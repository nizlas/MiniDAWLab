#pragma once

// =============================================================================
// ExperimentalMidiPattern — timeline-note clip pattern
// =============================================================================
//
// (The original step-sequencer prototype representation — `PrototypeMidiNote` keyed by discrete
// step — was removed; all clips are timeline clips.)
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

/// Note-off velocity used for new notes and for note data that predates off-velocity support.
inline constexpr int kDefaultMidiNoteOffVelocity = 64;

/// Clamp any loaded/edited note-off velocity into the MIDI-valid range.
[[nodiscard]] inline int sanitizeMidiNoteOffVelocity(const int v) noexcept
{
    return juce::jlimit(0, 127, v);
}

/// I3f: editable note in PPQ‑tick clock time **relative to the clip’s MIDI file position 0** (song start).
/// `startTick` may be arbitrarily large — leading silence before the first hit is intentional and preserved.
/// `durationTicks` is stored for future editing; drums still use fixed transport gate samples.
struct TimelineMidiNote
{
    int midiNote = 60;
    /// 1 … 127 stored for realism; clipped at playback scheduling.
    int velocity = 100;
    /// MIDI note-off (release) velocity, 0 … 127. Default **64** matches Cubase's visible default.
    /// Sent on scheduled note ends for MIDI correctness; many instruments ignore the value.
    int offVelocity = kDefaultMidiNoteOffVelocity;
    /// 1 … 16 (shown on channel messages in the MIDI file).
    std::uint8_t channel = 1;
    /// Ticks since MIDI time zero for this clip, in `ticksPerQuarter` domain (usually 960).
    std::int64_t startTick = 0;
    /// Held note length for future MIDI editing; drums ignore fixed gate separately.
    std::int64_t durationTicks = 240;
};

/// Default internal Musical Time resolution (ticks per quarter note).
inline constexpr int kDefaultExperimentalTicksPerQuarter = 960;

/// Timeline-note clip pattern (in-memory).
struct ExperimentalMidiPattern
{
    double bpm = 110.0;

    std::vector<TimelineMidiNote> timelineNotes;
    /// Ticks per quarter for `timelineNotes` (normally 960; saved with clip).
    int ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
};

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

[[nodiscard]] inline std::int64_t absoluteSampleForTimelineNote(const std::int64_t timelineAnchorSamplesForTickZero,
                                                                const TimelineMidiNote& n,
                                                                const ExperimentalMidiPattern& p,
                                                                double sampleRate) noexcept
{
    const int tpq = experimentalEffectiveTicksPerQuarter(p);
    return timelineAnchorSamplesForTickZero
           + ticksToRelativeSamples(n.startTick, p.bpm > 0.0 ? p.bpm : 120.0, tpq, sampleRate);
}

/// Exclusive end tick: `start + max(1, duration)`. Touching `a.end == b.start` is not an overlap.
[[nodiscard]] inline std::int64_t timelineNoteEndTick(const TimelineMidiNote& n) noexcept
{
    return n.startTick + juce::jmax<std::int64_t>(1, n.durationTicks);
}

/// Same pitch **and** channel (channel is part of note identity in the editor model).
[[nodiscard]] inline bool timelineNotesSharePitchAndChannel(const TimelineMidiNote& a,
                                                            const TimelineMidiNote& b) noexcept
{
    return a.midiNote == b.midiNote && a.channel == b.channel;
}

/// Positive-duration overlap on the same pitch/channel. End-to-start touching is allowed.
[[nodiscard]] inline bool timelineNotesIntervalsOverlap(const TimelineMidiNote& a,
                                                        const TimelineMidiNote& b) noexcept
{
    if (!timelineNotesSharePitchAndChannel(a, b))
    {
        return false;
    }
    return a.startTick < timelineNoteEndTick(b) && b.startTick < timelineNoteEndTick(a);
}

struct TimelineNoteOverlapConflict
{
    int candidateIndex = -1;
    int otherIndex = -1;
    bool otherIsCandidate = false;
};

struct TimelineNoteOverlapCheck
{
    bool valid = true;
    TimelineNoteOverlapConflict conflict;
};

/// Validates a candidate batch against remaining notes and against itself.
/// `ignoreIndices` are existing-note slots being replaced (their originals are not obstacles).
/// When `grandfatherOriginals` has the same size as `candidates`, a candidate-vs-candidate overlap
/// is allowed only if that pair already overlapped (old projects; do not freeze a broken selection).
[[nodiscard]] inline TimelineNoteOverlapCheck validateTimelineNotesNoOverlap(
    const std::vector<TimelineMidiNote>& existing,
    const std::vector<int>& ignoreIndices,
    const std::vector<TimelineMidiNote>& candidates,
    const std::int64_t minDurationTicks,
    const std::vector<TimelineMidiNote>* grandfatherOriginals = nullptr) noexcept
{
    TimelineNoteOverlapCheck r;
    const std::int64_t minD = juce::jmax<std::int64_t>(1, minDurationTicks);
    const bool grandfather = grandfatherOriginals != nullptr
                             && grandfatherOriginals->size() == candidates.size();

    const auto ignored = [&ignoreIndices](const int i) noexcept {
        return std::find(ignoreIndices.begin(), ignoreIndices.end(), i) != ignoreIndices.end();
    };

    for (int c = 0; c < (int)candidates.size(); ++c)
    {
        const auto& cand = candidates[(size_t)c];
        if (cand.durationTicks < minD)
        {
            r.valid = false;
            r.conflict.candidateIndex = c;
            r.conflict.otherIndex = -1;
            r.conflict.otherIsCandidate = false;
            return r;
        }
        for (int e = 0; e < (int)existing.size(); ++e)
        {
            if (ignored(e))
            {
                continue;
            }
            if (timelineNotesIntervalsOverlap(cand, existing[(size_t)e]))
            {
                r.valid = false;
                r.conflict.candidateIndex = c;
                r.conflict.otherIndex = e;
                r.conflict.otherIsCandidate = false;
                return r;
            }
        }
        for (int d = c + 1; d < (int)candidates.size(); ++d)
        {
            if (!timelineNotesIntervalsOverlap(cand, candidates[(size_t)d]))
            {
                continue;
            }
            if (grandfather
                && timelineNotesIntervalsOverlap((*grandfatherOriginals)[(size_t)c],
                                                 (*grandfatherOriginals)[(size_t)d]))
            {
                continue;
            }
            r.valid = false;
            r.conflict.candidateIndex = c;
            r.conflict.otherIndex = d;
            r.conflict.otherIsCandidate = true;
            return r;
        }
    }
    return r;
}
