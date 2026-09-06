#pragma once

// =============================================================================
// ProxyRenderSnapshot — immutable render-relevant snapshot of one instrument destination
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §10; P1C slice, R4 data shape)
// =============================================================================
// Built on the MESSAGE THREAD, then never mutated. The snapshot is self-sufficient: it deep-copies
// every piece of musical content and holds NO references to live tracks, routing containers,
// controllers, editors, or plugin instances (PI-011 isolation; safe cancellation PI-013). Musical
// data stays in its NATIVE PERSISTED DOMAINS (§10.1): notes/CC in ticks, clip anchors/windows in
// samples at the recorded timeline reference rate (renderConfig.timelineReferenceRate, F11).
// Re-baking to render-rate samples happens at render time (P1D) via the §10.1 conversion rules —
// never here, and live-rate baked samples are never reused (HR-9).
//
// The plugin state blob is RENDER CONTENT only (F2 content). Its identity component in the
// fingerprint is the host-managed state revision + persisted save-pairing (hybrid contract §9.4,
// revision 5) — raw blob bytes are never serialized into the fingerprint.
// =============================================================================

#include "domain/TimelineDomain.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h" // InstrumentMidiClip (plain data)
#include "instruments/MidiDependencyEnumeration.h"
#include "io/ProjectFile.h"                        // ProjectFileGenericVst3DescriptorV1 (F1)
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace proxy_snapshot
{
    /// F5 — one CC point (tick domain, repository-canonical normalized order preserved).
    struct SnapshotCcPoint
    {
        std::int64_t startTick = 0;
        int controller = 0;
        int value = 0;
        int channel = 1;
        int interpolationToNext = 0; // 0 = hold, 1 = linear (persisted encoding)
    };

    /// F4 — one note (tick domain, stored vector order preserved: the ORD-1 equal-time tie-break).
    struct SnapshotNote
    {
        int midiNote = 60;
        int velocity = 100;
        int offVelocity = 64;
        int channel = 1;
        std::int64_t startTick = 0;
        std::int64_t durationTicks = 0;
    };

    /// F3 — one MIDI clip: sample-domain integers serialize raw (interpretation rate is F11's
    /// timeline reference rate); bpm/tpq are the tick→sample conversion inputs.
    struct SnapshotClip
    {
        std::uint64_t clipId = 0;
        std::int64_t startSamples = 0;
        std::int64_t timelineAnchorSamples = 0;
        std::int64_t lengthSamples = 0;
        double bpm = 110.0;
        int ticksPerQuarter = 960;
        std::vector<SnapshotNote> notes;
        std::vector<SnapshotCcPoint> ccPoints;
    };

    /// F7/F8/F9 — one routed MIDI source with its content and eligibility, in session order.
    struct SnapshotSource
    {
        TrackId trackId = kInvalidTrackId;
        int midiOutputChannel = 0;
        bool trackOff = false;
        bool muted = false;
        std::vector<SnapshotClip> clips;
    };

    /// F1/F1v — plugin identity.
    struct SnapshotPluginIdentity
    {
        juce::String fileOrIdentifier; // path; fingerprint normalizes to forward slashes
        int uniqueId = 0;
        int deprecatedUid = 0;
        juce::String format;    // e.g. "VST3"
        bool isInstrument = true;
        juce::String version;   // F1v (v20 persisted / live PluginDescription::version)
    };

    /// F2 identity component — hybrid contract (§9.4.2): host-managed monotonic revision plus the
    /// persisted proxy↔saved-state pairing. Raw blob bytes are content, never identity.
    struct SnapshotStateIdentity
    {
        std::uint64_t primaryStateRevision = 0;
        bool pairedWithSavedState = false;
    };

    /// F10/F11 — render configuration. `timelineReferenceRate` records the rate under which the
    /// snapshot's sample-domain integers are interpreted (TLD-1, §10.1).
    struct SnapshotRenderConfig
    {
        double renderSampleRate = 48000.0;
        int renderBlockSize = 512;      // Locked initial default (steering §15.4, revision 6)
        double timelineReferenceRate = 48000.0;
        int noteOffGateMs = 100;        // F10 gate rule input (Verified)
    };

    /// F12/F13 — policy versions (Locked inclusions).
    struct SnapshotPolicies
    {
        int latencyPolicyVersion = 1;
        int tailPolicyVersion = 1;
        int renderPolicyVersion = 1;
        int proxyFormatVersion = 1;
    };

    /// PID-007 span + §15.7 silent-generation eligibility inputs (both fingerprinted).
    struct SnapshotSpanAndSilence
    {
        /// Last relevant delivered event end (note end incl. gate-less explicit duration, or CC
        /// point) across destination content and ELIGIBLE sources, in TIMELINE REFERENCE samples.
        /// 0 when no host-scheduled events exist. The render span is project start (0) → this
        /// point + detected tail (tail found at render time, P1D).
        std::int64_t lastRelevantEventReferenceSample = 0;
        /// True when the destination's delivered content (own clips + eligible sources) contains
        /// at least one note or CC point.
        bool hasHostScheduledEvents = false;
        /// Explicit compatibility classification input: true ONLY when the instrument is known to
        /// produce sound exclusively from DAL-host-scheduled events (§15.7). Default false —
        /// autonomous generators / plugin-internal sequencers are UNKNOWN, and empty MIDI input
        /// alone must never be treated as proof of silence.
        bool instrumentClassifiedHostEventDriven = false;
        /// The conservative verdict: the silent-generation fast path (no WAV) is allowed only for
        /// an event-empty destination on an explicitly host-event-driven-classified instrument.
        /// False ⇒ a normal full-span render is required (honest handling of the unknown).
        bool silentGenerationEligible = false;
    };

    /// The immutable snapshot (steering §10 draft shape). Everything below is a deep copy.
    struct ProxyRenderSnapshot
    {
        TrackId destinationTrackId = kInvalidTrackId;
        SnapshotPluginIdentity pluginIdentity;
        SnapshotStateIdentity stateIdentity;
        /// F2 CONTENT for the render instance (restore before prepare). Never fingerprinted.
        juce::MemoryBlock pluginStateBlob;
        /// Destination-local clips in STORED order (the fingerprint serializer derives the plan
        /// order — stable sort by startSamples — from this, §11.4).
        std::vector<SnapshotClip> destinationClips;
        int destinationMidiOutputChannel = 0; // F6
        /// Routed sources in session order (F9), each with full content + eligibility.
        std::vector<SnapshotSource> sources;
        SnapshotRenderConfig renderConfig;
        SnapshotPolicies policies;
        SnapshotSpanAndSilence spanAndSilence;
    };

    /// Inputs the builder cannot read from the session snapshot itself.
    struct BuildInputs
    {
        SnapshotPluginIdentity pluginIdentity;
        SnapshotStateIdentity stateIdentity;
        juce::MemoryBlock pluginStateBlob;
        SnapshotRenderConfig renderConfig;
        SnapshotPolicies policies;
        bool instrumentClassifiedHostEventDriven = false;
    };

    namespace detail
    {
        [[nodiscard]] inline SnapshotClip copyClip(const InstrumentMidiClip& c)
        {
            SnapshotClip out;
            out.clipId = c.id;
            out.startSamples = c.startSamples;
            out.timelineAnchorSamples = c.timelineAnchorSamples;
            out.lengthSamples = c.lengthSamples;
            out.bpm = c.pattern.bpm;
            out.ticksPerQuarter = c.pattern.ticksPerQuarter;
            out.notes.reserve(c.pattern.timelineNotes.size());
            for (const auto& n : c.pattern.timelineNotes)
            {
                SnapshotNote sn;
                sn.midiNote = n.midiNote;
                sn.velocity = n.velocity;
                sn.offVelocity = n.offVelocity;
                sn.channel = (int)n.channel;
                sn.startTick = n.startTick;
                sn.durationTicks = n.durationTicks;
                out.notes.push_back(sn);
            }
            out.ccPoints.reserve(c.pattern.ccPoints.size());
            for (const auto& p : c.pattern.ccPoints)
            {
                SnapshotCcPoint sp;
                sp.startTick = p.startTick;
                sp.controller = (int)p.controller;
                sp.value = (int)p.value;
                sp.channel = (int)p.channel;
                sp.interpolationToNext = (p.interpolationToNext == MidiCcInterpolation::linear) ? 1 : 0;
                out.ccPoints.push_back(sp);
            }
            return out;
        }

        /// Last event end of one clip in timeline-reference samples (ticks bake at the reference
        /// rate against the clip's reference-domain anchor — consistent single domain, §10.1).
        [[nodiscard]] inline std::int64_t clipLastEventReferenceSample(const SnapshotClip& c,
                                                                       const double referenceRate)
        {
            std::int64_t last = 0;
            const double bpm = (c.bpm > 0.0) ? c.bpm : 110.0;
            const int tpq = (c.ticksPerQuarter > 0) ? c.ticksPerQuarter : 960;
            for (const auto& n : c.notes)
            {
                const std::int64_t endTick = n.startTick + ((n.durationTicks > 0) ? n.durationTicks : 1);
                const std::int64_t endRef = c.timelineAnchorSamples
                                            + ticksToRelativeSamples(endTick, bpm, tpq, referenceRate);
                if (endRef > last)
                {
                    last = endRef;
                }
            }
            for (const auto& p : c.ccPoints)
            {
                const std::int64_t atRef = c.timelineAnchorSamples
                                           + ticksToRelativeSamples(p.startTick, bpm, tpq, referenceRate);
                if (atRef > last)
                {
                    last = atRef;
                }
            }
            return last;
        }
    } // namespace detail

    /// Message-thread builder. `clipsForTrack` supplies the stored (append/load-ordered) MIDI
    /// clips of a track id — destination-local clips for the destination id, and each routed
    /// source's clips (MidiContent controllers). Returned pointers are only dereferenced during
    /// this call (deep copy); the snapshot holds nothing live afterwards.
    [[nodiscard]] inline ProxyRenderSnapshot
        buildProxyRenderSnapshot(const SessionSnapshot& session,
                                 const TrackId destinationTrackId,
                                 const std::function<std::vector<const InstrumentMidiClip*>(TrackId)>& clipsForTrack,
                                 const BuildInputs& in)
    {
        ProxyRenderSnapshot snap;
        snap.destinationTrackId = destinationTrackId;
        snap.pluginIdentity = in.pluginIdentity;
        snap.stateIdentity = in.stateIdentity;
        snap.pluginStateBlob = in.pluginStateBlob;
        snap.renderConfig = in.renderConfig;
        snap.policies = in.policies;
        snap.spanAndSilence.instrumentClassifiedHostEventDriven = in.instrumentClassifiedHostEventDriven;

        const int destIndex = session.findTrackIndexById(destinationTrackId);
        if (destIndex >= 0)
        {
            snap.destinationMidiOutputChannel = session.getTrack(destIndex).getMidiOutputChannel();
        }

        if (clipsForTrack)
        {
            for (const InstrumentMidiClip* c : clipsForTrack(destinationTrackId))
            {
                if (c != nullptr)
                {
                    snap.destinationClips.push_back(detail::copyClip(*c));
                }
            }
        }

        // R3: the one authoritative reverse enumeration, session order (F7/F9); eligibility as
        // data (F8) — ineligible sources keep their content in the snapshot/fingerprint.
        for (const auto& src : midi_dependency::sourcesForDestination(session, destinationTrackId))
        {
            SnapshotSource s;
            s.trackId = src.trackId;
            s.midiOutputChannel = src.midiOutputChannel;
            s.trackOff = src.trackOff;
            s.muted = src.muted;
            if (clipsForTrack)
            {
                for (const InstrumentMidiClip* c : clipsForTrack(src.trackId))
                {
                    if (c != nullptr)
                    {
                        s.clips.push_back(detail::copyClip(*c));
                    }
                }
            }
            snap.sources.push_back(std::move(s));
        }

        // Span + silent-generation inputs (PID-007, §15.7): delivered content = destination-local
        // clips (destination mute/off is a playback gate, PID-006) + ELIGIBLE sources only.
        const double refRate = timeline_domain::isValidRate(snap.renderConfig.timelineReferenceRate)
                                   ? snap.renderConfig.timelineReferenceRate
                                   : 48000.0;
        std::int64_t lastRef = 0;
        bool anyEvents = false;
        const auto scanClips = [&lastRef, &anyEvents, refRate](const std::vector<SnapshotClip>& clips) {
            for (const auto& c : clips)
            {
                if (!c.notes.empty() || !c.ccPoints.empty())
                {
                    anyEvents = true;
                    const std::int64_t e = detail::clipLastEventReferenceSample(c, refRate);
                    if (e > lastRef)
                    {
                        lastRef = e;
                    }
                }
            }
        };
        scanClips(snap.destinationClips);
        for (const auto& s : snap.sources)
        {
            if (!s.trackOff && !s.muted)
            {
                scanClips(s.clips);
            }
        }
        snap.spanAndSilence.lastRelevantEventReferenceSample = lastRef;
        snap.spanAndSilence.hasHostScheduledEvents = anyEvents;
        // Conservative verdict (§15.7): the no-WAV fast path needs BOTH an event-empty delivered
        // content AND an explicit host-event-driven classification. Autonomous-output uncertainty
        // (default) always forces the normal full-span render path.
        snap.spanAndSilence.silentGenerationEligible
            = !anyEvents && in.instrumentClassifiedHostEventDriven;
        return snap;
    }
} // namespace proxy_snapshot
