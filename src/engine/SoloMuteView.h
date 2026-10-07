#pragma once

// =============================================================================
// SoloMuteView — immutable derived Solo listening decision for the audio engine
// =============================================================================
//
// MODEL (spec §1/§5)
//   Solo is a temporary LISTENING layer on top of the stored Mute flags. `Session` owns the five
//   explicit solo sets (one temporary + four memories); the app derives this view from the
//   CURRENT set and the CURRENT `SessionSnapshot` on the **message thread**
//   (`deriveSoloMuteView`) and publishes it to `PlaybackEngine` as an immutable value behind an
//   atomic shared_ptr (same release/acquire discipline as the session snapshot itself). The audio
//   callback acquires ONE view per block — a consistent picture at the block boundary — and only
//   performs allocation-free binary-search lookups. Stored `Track::isMuted()` flags are NEVER
//   rewritten; project save always writes the stored flags.
//
// DECISION PER TRACK while solo is active (derived from the actual routing model):
//   • ForcedAudible  — explicitly soloed sources (heard even if base-muted) and every bus the
//     soloed signal needs downstream: the main-output chain to Stereo Out plus enabled send
//     destinations and THEIR chains. Also the destination Instrument lane of a soloed MIDI
//     track (carrier). These strips pass signal even when base-muted; Off still wins.
//   • PassStoredMute — tracks included INDIRECTLY as upstream feeders of a soloed Group bus
//     (including send paths into it, transitively). They keep their stored Mute behavior: solo
//     on a Group means "hear what the group currently plays", so a base-muted feeder stays
//     silent and derived pass-through never marks an S button red (spec §4).
//   • ForcedSilent   — everything else: effective gain 0 at that track's own strip, exactly like
//     Mute (inserts/instruments keep processing — the 1.1.9 "never skip the host" rule — so
//     un-soloing instantly restores the base Mute picture with no restart glitches).
//   Because every unrelated source is zeroed at its OWN strip, a shared Group/FX bus marked
//   ForcedAudible carries only soloed signal — no leakage of unrelated sources (spec §5).
//
// MIDI GATING (spec §5, MIDI): audio-strip zeroing cannot separate several MIDI sources feeding
//   ONE Instrument strip, so the view also carries two suppression sets consumed by
//   `InstrumentTrackController::audioThread_scheduleTransportMidiForSegment`:
//   • suppressOwnTransportClips — Instrument lanes whose strip is open only as a CARRIER for a
//     soloed MIDI source: their own timeline clips must not leak into the audible mix.
//   • suppressRoutedMidiSources — MIDI lanes routed into an open destination strip whose events
//     must not sound (another source into the same instrument is soloed).
//   Suppression uses the existing pending-note-off ownership (flush first, then skip new events):
//   no hanging notes and never a global All Notes Off that would cut other audible sources.
//   Instruments whose strip is ForcedSilent are NOT gated — they keep consuming their MIDI with
//   output zeroed (mute semantics), so clearing solo restores them mid-note like un-muting.
//
// STALENESS: the view carries no pointers into the snapshot, only TrackIds. If the session
//   changes before the app re-derives (it re-derives on every solo command and when it observes a
//   new snapshot pointer), a track missing from the map defaults to ForcedSilent while solo is
//   active — a brand-new or re-routed track can be briefly over-silenced, never falsely audible.
// =============================================================================

#include "domain/Track.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class SessionSnapshot;

enum class SoloTrackAudioDecision : std::uint8_t
{
    ForcedSilent = 0,
    ForcedAudible = 1,
    PassStoredMute = 2,
};

struct SoloMuteView
{
    /// False ⇒ the whole view is a no-op (stored Mute applies everywhere, no MIDI gating).
    bool soloActive = false;
    /// Sorted by TrackId (binary search on the audio thread). Tracks absent from this list are
    /// ForcedSilent while `soloActive` (see STALENESS above).
    std::vector<std::pair<TrackId, SoloTrackAudioDecision>> audioDecisions;
    /// Sorted TrackIds of Instrument lanes whose OWN timeline clips are suppressed (carrier case).
    std::vector<TrackId> suppressOwnTransportClips;
    /// Sorted TrackIds of MIDI lanes whose routed events are suppressed at the destination.
    std::vector<TrackId> suppressRoutedMidiSources;
};

namespace solo_mute_view
{

/// [Message thread] Pure derivation — see the header comment for the decision rules.
/// `currentSoloSet` is the raw current set (`Session::getCurrentSoloSetTrackIds`); unknown ids and
/// the Master row are ignored here, so stale references can never produce a false-active solo.
/// Returns a view with `soloActive == false` when the effective set is empty.
[[nodiscard]] std::shared_ptr<const SoloMuteView> deriveSoloMuteView(
    const SessionSnapshot& snapshot,
    const std::vector<TrackId>& currentSoloSet);

/// [Audio thread] Effective mute for one track strip under `view` (nullptr / inactive ⇒ stored).
[[nodiscard]] inline bool effectiveTrackMuted(const SoloMuteView* view, const Track& track) noexcept
{
    if (view == nullptr || !view->soloActive)
    {
        return track.isMuted();
    }
    const TrackId id = track.getId();
    // Branch-free-ish lower_bound over a small sorted vector; no allocation, no locks.
    const auto& v = view->audioDecisions;
    std::size_t lo = 0;
    std::size_t hi = v.size();
    while (lo < hi)
    {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        if (v[mid].first < id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo >= v.size() || v[lo].first != id)
    {
        return true; // Unknown while solo active: ForcedSilent (safe default).
    }
    switch (v[lo].second)
    {
    case SoloTrackAudioDecision::ForcedAudible:
        return false;
    case SoloTrackAudioDecision::PassStoredMute:
        return track.isMuted();
    case SoloTrackAudioDecision::ForcedSilent:
    default:
        return true;
    }
}

[[nodiscard]] inline bool sortedIdListContains(const std::vector<TrackId>& v, const TrackId id) noexcept
{
    std::size_t lo = 0;
    std::size_t hi = v.size();
    while (lo < hi)
    {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        if (v[mid] < id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo < v.size() && v[lo] == id;
}

/// [Audio thread] True when `instrumentTrackId`'s OWN timeline clips must be suppressed.
[[nodiscard]] inline bool transportClipsSuppressedBySolo(const SoloMuteView* view,
                                                         const TrackId instrumentTrackId) noexcept
{
    return view != nullptr && view->soloActive
           && sortedIdListContains(view->suppressOwnTransportClips, instrumentTrackId);
}

/// [Audio thread] True when the MIDI lane `sourceTrackId`'s routed events must be suppressed.
[[nodiscard]] inline bool routedMidiSourceSuppressedBySolo(const SoloMuteView* view,
                                                           const TrackId sourceTrackId) noexcept
{
    return view != nullptr && view->soloActive
           && sortedIdListContains(view->suppressRoutedMidiSources, sourceTrackId);
}

/// [Audio thread] True when solo FORCES `trackId` audible (explicit solo / needed carrier). The
/// instrument MIDI scheduler uses this to override the stored-mute part of its own gate: a
/// base-muted but explicitly soloed Instrument/Midi lane must deliver its events (spec §2) even
/// though `InstrumentTrackController` bakes `muted_` into `playbackEnabled`. Stored mute flags
/// stay untouched; clearing solo restores the baked gate on the next block.
[[nodiscard]] inline bool trackForcedAudibleBySolo(const SoloMuteView* view, const TrackId trackId) noexcept
{
    if (view == nullptr || !view->soloActive)
    {
        return false;
    }
    const auto& v = view->audioDecisions;
    std::size_t lo = 0;
    std::size_t hi = v.size();
    while (lo < hi)
    {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        if (v[mid].first < trackId)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo < v.size() && v[lo].first == trackId
           && v[lo].second == SoloTrackAudioDecision::ForcedAudible;
}

} // namespace solo_mute_view
