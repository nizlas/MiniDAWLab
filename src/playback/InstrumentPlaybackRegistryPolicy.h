#pragma once

// =============================================================================
// InstrumentPlaybackRegistryPolicy — pure eligibility rule for the experimental
// instrument playback registry (one rule, used by BOTH registry publishers:
// ExperimentalInstrumentPlaybackBridge and InstrumentRuntimeCoordinator).
// =============================================================================
//
// P1 missing-Primary correction (§7.3 source priority, §12.2 proxy substitution):
// eligibility MUST NOT depend on whether the destination's plugin is currently
// loaded. The PlaybackEngine only schedules timeline segments and invokes
// `audioThread_processBlockAndAddToOutputs` for REGISTERED entries, and that call
// is the ONLY path through which a published proxy playback view can produce
// sound. The former rule ("skip generic-catalog lanes without a loaded plugin",
// pre-proxy, aec2441) therefore silenced a valid Current proxy on any machine
// where the Primary failed to load — the exact two-computer portable case —
// while the Inspector honestly reported "Playing: Proxy current".
//
// A registered entry with neither a loaded instrument nor a proxy view stays
// inert: the host's audio-thread entry point discards the block's MIDI and
// returns without touching the outputs.

#include "domain/Track.h"

namespace instrument_playback
{

/// True when a (host, controller) runtime pair must be registered for playback.
/// `isGenericCatalogInstrument` and `hasLoadedInstrument` are deliberately
/// accepted and IGNORED: they are the exact inputs the former (buggy) rule
/// consulted; keeping them in the signature lets the selftests lock that they
/// can never regain influence over registration.
[[nodiscard]] constexpr bool playbackEntryEligible(const bool hasInstrumentTrack,
                                                   const bool isGenericCatalogInstrument,
                                                   const bool hasLoadedInstrument,
                                                   const TrackId playbackKey) noexcept
{
    (void)isGenericCatalogInstrument;
    (void)hasLoadedInstrument;
    return hasInstrumentTrack && playbackKey != kInvalidTrackId;
}

/// P2 Secondary audition (steering §17 audition split, PID-008): whether a loaded Secondary rides
/// along in the track's playback entry as its AUDITION host — rendered through the same strip as
/// the transport host, and only while the transport is not playing (the engine enforces that part
/// per block, so live audition is never layered over transport playback).
///
/// Two situations qualify:
/// * **Primary missing** — the documented fallback: stopped audition of newly played notes has no
///   other instrument to sound through.
/// * **Primary loaded, but the user has the Secondary's own plug-in editor open** — sound design in
///   that editor must be audible, otherwise a Secondary can only ever be dialled in on a machine
///   where the Primary happens to be unavailable. Exactly one instrument is auditioned either way:
///   DAL's own UI/editor notes keep going to the Primary, so the authoritative and the approximate
///   sound are never combined.
///
/// A Secondary that IS the transport source is never an audition host — it already renders as the
/// entry's transport host, and rendering it twice would double its level.
[[nodiscard]] constexpr bool secondaryAuditionHostEligible(const bool secondaryLoaded,
                                                           const bool secondaryIsTransportSource,
                                                           const bool primaryHasLoadedInstrument,
                                                           const bool secondaryEditorOpen) noexcept
{
    if (!secondaryLoaded || secondaryIsTransportSource)
    {
        return false;
    }
    return !primaryHasLoadedInstrument || secondaryEditorOpen;
}

// Locks the rule above at compile time (pure predicate: a regression cannot reach a test run).
static_assert(!secondaryAuditionHostEligible(false, false, false, true), "unloaded Secondary never auditions");
static_assert(!secondaryAuditionHostEligible(true, true, false, true), "transport-source Secondary is not an audition host");
static_assert(!secondaryAuditionHostEligible(true, true, true, false), "transport-source Secondary is not an audition host");
static_assert(secondaryAuditionHostEligible(true, false, false, false), "Primary missing: stopped audition works without the editor");
static_assert(secondaryAuditionHostEligible(true, false, false, true), "Primary missing + editor open still auditions");
static_assert(secondaryAuditionHostEligible(true, false, true, true), "Primary loaded: the open Secondary editor is audible");
static_assert(!secondaryAuditionHostEligible(true, false, true, false), "Primary loaded, editor closed: Primary alone sounds");

} // namespace instrument_playback
