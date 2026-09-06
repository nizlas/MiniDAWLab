#pragma once

// =============================================================================
// midi_dependency — the ONE authoritative reverse enumeration of MIDI sources feeding an
// instrument destination (R3, PID-010 Locked; steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md
// §8.1–8.2). P1C slice.
// =============================================================================
// Dependency model (Verified, Audit §5.1): strictly ONE hop — a `TrackKind::Midi` row carries a
// persisted `midiTo` (`midiDestinationTrackId`) pointing at exactly one `TrackKind::Instrument`
// row. Only Midi rows can be sources and only Instrument rows are legal destinations (enforced at
// every mutation point / snapshot repair), so transitive chains and cycles are STRUCTURALLY
// impossible — the cycle policy is therefore "cannot occur by construction", and this enumerator
// additionally refuses the degenerate self/kind-violating rows defensively rather than following
// them (a hand-edited file that survived repair must never create a loop here).
//
// Ordering contract (Locked, F9): sources return in SESSION TRACK ORDER — the same order the live
// merge uses (destination's own content schedules first, then Midi sources in session order;
// `PlaybackEngine`/`InstrumentRuntimeCoordinator` merge, steering §8.3). Session order is
// semantic data (equal-time CC last-wins depends on it, HR-4) and is fingerprinted.
//
// Eligibility (F8) is returned as DATA (`trackOff`, `muted`), never filtered away here: the
// canonical fingerprint includes every routed source with its eligibility flags (steering §11.1
// F7/F8), and callers that need delivered-content questions (e.g. silent-generation eligibility)
// apply the flags themselves.
// =============================================================================

#include "domain/SessionSnapshot.h"
#include "domain/Track.h"

#include <vector>

namespace midi_dependency
{
    /// One routed MIDI source row feeding a destination (F7/F8 inputs).
    struct SourceRef
    {
        TrackId trackId = kInvalidTrackId;
        /// Position in the session snapshot's track list — the merge/fingerprint order key (F9).
        int sessionTrackIndex = -1;
        /// `kTrackMidiOutputChannelAny` or the fixed 1…16 remap channel (effective-channel input).
        int midiOutputChannel = 0;
        /// Eligibility gating MIDI delivery (F8): powered off lane.
        bool trackOff = false;
        /// Eligibility gating MIDI delivery (F8): muted lane.
        bool muted = false;
    };

    /// The authoritative reverse enumeration (R3): every `TrackKind::Midi` row whose
    /// `midiDestinationTrackId` equals `destination`, in session track order. Returns empty when
    /// `destination` is not a `TrackKind::Instrument` row of `session` (never a partial answer).
    /// The destination's own local events are NOT in this list — the destination-first merge rule
    /// is the caller's contract (snapshot builder serializes destination content first).
    [[nodiscard]] inline std::vector<SourceRef> sourcesForDestination(const SessionSnapshot& session,
                                                                      const TrackId destination)
    {
        std::vector<SourceRef> out;
        const int destIndex = session.findTrackIndexById(destination);
        if (destIndex < 0 || session.getTrack(destIndex).getKind() != TrackKind::Instrument)
        {
            return out;
        }
        const int n = session.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            const Track& t = session.getTrack(i);
            if (t.getKind() != TrackKind::Midi)
            {
                continue; // Only Midi rows can be MIDI sources (one-hop model, §8.1).
            }
            if (t.getMidiDestinationTrackId() != destination)
            {
                continue;
            }
            if (t.getId() == destination)
            {
                continue; // Defensive: a self-loop row is structurally invalid; never follow it.
            }
            SourceRef ref;
            ref.trackId = t.getId();
            ref.sessionTrackIndex = i;
            ref.midiOutputChannel = t.getMidiOutputChannel();
            ref.trackOff = t.isTrackOff();
            ref.muted = t.isMuted();
            out.push_back(ref);
        }
        return out;
    }
} // namespace midi_dependency
