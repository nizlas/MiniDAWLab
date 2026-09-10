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

} // namespace instrument_playback
