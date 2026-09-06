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
//   * ORD-1: stable equal-time ordering — plans stable-sorted by start, notes
//     stable-sorted by absSample, stored order is the tie-break;
//   * note-off gate: explicit duration wins; legacy fallback = 100 ms
//     (snapshot renderConfig.noteOffGateMs) baked at the RENDER rate;
//   * destination mute/off is a PLAYBACK gate, never a render gate (PID-006):
//     destination content always renders; ineligible sources are skipped;
//   * CC events are NOT filtered to clip windows (controller state is sticky);
//     notes ARE filtered to [startSamples, endSamplesExclusive).
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
            for (const auto& plan : u.clips)
            {
                for (const auto& n : plan.notes)
                {
                    lastEventSample_ = std::max(lastEventSample_, std::max(n.absSample, n.noteOffAbsSample));
                    usedChannels_[(size_t)juce::jlimit(1, 16, n.midiChannel) - 1] = true;
                }
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
            if (!u.clips.empty() || !u.ccStreams.empty())
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
    struct BakedPlan
    {
        std::int64_t startSamples = 0;
        std::int64_t endSamplesExclusive = 0;
        std::vector<BakedNote> notes;
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
    struct Unit
    {
        std::vector<BakedPlan> clips;
        std::vector<BakedCcStream> ccStreams;
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

        for (const auto& clip : clips)
        {
            if (clip.lengthSamples <= 0)
            {
                continue;
            }
            BakedPlan plan;
            plan.startSamples = toRender(clip.startSamples);
            plan.endSamplesExclusive = toRender(clip.startSamples + clip.lengthSamples);
            const std::int64_t anchorRender = toRender(clip.timelineAnchorSamples);
            const double bpm = clip.bpm > 0.0 ? clip.bpm : 120.0;
            const int tpq = juce::jmax(1, clip.ticksPerQuarter);
            for (const auto& tn : clip.notes)
            {
                BakedNote ev;
                ev.absSample = anchorRender + ticksToRelativeSamples(tn.startTick, bpm, tpq, renderRate_);
                const std::int64_t durSam = ticksToRelativeSamples(
                    juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, renderRate_);
                ev.noteOffAbsSample = ev.absSample + juce::jmax<std::int64_t>(1, durSam);
                ev.noteOffAbsSample = juce::jmin(ev.noteOffAbsSample, plan.endSamplesExclusive);
                if (ev.noteOffAbsSample <= ev.absSample)
                {
                    ev.noteOffAbsSample = ev.absSample + 1;
                }
                ev.midiNote = juce::jlimit(0, 127, tn.midiNote);
                ev.velocity = juce::jlimit(1, 127, tn.velocity);
                ev.offVelocity = sanitizeMidiNoteOffVelocity(tn.offVelocity);
                ev.midiChannel = (forcedMidiChannel == kTrackMidiOutputChannelAny
                                      ? juce::jlimit(1, 16, tn.channel)
                                      : forcedMidiChannel);
                if (ev.absSample < plan.startSamples || ev.absSample >= plan.endSamplesExclusive)
                {
                    continue;
                }
                plan.notes.push_back(ev);
            }
            // ORD-1: stored order is the documented equal-time tie-break (stable sort).
            std::stable_sort(plan.notes.begin(), plan.notes.end(),
                             [](const BakedNote& a, const BakedNote& b) {
                                 return a.absSample < b.absSample;
                             });
            unit.clips.push_back(std::move(plan));
        }
        std::stable_sort(unit.clips.begin(), unit.clips.end(),
                         [](const BakedPlan& a, const BakedPlan& b) {
                             return a.startSamples < b.startSamples;
                         });

        // Stage D CC bake mirror: clips visited in ascending start order (stable), events NOT
        // window-filtered, per-(controller, effective channel) streams, last-wins on equal
        // samples by visitation order, then per-stream stable sort by sample.
        {
            struct ClipRef
            {
                const proxy_snapshot::SnapshotClip* clip = nullptr;
            };
            std::vector<ClipRef> orderedClips;
            for (const auto& clip : clips)
            {
                if (clip.lengthSamples > 0 && !clip.ccPoints.empty())
                {
                    orderedClips.push_back({ &clip });
                }
            }
            std::stable_sort(orderedClips.begin(), orderedClips.end(),
                             [](const ClipRef& a, const ClipRef& b) {
                                 return a.clip->startSamples < b.clip->startSamples;
                             });
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
            for (const auto& ref : orderedClips)
            {
                const proxy_snapshot::SnapshotClip& clip = *ref.clip;
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
                    const int effCh = midi_channel_diag::effectiveChannel(key.channel, forcedMidiChannel);
                    auto& stream = findOrAddStream(key.controller, effCh);
                    for (const auto& e : evs)
                    {
                        BakedCcEvent rev;
                        rev.absSample = anchorRender
                                        + ticksToRelativeSamples(e.tick, bpm, tpq, renderRate_);
                        rev.value = (int)e.value;
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

        // 3) Note scan (Note On + same-segment or deferred Note Off).
        for (const auto& plan : u.clips)
        {
            if (plan.endSamplesExclusive <= segStart || plan.startSamples >= segEnd)
            {
                continue;
            }
            auto it = std::lower_bound(plan.notes.begin(), plan.notes.end(), segStart,
                                       [](const BakedNote& e, const std::int64_t v) {
                                           return e.absSample < v;
                                       });
            for (; it != plan.notes.end() && it->absSample < segEnd; ++it)
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
