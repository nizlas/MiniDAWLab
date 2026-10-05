#pragma once

// =============================================================================
// ProxyOfflineSequencer — deterministic offline event scheduler for P1D
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §8.3, §10.1, §15.6)
// =============================================================================
// Consumes the immutable P1C ProxyRenderSnapshot and emits per-block MidiBuffers
// with EXACTLY the semantics of live transport scheduling. This is deliberately a
// line-for-line structural mirror of the live pair
//
//   InstrumentTrackController::publishRenderSnapshot          (bake stage)
//   InstrumentTrackController::audioThread_scheduleTransportMidiForSegment
//   InstrumentTrackController::audioThread_scheduleCcForSegment (emission stage)
//
// using the same shared pure helpers (ticksToRelativeSamples,
// absoluteSampleForTimelineNote semantics, midi_cc::collectCcEventsInTickRange,
// midi_channel_diag::effectiveChannel, sanitizeMidiNoteOffVelocity) so the
// renderer never invents a new interpretation of MIDI ordering (task §4).
// If the live pair changes, this mirror MUST change with it — both sites carry
// a cross-reference comment.
//
// Live-parity contract implemented here:
//   * merge order per block: the DESTINATION's own events first, then every
//     ELIGIBLE routed source in session order (PlaybackEngine.cpp ~1391/~1410:
//     "midiSources ... in snapshot order = deterministic merge order ...
//     after that destination's own events");
//   * per track unit within a block: pending Note Offs → CC (chase + due, with
//     per-stream last-sent dedup) → note scan (Note On + same-segment Note Off);
//     equal-offset insertion order inside a juce::MidiBuffer is preserved, so a
//     CC at a note's start sample is active before that Note On (Stage D rule);
//   * LAYER-1 (MidiLayeredRenderBake.h): per source unit the clips form a stack in
//     stored order (last = topmost); the topmost clip owns its whole window, notes of
//     lower clips are cut / resumed at the cover boundaries, CC and pitch bend are
//     delivered only inside a clip's audible spans with the clip's current value
//     restated at each span start (chase). One merged note list per unit;
//   * ORD-1: stable equal-time ordering — notes stable-sorted by absSample, stored
//     order (stack order, then stored note order) is the tie-break;
//   * note-off gate: explicit duration wins; legacy fallback = 100 ms
//     (snapshot renderConfig.noteOffGateMs) baked at the RENDER rate;
//   * destination mute/off is a PLAYBACK gate, never a render gate (PID-006):
//     destination content always renders; ineligible sources are skipped.
//
// TIME DOMAINS (§10.1, TLD-1): snapshot anchors/windows are integers at the
// timeline REFERENCE rate → converted once here via timeline_domain; note/CC
// ticks bake DIRECTLY at the render rate. Persisted coordinates are never
// reinterpreted with the current device rate.
//
// Thread affinity: build on any thread (typically the message thread, before
// worker handoff); emitBlock is then called ONLY by the render worker. The
// sequencer owns all of its data (deep-baked from the immutable snapshot).

#include "domain/TimelineDomain.h"
#include "instruments/MidiLayeredRenderBake.h"
#include "instruments/ProxyRenderSnapshot.h"
#include "ui/experimental/ExperimentalMidiCcAutomation.h"
#include "ui/experimental/ExperimentalMidiChannelDiagnostics.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace proxy_render
{

class ProxyOfflineSequencer final
{
public:
    ProxyOfflineSequencer(const proxy_snapshot::ProxyRenderSnapshot& snap, const double renderRate)
        : renderRate_(renderRate)
    {
        const double refRate = timeline_domain::isValidRate(snap.renderConfig.timelineReferenceRate)
                                   ? snap.renderConfig.timelineReferenceRate
                                   : 48000.0;
        // Mirror of publishRenderSnapshot's gate bake (100 ms default), at the RENDER rate.
        const int gateMs = snap.renderConfig.noteOffGateMs > 0 ? snap.renderConfig.noteOffGateMs : 100;
        gateSamples_ = std::max<std::int64_t>(1, (std::int64_t)std::llround(0.001 * gateMs * renderRate_));

        // Unit 0: the destination's own content (always renders — PID-006); then eligible
        // routed sources in session order (the live merge order, see header comment).
        bakeUnit(snap.destinationClips, snap.destinationMidiOutputChannel, refRate);
        for (const auto& src : snap.sources)
        {
            if (!src.trackOff && !src.muted)
            {
                bakeUnit(src.clips, src.midiOutputChannel, refRate);
            }
        }

        for (const auto& u : units_)
        {
            for (const auto& n : u.notes)
            {
                lastEventSample_ = std::max(lastEventSample_, std::max(n.absSample, n.noteOffAbsSample));
                usedChannels_[(size_t)juce::jlimit(1, 16, n.midiChannel) - 1] = true;
            }
            for (const auto& s : u.ccStreams)
            {
                usedChannels_[(size_t)juce::jlimit(1, 16, s.midiChannel) - 1] = true;
                for (const auto& e : s.events)
                {
                    lastEventSample_ = std::max(lastEventSample_, e.absSample);
                }
            }
        }
    }

    /// Last relevant event in RENDER-rate samples (span end input; §15.6 span rule).
    [[nodiscard]] std::int64_t lastEventRenderSample() const noexcept { return lastEventSample_; }

    [[nodiscard]] bool hasAnyEvents() const noexcept
    {
        for (const auto& u : units_)
        {
            if (!u.notes.empty() || !u.ccStreams.empty() || !u.pitchBendStreams.empty())
            {
                return true;
            }
        }
        return false;
    }

    /// §4 initial-state sequence, measured SPIKE-02 contract (report §8; steering §9.4.4):
    /// per channel used by the schedule — sustain off (CC64=0), all sound off (CC120),
    /// reset all controllers (CC121), all notes off (CC123) — injected at sample 0 of the
    /// first block, BEFORE the first musical events (which then perform their own CC chase).
    void emitResetAndChasePrefix(juce::MidiBuffer& out) const
    {
        for (int ch = 1; ch <= 16; ++ch)
        {
            if (!usedChannels_[(size_t)ch - 1])
            {
                continue;
            }
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 64, 0), 0);
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 120, 0), 0);
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 121, 0), 0);
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 123, 0), 0);
        }
    }

    /// One distinct (channel, note) the content plays, with the velocity of its first occurrence
    /// — the stimulus set of the readiness verification (ProxyRenderExecutor.h).
    struct StimulusNote
    {
        int midiChannel = 1;
        int midiNote = 60;
        int velocity = 100;
    };

    /// Distinct (channel, note) pairs across all units in timeline order of first occurrence,
    /// at most `maxCount`. Pure query — emission state is untouched.
    [[nodiscard]] std::vector<StimulusNote> collectDistinctNoteOns(const std::size_t maxCount) const
    {
        std::vector<const BakedNote*> all;
        for (const auto& u : units_)
        {
            for (const auto& n : u.notes)
            {
                all.push_back(&n);
            }
        }
        std::stable_sort(all.begin(), all.end(),
                         [](const BakedNote* a, const BakedNote* b) { return a->absSample < b->absSample; });
        std::vector<StimulusNote> out;
        bool seen[16][128] = {};
        for (const BakedNote* n : all)
        {
            const int ch = juce::jlimit(1, 16, n->midiChannel);
            const int note = juce::jlimit(0, 127, n->midiNote);
            if (seen[ch - 1][note])
            {
                continue;
            }
            seen[ch - 1][note] = true;
            out.push_back({ ch, note, juce::jlimit(1, 127, n->velocity) });
            if (out.size() >= maxCount)
            {
                break;
            }
        }
        return out;
    }

    /// The controller state the content establishes first (the first value of every CC and
    /// pitch-bend stream, after the reset prefix) — what the readiness stimulus plays into so an
    /// instrument whose sound depends on its initial controllers answers as it will in the
    /// render. Pure query — the per-stream `lastSentValue` emission state is untouched.
    void emitInitialControllerState(juce::MidiBuffer& out) const
    {
        emitResetAndChasePrefix(out);
        for (const auto& u : units_)
        {
            for (const auto& s : u.ccStreams)
            {
                if (!s.events.empty())
                {
                    out.addEvent(juce::MidiMessage::controllerEvent(s.midiChannel, s.controller, s.events.front().value), 0);
                }
            }
            for (const auto& s : u.pitchBendStreams)
            {
                if (!s.events.empty())
                {
                    out.addEvent(juce::MidiMessage::pitchWheel(s.midiChannel, juce::jlimit(0, kMidiPitchBendMax, s.events.front().value)), 0);
                }
            }
        }
    }

    /// All Sound Off / All Notes Off / sustain off on every channel the schedule uses (the flush
    /// between the readiness stimulus and the render).
    void emitAllSoundOff(juce::MidiBuffer& out) const
    {
        for (int ch = 1; ch <= 16; ++ch)
        {
            if (!usedChannels_[(size_t)ch - 1])
            {
                continue;
            }
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 64, 0), 0);
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 120, 0), 0);
            out.addEvent(juce::MidiMessage::controllerEvent(ch, 123, 0), 0);
        }
    }

    /// Emit every event due in [blockStart, blockStart + numSamples) into `out` (offsets are
    /// block-relative). Blocks MUST be requested sequentially from 0 — the sequencer carries
    /// pending-note-off and CC-last-sent state across blocks exactly like the live engine.
    /// [Render worker] after handoff.
    void emitBlock(const std::int64_t blockStart, const int numSamples, juce::MidiBuffer& out)
    {
        if (numSamples <= 0)
        {
            return;
        }
        const std::int64_t segEnd = blockStart + numSamples;
        for (auto& u : units_)
        {
            emitUnitSegment(u, blockStart, segEnd, numSamples, out);
        }
    }

private:
    // ---- Baked shapes: mirrors of InstrumentNoteRenderEvent / InstrumentCcRenderStream ----
    struct BakedNote
    {
        std::int64_t absSample = 0;
        std::int64_t noteOffAbsSample = 0;
        int midiNote = 60;
        int velocity = 100;
        int offVelocity = 64;
        int midiChannel = 1;
    };
    struct BakedCcEvent
    {
        std::int64_t absSample = 0;
        int value = 0;
    };
    struct BakedCcStream
    {
        int controller = 0;
        int midiChannel = 1;
        std::vector<BakedCcEvent> events;
        int lastSentValue = -1; ///< emission state (live rtCcLastSentValue_ mirror)
    };
    struct PendingOff
    {
        std::int64_t dueAbsSample = 0;
        int midiNote = 0;
        int midiChannel = 1;
        int offVelocity = 64;
    };
    struct BakedPitchBendStream
    {
        int midiChannel = 1;
        std::vector<BakedCcEvent> events; ///< `value` = raw 14-bit wheel position
        int lastSentValue = -1;           ///< emission state (live rtPitchBendLastSentValue_ mirror)
    };
    struct Unit
    {
        /// Merged audible note segments of the unit (LAYER-1), ascending `absSample`.
        std::vector<BakedNote> notes;
        std::vector<BakedCcStream> ccStreams;
        std::vector<BakedPitchBendStream> pitchBendStreams;
        std::vector<PendingOff> pendingOffs; ///< emission state (live rtPendingOffs_ mirror)
        bool firstSegment = true;            ///< live "discontinuity" on the first delivered block
    };

    /// Bake one track unit — structural mirror of publishRenderSnapshot (see header comment),
    /// with §10.1 domain conversion: anchors/windows reference→render, ticks at render rate.
    void bakeUnit(const std::vector<proxy_snapshot::SnapshotClip>& clips,
                  const int trackMidiOutputChannel,
                  const double refRate)
    {
        Unit unit;
        const int forcedMidiChannel = trackMidiOutputChannel;
        const auto toRender = [this, refRate](const std::int64_t refSamples) {
            return timeline_domain::referenceToEngineSamples(refSamples, refRate, renderRate_);
        };

        // LAYER-1: the clip stack is the snapshot's STORED order (last = topmost); windows and
        // audible spans in RENDER samples. Mirror of publishRenderSnapshot — change both together.
        std::vector<const proxy_snapshot::SnapshotClip*> stack;
        std::vector<midi_layer_bake::ClipWindow> windows;
        for (const auto& clip : clips)
        {
            if (clip.lengthSamples > 0)
            {
                stack.push_back(&clip);
                windows.push_back({ toRender(clip.startSamples), toRender(clip.startSamples + clip.lengthSamples) });
            }
        }
        const std::vector<std::vector<midi_layer_bake::SampleSpan>> audible
            = midi_layer_bake::audibleSpansPerClip(windows);

        for (std::size_t ci = 0; ci < stack.size(); ++ci)
        {
            const proxy_snapshot::SnapshotClip& clip = *stack[ci];
            const std::vector<midi_layer_bake::SampleSpan>& spans = audible[ci];
            if (spans.empty())
            {
                continue;
            }
            const std::int64_t windowStart = windows[ci].start;
            const std::int64_t windowEnd = windows[ci].endExclusive;
            const std::int64_t anchorRender = toRender(clip.timelineAnchorSamples);
            const double bpm = clip.bpm > 0.0 ? clip.bpm : 120.0;
            const int tpq = juce::jmax(1, clip.ticksPerQuarter);
            std::vector<BakedNote> clipNotes;
            for (const auto& tn : clip.notes)
            {
                const std::int64_t on = anchorRender + ticksToRelativeSamples(tn.startTick, bpm, tpq, renderRate_);
                if (on < windowStart || on >= windowEnd)
                {
                    continue;
                }
                const std::int64_t durSam = ticksToRelativeSamples(
                    juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, renderRate_);
                std::int64_t off = on + juce::jmax<std::int64_t>(1, durSam);
                off = juce::jmin(off, windowEnd);
                if (off <= on)
                {
                    off = on + 1;
                }
                BakedNote proto;
                proto.midiNote = juce::jlimit(0, 127, tn.midiNote);
                proto.velocity = juce::jlimit(1, 127, tn.velocity);
                proto.offVelocity = sanitizeMidiNoteOffVelocity(tn.offVelocity);
                proto.midiChannel = (forcedMidiChannel == kTrackMidiOutputChannelAny
                                         ? juce::jlimit(1, 16, tn.channel)
                                         : forcedMidiChannel);
                midi_layer_bake::forEachAudibleNoteSegment(on, off, spans,
                    [&](const std::int64_t segOn, const std::int64_t segOff) {
                        BakedNote ev = proto;
                        ev.absSample = segOn;
                        ev.noteOffAbsSample = segOff;
                        clipNotes.push_back(ev);
                    });
            }
            // ORD-1: stored order is the documented equal-time tie-break (stable sort).
            std::stable_sort(clipNotes.begin(), clipNotes.end(),
                             [](const BakedNote& a, const BakedNote& b) {
                                 return a.absSample < b.absSample;
                             });
            unit.notes.insert(unit.notes.end(), clipNotes.begin(), clipNotes.end());
        }
        std::stable_sort(unit.notes.begin(), unit.notes.end(),
                         [](const BakedNote& a, const BakedNote& b) {
                             return a.absSample < b.absSample;
                         });

        // Stage D CC bake mirror: per-(controller, effective channel) streams, each clip's events
        // restricted to its audible spans with the span-start chase restatement, clips visited
        // bottom → top (the upper clip's value is appended last at a touching boundary and wins),
        // then per-stream stable sort by sample.
        {
            const auto findOrAddStream = [&unit](const int controller, const int effCh) -> BakedCcStream& {
                for (auto& s : unit.ccStreams)
                {
                    if (s.controller == controller && s.midiChannel == effCh)
                    {
                        return s;
                    }
                }
                BakedCcStream ns;
                ns.controller = controller;
                ns.midiChannel = effCh;
                unit.ccStreams.push_back(std::move(ns));
                return unit.ccStreams.back();
            };
            for (std::size_t ci = 0; ci < stack.size(); ++ci)
            {
                const proxy_snapshot::SnapshotClip& clip = *stack[ci];
                if (clip.ccPoints.empty() || audible[ci].empty())
                {
                    continue;
                }
                const std::int64_t anchorRender = toRender(clip.timelineAnchorSamples);
                const double bpm = clip.bpm > 0.0 ? clip.bpm : 120.0;
                const int tpq = juce::jmax(1, clip.ticksPerQuarter);
                std::vector<MidiCcPoint> pts;
                pts.reserve(clip.ccPoints.size());
                for (const auto& sp : clip.ccPoints)
                {
                    MidiCcPoint p;
                    p.startTick = sp.startTick;
                    p.controller = (std::uint8_t)juce::jlimit(0, 127, sp.controller);
                    p.value = (std::uint8_t)juce::jlimit(0, 127, sp.value);
                    p.channel = (std::uint8_t)juce::jlimit(1, 16, sp.channel);
                    p.interpolationToNext = sp.interpolationToNext == 1 ? MidiCcInterpolation::linear
                                                                        : MidiCcInterpolation::hold;
                    pts.push_back(p);
                }
                (void)midi_cc::normalizePoints(pts); // defensive, same as live bake
                std::int64_t lastTick = 0;
                for (const auto& p : pts)
                {
                    lastTick = juce::jmax(lastTick, p.startTick);
                }
                for (const auto& key : midi_cc::distinctStreams(pts))
                {
                    std::vector<midi_cc::MidiCcEvent> evs;
                    midi_cc::collectCcEventsInTickRange(pts, key.controller, key.channel, 0,
                                                        lastTick + 1, std::nullopt, evs);
                    std::vector<midi_layer_bake::StreamEvent> clipEvents;
                    clipEvents.reserve(evs.size());
                    for (const auto& e : evs)
                    {
                        clipEvents.push_back({ anchorRender + ticksToRelativeSamples(e.tick, bpm, tpq, renderRate_),
                                               (int)e.value });
                    }
                    std::stable_sort(clipEvents.begin(), clipEvents.end(),
                                     [](const midi_layer_bake::StreamEvent& a, const midi_layer_bake::StreamEvent& b) {
                                         return a.absSample < b.absSample;
                                     });
                    const std::vector<midi_layer_bake::StreamEvent> audibleEvents
                        = midi_layer_bake::restrictStreamToAudibleSpans(clipEvents, audible[ci], windows[ci].endExclusive);
                    const int effCh = midi_channel_diag::effectiveChannel(key.channel, forcedMidiChannel);
                    auto& stream = findOrAddStream(key.controller, effCh);
                    for (const auto& e : audibleEvents)
                    {
                        BakedCcEvent rev;
                        rev.absSample = e.absSample;
                        rev.value = juce::jlimit(0, 127, e.value);
                        stream.events.push_back(rev);
                    }
                }
            }
            for (auto& s : unit.ccStreams)
            {
                std::stable_sort(s.events.begin(), s.events.end(),
                                 [](const BakedCcEvent& a, const BakedCcEvent& b) {
                                     return a.absSample < b.absSample;
                                 });
            }
            std::stable_sort(unit.ccStreams.begin(), unit.ccStreams.end(),
                             [](const BakedCcStream& a, const BakedCcStream& b) {
                                 return a.controller != b.controller ? a.controller < b.controller
                                                                     : a.midiChannel < b.midiChannel;
                             });
        }

        // Pitch-bend bake mirror (live publishRenderSnapshot): one stream per effective channel,
        // recorded values verbatim, the same layering / chase rule, per-stream stable sort.
        {
            const auto findOrAddStream = [&unit](const int effCh) -> BakedPitchBendStream& {
                for (auto& s : unit.pitchBendStreams)
                {
                    if (s.midiChannel == effCh)
                    {
                        return s;
                    }
                }
                BakedPitchBendStream ns;
                ns.midiChannel = effCh;
                unit.pitchBendStreams.push_back(std::move(ns));
                return unit.pitchBendStreams.back();
            };
            for (std::size_t ci = 0; ci < stack.size(); ++ci)
            {
                const proxy_snapshot::SnapshotClip& clip = *stack[ci];
                if (clip.pitchBendPoints.empty() || audible[ci].empty())
                {
                    continue;
                }
                const std::int64_t anchorRender = toRender(clip.timelineAnchorSamples);
                const double bpm = clip.bpm > 0.0 ? clip.bpm : 120.0;
                const int tpq = juce::jmax(1, clip.ticksPerQuarter);
                std::vector<MidiPitchBendPoint> pts;
                pts.reserve(clip.pitchBendPoints.size());
                for (const auto& sp : clip.pitchBendPoints)
                {
                    MidiPitchBendPoint p;
                    p.startTick = sp.startTick;
                    p.value = midi_pb::sanitizeValue(sp.value);
                    p.channel = (std::uint8_t)midi_pb::sanitizeChannel(sp.channel);
                    pts.push_back(p);
                }
                (void)midi_pb::normalizePoints(pts);
                for (int nativeCh = 1; nativeCh <= 16; ++nativeCh)
                {
                    std::vector<midi_layer_bake::StreamEvent> clipEvents;
                    for (const auto& p : pts)
                    {
                        if ((int)p.channel == nativeCh)
                        {
                            clipEvents.push_back({ anchorRender + ticksToRelativeSamples(p.startTick, bpm, tpq, renderRate_),
                                                   p.value });
                        }
                    }
                    if (clipEvents.empty())
                    {
                        continue;
                    }
                    std::stable_sort(clipEvents.begin(), clipEvents.end(),
                                     [](const midi_layer_bake::StreamEvent& a, const midi_layer_bake::StreamEvent& b) {
                                         return a.absSample < b.absSample;
                                     });
                    const std::vector<midi_layer_bake::StreamEvent> audibleEvents
                        = midi_layer_bake::restrictStreamToAudibleSpans(clipEvents, audible[ci], windows[ci].endExclusive);
                    const int effCh = midi_channel_diag::effectiveChannel(nativeCh, forcedMidiChannel);
                    auto& stream = findOrAddStream(effCh);
                    for (const auto& e : audibleEvents)
                    {
                        BakedCcEvent rev;
                        rev.absSample = e.absSample;
                        rev.value = e.value;
                        stream.events.push_back(rev);
                    }
                }
            }
            for (auto& s : unit.pitchBendStreams)
            {
                std::stable_sort(s.events.begin(), s.events.end(),
                                 [](const BakedCcEvent& a, const BakedCcEvent& b) {
                                     return a.absSample < b.absSample;
                                 });
            }
            std::stable_sort(unit.pitchBendStreams.begin(), unit.pitchBendStreams.end(),
                             [](const BakedPitchBendStream& a, const BakedPitchBendStream& b) {
                                 return a.midiChannel < b.midiChannel;
                             });
        }

        units_.push_back(std::move(unit));
    }

    /// One unit, one segment — structural mirror of audioThread_scheduleTransportMidiForSegment
    /// for the offline case (sequential from 0: no seeks/loops, so "discontinuity" only fires on
    /// the very first segment, matching the live first-delivery revision bump).
    void emitUnitSegment(Unit& u,
                         const std::int64_t segStart,
                         const std::int64_t segEnd,
                         const int numSamples,
                         juce::MidiBuffer& out)
    {
        const bool discontinuity = u.firstSegment;
        u.firstSegment = false;

        // 1) Pending Note Offs (cleanup priority: "Note Off / cleanup → CC → Note On").
        {
            size_t w = 0;
            for (size_t i = 0; i < u.pendingOffs.size(); ++i)
            {
                const PendingOff p = u.pendingOffs[i];
                if (p.dueAbsSample >= segEnd)
                {
                    u.pendingOffs[w++] = p;
                    continue;
                }
                const int rel = (int)juce::jmax<std::int64_t>(0, p.dueAbsSample - segStart);
                out.addEvent(juce::MidiMessage::noteOff(p.midiChannel, p.midiNote,
                                                        (juce::uint8)juce::jlimit(0, 127, p.offVelocity)),
                             juce::jlimit(0, numSamples - 1, rel));
            }
            u.pendingOffs.resize(w);
        }

        // 2) CC (chase + due events, unchanged-value dedup) — mirror of
        //    audioThread_scheduleCcForSegment.
        for (auto& s : u.ccStreams)
        {
            const auto emitCc = [&](const int offset, const int value) {
                if (s.lastSentValue == value)
                {
                    return;
                }
                out.addEvent(juce::MidiMessage::controllerEvent(s.midiChannel, s.controller, value),
                             juce::jlimit(0, numSamples - 1, offset));
                s.lastSentValue = value;
            };
            const auto lowerBound = [&s](const std::int64_t v) {
                return std::lower_bound(s.events.begin(), s.events.end(), v,
                                        [](const BakedCcEvent& e, const std::int64_t x) {
                                            return e.absSample < x;
                                        });
            };
            if (discontinuity || s.lastSentValue < 0)
            {
                // Chase: latest event STRICTLY before the segment start; no invented default
                // before the stream's first point.
                auto it = lowerBound(segStart);
                if (it != s.events.begin())
                {
                    emitCc(0, (it - 1)->value);
                }
            }
            for (auto it = lowerBound(segStart); it != s.events.end() && it->absSample < segEnd; ++it)
            {
                emitCc((int)(it->absSample - segStart), it->value);
            }
        }

        // 2b) Pitch bend — mirror of audioThread_schedulePitchBendForSegment (hold semantics,
        //     chase at the first segment, unchanged-value dedup).
        for (auto& s : u.pitchBendStreams)
        {
            const auto emitPb = [&](const int offset, const int value) {
                if (s.lastSentValue == value)
                {
                    return;
                }
                out.addEvent(juce::MidiMessage::pitchWheel(s.midiChannel, juce::jlimit(0, kMidiPitchBendMax, value)),
                             juce::jlimit(0, numSamples - 1, offset));
                s.lastSentValue = value;
            };
            const auto lowerBound = [&s](const std::int64_t v) {
                return std::lower_bound(s.events.begin(), s.events.end(), v,
                                        [](const BakedCcEvent& e, const std::int64_t x) {
                                            return e.absSample < x;
                                        });
            };
            if (discontinuity || s.lastSentValue < 0)
            {
                auto it = lowerBound(segStart);
                if (it != s.events.begin())
                {
                    emitPb(0, (it - 1)->value);
                }
            }
            for (auto it = lowerBound(segStart); it != s.events.end() && it->absSample < segEnd; ++it)
            {
                emitPb((int)(it->absSample - segStart), it->value);
            }
        }

        // 3) Note scan over the unit's merged segment list (Note On + same-segment or deferred
        //    Note Off) — mirror of the live scheduler's walk of `InstrumentTrackRenderSnapshot::notes`.
        {
            auto it = std::lower_bound(u.notes.begin(), u.notes.end(), segStart,
                                       [](const BakedNote& e, const std::int64_t v) {
                                           return e.absSample < v;
                                       });
            for (; it != u.notes.end() && it->absSample < segEnd; ++it)
            {
                const BakedNote& ev = *it;
                if (ev.absSample < segStart)
                {
                    continue;
                }
                const int onOffset = juce::jlimit(0, numSamples - 1, (int)(ev.absSample - segStart));
                out.addEvent(juce::MidiMessage::noteOn(ev.midiChannel, ev.midiNote,
                                                       (float)ev.velocity / 127.0f),
                             onOffset);
                const std::int64_t dueAbs = (ev.noteOffAbsSample > ev.absSample)
                                                ? ev.noteOffAbsSample
                                                : (ev.absSample + gateSamples_);
                if (dueAbs >= segStart && dueAbs < segEnd)
                {
                    out.addEvent(juce::MidiMessage::noteOff(ev.midiChannel, ev.midiNote,
                                                            (juce::uint8)juce::jlimit(0, 127, ev.offVelocity)),
                                 juce::jlimit(0, numSamples - 1, (int)(dueAbs - segStart)));
                }
                else if (dueAbs >= segEnd)
                {
                    u.pendingOffs.push_back({ dueAbs, ev.midiNote, ev.midiChannel, ev.offVelocity });
                }
            }
        }
    }

    const double renderRate_;
    std::int64_t gateSamples_ = 1;
    std::vector<Unit> units_;
    bool usedChannels_[16] = {};
    std::int64_t lastEventSample_ = 0;
};

} // namespace proxy_render
