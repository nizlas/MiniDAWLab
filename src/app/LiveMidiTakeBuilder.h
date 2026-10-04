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
//   playing) and calls `buildTakePasses` once at stop.
//
// CYCLE PASSES (`buildTakePasses`)
//   With Cycle on, one recording run yields one take per pass. Pass boundaries are the engine's
//   WRAP MARKERS (exact mono sample of each wrap, with the transport's wrap serial), never a UI
//   timer and never inferred from event positions: an event belongs to the pass whose wrap
//   markers precede it on the monotone clock, which keeps late-delivered events in the right
//   pass. Windows: pass 0 = [record start, right locator), full passes = [left, right), the last
//   pass = [left, stop) — a stop exactly on a wrap yields no zero-length pass. Markers beyond the
//   serial observed at stop belong to blocks that ran after the user's stop; they and every
//   event after them are discarded. Each pass is built with the single-window builder, starting
//   from the row state at the end of the previous pass: a key held across a wrap ends on the
//   previous pass's end boundary and re-enters the next pass at tick 0 with its channel and
//   velocity; sustain / expression / wheel state is restated at tick 0. These boundary events are
//   synthesized into the stored takes only — the live tracker never sees them.
// =============================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "ui/experimental/ExperimentalMidiPattern.h"

namespace live_midi_take
{
    /// One captured event of a single row (bytes as received; channel NOT remapped).
    struct TakeEvent
    {
        std::int64_t timelineSample = 0;
        /// Position on the engine's monotone clock (pass assignment / ordering; see header).
        std::int64_t monoSample = 0;
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
        /// Mono-clock position of the start boundary (engine-acknowledged); when >= 0 every event
        /// before it on the mono clock shapes the start state only (exact, independent of the
        /// playing flag). -1 = unknown (timeline / playing-flag rules alone).
        std::int64_t recordStartMonoSample = -1;
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
            // A stopped event shapes the start state only while the transport still stands at (or
            // before) the record boundary — a key pressed after Stop (frozen playhead = stop
            // position) in a pass that never saw a playing event must not be invented as held.
            const bool beforeByMono = p.recordStartMonoSample >= 0 && e.monoSample < p.recordStartMonoSample;
            const bool beforeWindow = beforeByMono
                                      || (!e.transportPlaying && !sawPlaying && e.timelineSample <= p.recordStartSample)
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
            const bool inWindow = !beforeByMono && e.transportPlaying && e.timelineSample >= p.recordStartSample
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

    // ------------------------------------------------------------------------- cycle passes

    /// One engine wrap seen during the take (from `LiveMidiInputBus::CapturedEvent::wrapMarker`).
    struct WrapMarker
    {
        std::int64_t monoSample = 0;
        std::uint32_t wrapSerial = 0;
    };

    struct CycleTakeParams
    {
        /// Record boundary and stop boundary on the timeline (engine-acknowledged run boundaries).
        std::int64_t recordStartSample = 0;
        std::int64_t recordStopSample = 0;
        /// The same boundaries on the engine's mono clock (-1 = unknown). When known they are
        /// authoritative: events / markers at or after `recordStopMonoSample` are discarded,
        /// events before `recordStartMonoSample` only shape the start state.
        std::int64_t recordStartMonoSample = -1;
        std::int64_t recordStopMonoSample = -1;
        /// Transport wrap count at the stop boundary: markers with a larger serial happened after
        /// the stop and are discarded with their events (second guard next to the mono stop).
        std::uint32_t stopWrapSerial = 0;
        /// Cycle range while recording (used only when `cycleActive`).
        bool cycleActive = false;
        std::int64_t leftLocatorSample = 0;
        std::int64_t rightLocatorSample = 0;
        /// Wrap markers received during the take, in capture order.
        std::vector<WrapMarker> wrapMarkers;
        double sampleRate = 48000.0;
        double bpm = 120.0;
        int ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
    };

    struct TakePassResult
    {
        int passIndex = 0;
        /// Clip window on the timeline: `[startSample, endSampleExclusive)`.
        std::int64_t startSample = 0;
        std::int64_t endSampleExclusive = 0;
        TakeBuildResult build;
        /// Events of this pass in captured order (diagnostics / tests).
        int eventsAssigned = 0;
    };

    /// Split one recording run into its passes and build each pass's pattern. With Cycle off (or
    /// no wrap marker) this is exactly one pass = `buildTakePattern` over the whole window.
    /// Passes with a non-positive window (stop exactly on a wrap) are not returned.
    [[nodiscard]] inline std::vector<TakePassResult> buildTakePasses(const std::vector<TakeEvent>& events,
                                                                     const TakeStateTracker& initialState,
                                                                     const CycleTakeParams& p)
    {
        std::vector<TakePassResult> out;

        // Valid wrap markers (at or before the stop), ascending on the mono clock; the first
        // marker beyond the stop serial bounds the recording on the mono clock.
        std::vector<WrapMarker> valid;
        std::int64_t stopMono = p.recordStopMonoSample >= 0 ? p.recordStopMonoSample
                                                            : std::numeric_limits<std::int64_t>::max();
        if (p.cycleActive)
        {
            for (const WrapMarker& m : p.wrapMarkers)
            {
                if (m.wrapSerial <= p.stopWrapSerial && m.monoSample < stopMono)
                {
                    valid.push_back(m);
                }
                else
                {
                    stopMono = std::min(stopMono, m.monoSample);
                }
            }
            std::stable_sort(valid.begin(), valid.end(), [](const WrapMarker& a, const WrapMarker& b) {
                return a.monoSample < b.monoSample;
            });
        }

        // Pass windows.
        struct Window
        {
            std::int64_t start = 0;
            std::int64_t end = 0;
        };
        std::vector<Window> windows;
        if (valid.empty())
        {
            windows.push_back({ p.recordStartSample, p.recordStopSample });
        }
        else
        {
            windows.push_back({ p.recordStartSample, p.rightLocatorSample });
            for (std::size_t k = 1; k < valid.size(); ++k)
            {
                windows.push_back({ p.leftLocatorSample, p.rightLocatorSample });
            }
            windows.push_back({ p.leftLocatorSample, p.recordStopSample });
        }

        // Pass assignment: number of valid markers at or before the event's mono position.
        std::vector<std::vector<TakeEvent>> perPass(windows.size());
        for (const TakeEvent& e : events)
        {
            if (e.monoSample >= stopMono)
            {
                continue; // after the stop (a block that ran past the user's Stop)
            }
            std::size_t pass = 0;
            for (const WrapMarker& m : valid)
            {
                if (m.monoSample <= e.monoSample)
                {
                    ++pass;
                }
            }
            if (pass < perPass.size())
            {
                perPass[pass].push_back(e);
            }
        }

        // Build each pass from the row state at the end of the previous pass.
        TakeStateTracker state = initialState;
        for (std::size_t k = 0; k < windows.size(); ++k)
        {
            const Window& w = windows[k];
            if (w.end > w.start)
            {
                TakeBuildParams bp;
                bp.recordStartSample = w.start;
                bp.recordStopSample = w.end;
                // Pass 0 starts at the run's start boundary; a later pass at its wrap marker.
                bp.recordStartMonoSample = k == 0 ? p.recordStartMonoSample : valid[k - 1].monoSample;
                bp.sampleRate = p.sampleRate;
                bp.bpm = p.bpm;
                bp.ticksPerQuarter = p.ticksPerQuarter;
                TakePassResult r;
                r.passIndex = (int)k;
                r.startSample = w.start;
                r.endSampleExclusive = w.end;
                r.eventsAssigned = (int)perPass[k].size();
                r.build = buildTakePattern(perPass[k], state, bp);
                out.push_back(std::move(r));
            }
            for (const TakeEvent& e : perPass[k])
            {
                state.feed(e); // the next pass starts from the real state, never from synthesized boundary events
            }
        }
        return out;
    }
} // namespace live_midi_take
