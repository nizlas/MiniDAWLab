#pragma once

// =============================================================================
// LiveMidiTakeBuilder — turns a captured live-MIDI stream into one clip pattern (pure, testable)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Message-thread finalization logic of a MIDI take, kept free of JUCE components, session or
//   hosts so the focused tests can assert every boundary rule against literal event streams:
//     * events are already stamped with their transport-timeline sample (see
//       `LiveMidiInputBus` time model); the builder converts them to clip ticks with the
//       PROJECT tempo / PPQ — no 120 BPM assumption, no quantization, no interpolation;
//     * a Note On with velocity 0 is a Note Off;
//     * events finished before the record start are not part of the take, but a key still held
//       at the start enters at tick 0 with its real channel and velocity (the tracker remembers
//       it), and the controller state the player had established (sustain, pitch bend, the
//       expression-type controllers) is re-stated at tick 0 so the clip plays back the way the
//       take sounded;
//     * at stop, notes still held end exactly on the stop boundary and a held sustain pedal is
//       released there, so a clip can never end with a hanging tone;
//     * a take with no notes and no controller movement inside the window is empty (no clip);
//       controller-only takes are valid content.
//   The owning coordinator feeds the tracker with every captured event of the row (stopped or
//   playing) and calls `buildTakePattern` once at stop.
// =============================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ui/experimental/ExperimentalMidiPattern.h"

namespace live_midi_take
{
    /// One captured event of a single row (bytes as received; channel NOT remapped).
    struct TakeEvent
    {
        std::int64_t timelineSample = 0;
        std::uint8_t bytes[3] = { 0, 0, 0 };
        std::uint8_t size = 0;
        bool transportPlaying = false;
        bool overflowMarker = false;

        [[nodiscard]] int status() const noexcept { return bytes[0] & 0xf0; }
        [[nodiscard]] int channel() const noexcept { return (bytes[0] & 0x0f) + 1; }
        [[nodiscard]] bool isNoteOn() const noexcept { return size == 3 && status() == 0x90 && bytes[2] > 0; }
        [[nodiscard]] bool isNoteOff() const noexcept
        {
            return size == 3 && (status() == 0x80 || (status() == 0x90 && bytes[2] == 0));
        }
        [[nodiscard]] bool isController() const noexcept { return size == 3 && status() == 0xb0; }
        [[nodiscard]] bool isPitchBend() const noexcept { return size == 3 && status() == 0xe0; }
        [[nodiscard]] int pitchBendValue() const noexcept { return (int)bytes[1] | ((int)bytes[2] << 7); }
    };

    struct HeldNote
    {
        int channel = 1;
        int note = 60;
        int velocity = 100;
    };

    /// Controller / wheel state established by the player (last received values).
    struct ControllerState
    {
        /// `cc[channel-1][controller]` = last value, -1 = never received.
        std::array<std::array<int, 128>, 16> cc {};
        /// Last raw 14-bit wheel position per channel, -1 = never received.
        std::array<int, 16> pitchBend {};

        ControllerState()
        {
            for (auto& row : cc)
            {
                row.fill(-1);
            }
            pitchBend.fill(-1);
        }
    };

    /// Always-on tracker of one row's live state (fed with every event, recording or not).
    struct TakeStateTracker
    {
        std::vector<HeldNote> heldNotes;
        ControllerState state;

        void feed(const TakeEvent& e)
        {
            if (e.overflowMarker)
            {
                // The device ring lost events: the sounding state is unknown — the bus released
                // the live notes, mirror that here so nothing is "held" by stale knowledge.
                heldNotes.clear();
                return;
            }
            const int ch = e.channel();
            if (e.isNoteOn())
            {
                bool updated = false;
                for (auto& h : heldNotes)
                {
                    if (h.channel == ch && h.note == e.bytes[1])
                    {
                        h.velocity = e.bytes[2];
                        updated = true;
                    }
                }
                if (!updated)
                {
                    heldNotes.push_back({ ch, (int)e.bytes[1], (int)e.bytes[2] });
                }
            }
            else if (e.isNoteOff())
            {
                for (size_t i = 0; i < heldNotes.size();)
                {
                    if (heldNotes[i].channel == ch && heldNotes[i].note == e.bytes[1])
                    {
                        heldNotes.erase(heldNotes.begin() + (std::ptrdiff_t)i);
                    }
                    else
                    {
                        ++i;
                    }
                }
            }
            else if (e.isController())
            {
                state.cc[(size_t)(ch - 1)][(size_t)(e.bytes[1] & 0x7f)] = e.bytes[2] & 0x7f;
            }
            else if (e.isPitchBend())
            {
                state.pitchBend[(size_t)(ch - 1)] = e.pitchBendValue();
            }
        }
    };

    struct TakeBuildParams
    {
        std::int64_t recordStartSample = 0;
        /// Exclusive stop boundary (the stop playhead).
        std::int64_t recordStopSample = 0;
        double sampleRate = 48000.0;
        double bpm = 120.0;
        int ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
    };

    struct TakeBuildResult
    {
        ExperimentalMidiPattern pattern;
        /// False when nothing was performed inside the window (no clip must be created).
        bool hasContent = false;
        int notesRecorded = 0;
        int notesClosedAtStop = 0;
        int controllerEventsRecorded = 0;
        int pitchBendEventsRecorded = 0;
        int eventsOutsideWindow = 0;
        bool overflowSeen = false;
        bool sustainReleasedAtStop = false;
    };

    /// Controllers whose established value is re-stated at the start boundary when known.
    [[nodiscard]] inline bool isBoundaryRestatedController(const int controller) noexcept
    {
        switch (controller)
        {
            case 1:  // Modulation
            case 7:  // Volume
            case 11: // Expression
            case 64: // Sustain
            case 65: // Portamento
            case 66: // Sostenuto
            case 67: // Soft pedal
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] inline std::int64_t tickForSample(const std::int64_t timelineSample,
                                                    const TakeBuildParams& p) noexcept
    {
        const std::int64_t rel = timelineSample - p.recordStartSample;
        if (rel <= 0)
        {
            return 0;
        }
        return relativeSamplesToTicks(rel, p.bpm, p.ticksPerQuarter, p.sampleRate);
    }

    /// Build the clip pattern of one row's take. `events` is the row's captured stream in
    /// received order from the moment the take was armed: events before the record start (or
    /// received while the transport was still stopped) only shape the START BOUNDARY state;
    /// events at/after the stop boundary are ignored. `initialState` is the row's always-on
    /// tracker state at the moment capture for this take began (keys already held, pedal state).
    [[nodiscard]] inline TakeBuildResult buildTakePattern(const std::vector<TakeEvent>& events,
                                                          const TakeStateTracker& initialState,
                                                          const TakeBuildParams& p)
    {
        TakeBuildResult r;
        r.pattern.bpm = p.bpm;
        r.pattern.ticksPerQuarter = p.ticksPerQuarter;
        const std::int64_t stopTick
            = juce::jmax<std::int64_t>(1, tickForSample(p.recordStopSample, p));

        struct OpenNote
        {
            int channel = 1;
            int note = 60;
            int velocity = 100;
            std::int64_t startTick = 0;
        };
        std::vector<OpenNote> open;
        std::array<bool, 16> sustainDown {};
        sustainDown.fill(false);

        const auto closeNote = [&](OpenNote& o, const std::int64_t endTick, const int offVelocity) {
            TimelineMidiNote n;
            n.midiNote = o.note;
            n.velocity = juce::jlimit(1, 127, o.velocity);
            n.offVelocity = juce::jlimit(0, 127, offVelocity);
            n.channel = (std::uint8_t)juce::jlimit(1, 16, o.channel);
            n.startTick = o.startTick;
            n.durationTicks = juce::jmax<std::int64_t>(1, endTick - o.startTick);
            r.pattern.timelineNotes.push_back(n);
            ++r.notesRecorded;
        };

        // ---- Start boundary: held keys and established controller state enter at tick 0. The
        // state is the tracker fed with every pre-window event, materialized when the first
        // in-window event arrives (or at the end, for a chord held through the whole take). ----
        TakeStateTracker pre = initialState;
        bool boundaryMaterialized = false;
        const auto materializeBoundary = [&] {
            if (boundaryMaterialized)
            {
                return;
            }
            boundaryMaterialized = true;
            for (const auto& h : pre.heldNotes)
            {
                open.push_back({ h.channel, h.note, h.velocity, 0 });
            }
            for (int ch = 1; ch <= 16; ++ch)
            {
                const auto& ccRow = pre.state.cc[(size_t)(ch - 1)];
                for (int c = 0; c < 128; ++c)
                {
                    if (ccRow[(size_t)c] >= 0 && isBoundaryRestatedController(c))
                    {
                        MidiCcPoint pt;
                        pt.startTick = 0;
                        pt.controller = (std::uint8_t)c;
                        pt.value = (std::uint8_t)ccRow[(size_t)c];
                        pt.channel = (std::uint8_t)ch;
                        pt.interpolationToNext = MidiCcInterpolation::hold;
                        r.pattern.ccPoints.push_back(pt);
                        if (c == 64)
                        {
                            sustainDown[(size_t)(ch - 1)] = ccRow[(size_t)c] >= 64;
                        }
                    }
                }
                const int pb = pre.state.pitchBend[(size_t)(ch - 1)];
                if (pb >= 0 && pb != kMidiPitchBendCentre)
                {
                    MidiPitchBendPoint pt;
                    pt.startTick = 0;
                    pt.value = pb;
                    pt.channel = (std::uint8_t)ch;
                    r.pattern.pitchBendPoints.push_back(pt);
                }
            }
        };

        // ---- The performance inside [start, stop). Captured order is audio-thread order, so
        // every stopped (count-in / pre-roll) event precedes the first playing one; stopped events
        // seen AFTER playing began are post-stop and never shape the start boundary. ----
        bool sawPlaying = false;
        for (const auto& e : events)
        {
            sawPlaying = sawPlaying || e.transportPlaying;
            const bool beforeWindow = (!e.transportPlaying && !sawPlaying)
                                      || (e.transportPlaying && e.timelineSample < p.recordStartSample);
            if (e.overflowMarker)
            {
                r.overflowSeen = true;
                if (!boundaryMaterialized && beforeWindow)
                {
                    pre.feed(e);
                }
                continue;
            }
            const bool inWindow = e.transportPlaying && e.timelineSample >= p.recordStartSample
                                  && e.timelineSample < p.recordStopSample;
            if (!inWindow)
            {
                ++r.eventsOutsideWindow;
                if (!boundaryMaterialized && beforeWindow)
                {
                    pre.feed(e); // before the window: shapes the start-boundary state only
                }
                continue;
            }
            materializeBoundary();
            const std::int64_t tick = juce::jmin(tickForSample(e.timelineSample, p), stopTick);
            const int ch = e.channel();
            if (e.isNoteOn())
            {
                // Retrigger of a held pitch: the earlier note ends where the new one starts.
                for (size_t i = 0; i < open.size();)
                {
                    if (open[i].channel == ch && open[i].note == e.bytes[1])
                    {
                        closeNote(open[i], juce::jmax(tick, open[i].startTick + 1), kDefaultMidiNoteOffVelocity);
                        open.erase(open.begin() + (std::ptrdiff_t)i);
                    }
                    else
                    {
                        ++i;
                    }
                }
                open.push_back({ ch, (int)e.bytes[1], (int)e.bytes[2], tick });
            }
            else if (e.isNoteOff())
            {
                const int offVel = e.status() == 0x80 ? (int)e.bytes[2] : kDefaultMidiNoteOffVelocity;
                for (size_t i = 0; i < open.size();)
                {
                    if (open[i].channel == ch && open[i].note == e.bytes[1])
                    {
                        closeNote(open[i], juce::jmax(tick, open[i].startTick + 1), offVel);
                        open.erase(open.begin() + (std::ptrdiff_t)i);
                    }
                    else
                    {
                        ++i;
                    }
                }
                // A release for a key that was never seen pressed (pressed before the device was
                // routed) carries no note — ignored, never invented.
            }
            else if (e.isController())
            {
                MidiCcPoint pt;
                pt.startTick = tick;
                pt.controller = (std::uint8_t)(e.bytes[1] & 0x7f);
                pt.value = (std::uint8_t)(e.bytes[2] & 0x7f);
                pt.channel = (std::uint8_t)ch;
                pt.interpolationToNext = MidiCcInterpolation::hold; // recorded verbatim, no ramps
                r.pattern.ccPoints.push_back(pt);
                ++r.controllerEventsRecorded;
                if (pt.controller == 64)
                {
                    sustainDown[(size_t)(ch - 1)] = pt.value >= 64;
                }
            }
            else if (e.isPitchBend())
            {
                MidiPitchBendPoint pt;
                pt.startTick = tick;
                pt.value = e.pitchBendValue();
                pt.channel = (std::uint8_t)ch;
                r.pattern.pitchBendPoints.push_back(pt);
                ++r.pitchBendEventsRecorded;
            }
        }

        materializeBoundary(); // nothing inside the window: a chord held throughout still counts

        // ---- Stop boundary: held notes end here; a held sustain pedal is released here. ----
        for (auto& o : open)
        {
            closeNote(o, juce::jmax(stopTick, o.startTick + 1), kDefaultMidiNoteOffVelocity);
            ++r.notesClosedAtStop;
        }
        open.clear();
        for (int ch = 1; ch <= 16; ++ch)
        {
            if (sustainDown[(size_t)(ch - 1)])
            {
                MidiCcPoint pt;
                pt.startTick = stopTick;
                pt.controller = 64;
                pt.value = 0;
                pt.channel = (std::uint8_t)ch;
                pt.interpolationToNext = MidiCcInterpolation::hold;
                r.pattern.ccPoints.push_back(pt);
                r.sustainReleasedAtStop = true;
            }
        }

        r.hasContent = r.notesRecorded > 0 || r.controllerEventsRecorded > 0 || r.pitchBendEventsRecorded > 0;
        if (!r.hasContent)
        {
            // Boundary re-statements alone are not a performance: return an empty pattern.
            r.pattern.timelineNotes.clear();
            r.pattern.ccPoints.clear();
            r.pattern.pitchBendPoints.clear();
            return r;
        }
        std::stable_sort(r.pattern.timelineNotes.begin(), r.pattern.timelineNotes.end(),
                         [](const TimelineMidiNote& a, const TimelineMidiNote& b) {
                             return a.startTick < b.startTick;
                         });
        (void)midi_cc::normalizePoints(r.pattern.ccPoints);
        (void)midi_pb::normalizePoints(r.pattern.pitchBendPoints);
        return r;
    }
} // namespace live_midi_take
