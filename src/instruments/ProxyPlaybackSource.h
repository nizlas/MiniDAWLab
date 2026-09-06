#pragma once

// =============================================================================
// ProxyPlaybackSource — P1G authoritative playback-source selection
// (steering §7.3 source priority, §12.3 currency rules, PI-030)
// =============================================================================
//
// One authoritative per-destination selector with the canonical transport
// priority: (1) Primary live when available and usable; (2) current Primary
// proxy when Primary is unavailable; (3) Secondary live — reserved for P2,
// structurally absent here; (4) missing/silent placeholder.
//
// CURRENCY (never guessed): a generation is Current iff the freshly recomputed
// expected fingerprint equals its `generationId`.
//   * Primary present: the fingerprint is computed from LIVE inputs (live plugin
//     description + live semantic revision + current musical content) — identical
//     inputs to publication, so equality covers musical content, plugin identity
//     AND the revision pairing in one compare.
//   * Primary missing (the portable case): the fingerprint is recomputed UNDER
//     THE GENERATION'S RECORDED RENDER CONFIGURATION (§12.3) from the persisted
//     v20 identity fields, plus the persisted save-pairing gate (see
//     `proxyStatePairingHolds`). Metadata without recorded identity (pre-P1G)
//     conservatively evaluates to Stale — never approximated.
// NOT used, by contract: latest filename, mere metadata presence, project dirty
// state, device/engine rate, or a fresh plugin-state blob hash.
//
// The selector's outputs are IMMUTABLE view objects published to the host via
// atomic shared_ptr (the verified `activeOwner_` pattern): the audio thread
// latches one view per block, so source changes only ever take effect at a
// safe block boundary and Primary/Proxy can never mix within a block.

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "domain/Track.h"
#include "instruments/ProxyFingerprint.h"
#include "instruments/ProxyPlaybackReader.h"
#include "instruments/ProxyRenderSnapshot.h"
#include "io/ProjectFile.h"

class InstrumentMidiClip;
class SessionSnapshot;

namespace proxy_playback
{

//==============================================================================
// Runtime source/status value (immutable; suitable for P1I status UI)
//==============================================================================
enum class ProxyPlaybackSourceState : int
{
    Primary = 0,      ///< live Primary selected (substitution branch not taken)
    ProxyPreparing,   ///< proxy selected; derived representation not resident yet
    ProxyCurrent,     ///< proxy selected and current (silent generation included)
    ProxyStale,       ///< retained but NEVER selected in P1
    ProxyMissing,     ///< current metadata but the asset file is missing
    ProxyCorrupt,     ///< invalid path / unreadable WAV / metadata mismatch / stream failure
    MissingPrimary,   ///< no Primary and no usable proxy — honest silence
    PlaybackUnderrun, ///< proxy selected; real pre-EOF underrun observed
};

[[nodiscard]] inline const char* proxyPlaybackSourceStateName(const ProxyPlaybackSourceState s)
{
    switch (s)
    {
        case ProxyPlaybackSourceState::Primary: return "Primary";
        case ProxyPlaybackSourceState::ProxyPreparing: return "ProxyPreparing";
        case ProxyPlaybackSourceState::ProxyCurrent: return "ProxyCurrent";
        case ProxyPlaybackSourceState::ProxyStale: return "ProxyStale";
        case ProxyPlaybackSourceState::ProxyMissing: return "ProxyMissing";
        case ProxyPlaybackSourceState::ProxyCorrupt: return "ProxyCorrupt";
        case ProxyPlaybackSourceState::MissingPrimary: return "MissingPrimary";
        case ProxyPlaybackSourceState::PlaybackUnderrun: return "PlaybackUnderrun";
    }
    return "?";
}

/// Immutable per-destination playback view. Published to the host through an
/// atomic shared_ptr; the audio thread loads it ONCE per block (block-boundary
/// latch). `useProxy == true` replaces ONLY the instrument-generation stage —
/// everything downstream (DAL inserts, fader, pan, meters, routing, mixdown)
/// stays the shared strip. A silent generation has `useProxy` true and a null
/// reader (zeros by definition, §15.7).
struct ProxyPlaybackView
{
    ProxyPlaybackSourceState selectedState = ProxyPlaybackSourceState::MissingPrimary;
    bool useProxy = false;
    bool silentGeneration = false;
    std::shared_ptr<ProxyPlaybackReader> reader; ///< null for silent generations
    juce::String generationId;                   ///< diagnostics (message-thread reads only)
};

//==============================================================================
// Currency: expected fingerprint under the generation's RECORDED configuration
//==============================================================================
/// [Message thread] §12.3 recomputation for the missing-Primary case: identical
/// BuildInputs to production capture, except every non-musical input (plugin
/// identity, state revision, render config, pairing byte) comes from the
/// generation's persisted v20 record instead of the live host. Returns an empty
/// string when the metadata carries no recorded identity (pre-P1G metadata) —
/// callers MUST treat that as not-verifiable (Stale), never as a match.
[[nodiscard]] inline juce::String computeExpectedFingerprintUnderRecordedConfig(
    const SessionSnapshot& sessionSnap,
    const TrackId destination,
    const std::function<std::vector<const InstrumentMidiClip*>(TrackId)>& clipsForTrack,
    const ProjectFileProxyMetadataV20& meta,
    const double fallbackTimelineRate)
{
    if (meta.pluginFileOrIdentifier.isEmpty() || meta.sampleRate <= 0.0)
    {
        return {};
    }
    proxy_snapshot::BuildInputs in;
    in.pluginIdentity.fileOrIdentifier = meta.pluginFileOrIdentifier;
    in.pluginIdentity.uniqueId = meta.pluginUniqueId;
    in.pluginIdentity.deprecatedUid = meta.pluginDeprecatedUid;
    in.pluginIdentity.format = meta.pluginFormatName;
    in.pluginIdentity.isInstrument = meta.pluginIsInstrument;
    in.pluginIdentity.version = meta.pluginVersionAtRender;
    in.stateIdentity.primaryStateRevision = (std::uint64_t)meta.primaryStateRevisionAtPublish;
    in.stateIdentity.pairedWithSavedState = meta.pairedWithSavedStateAtRender;
    in.renderConfig.renderSampleRate = meta.sampleRate;
    in.renderConfig.renderBlockSize = meta.renderBlockSize;
    in.renderConfig.timelineReferenceRate
        = meta.timelineReferenceRate > 0.0 ? meta.timelineReferenceRate : fallbackTimelineRate;
    in.renderConfig.noteOffGateMs = meta.noteOffGateMs;
    // Production constant (§15.7 conservative default) — matches capture-time inputs.
    in.instrumentClassifiedHostEventDriven = false;
    const proxy_snapshot::ProxyRenderSnapshot snap
        = proxy_snapshot::buildProxyRenderSnapshot(sessionSnap, destination, clipsForTrack, in);
    return proxy_fingerprint::computeFingerprint(snap);
}

/// [Message thread] §12.3 persisted save-pairing: with Primary missing, the
/// generation's state component is trusted iff the plugin state blob persisted
/// in the SAME project save is the state the generation rendered. Evidence:
/// the save stamped the live revision (`primaryStateRevisionAtSave`) and it
/// equals the publication revision — or the publication happened in THIS
/// session (the runtime flag), where the in-memory pairing is direct.
[[nodiscard]] inline bool proxyStatePairingHolds(const ProjectFileProxyMetadataV20& meta,
                                                 const bool publishedThisSession) noexcept
{
    if (publishedThisSession)
    {
        return true;
    }
    return meta.primaryStateRevisionAtPublish != 0
           && meta.primaryStateRevisionAtSave == meta.primaryStateRevisionAtPublish;
}

//==============================================================================
// Pure selection decision (deterministic; unit-tested without any runtime)
//==============================================================================
enum class ProxyCurrencyVerdict : int
{
    Current = 0,
    Stale,          ///< fingerprint mismatch, broken pairing, or unverifiable identity
    NoMetadata,     ///< destination has no published generation at all
};

enum class ProxyAssetAvailability : int
{
    SilentGeneration = 0, ///< valid metadata-only generation (zeros, no WAV)
    Available,            ///< asset opened and validated
    Missing,              ///< referenced file does not exist
    Corrupt,              ///< unsafe path / unreadable / mismatching WAV
};

struct ProxySourceDecision
{
    ProxyPlaybackSourceState state = ProxyPlaybackSourceState::MissingPrimary;
    bool useProxy = false;
};

/// Canonical priority (steering §7.3). Secondary does not exist in P1 — there is
/// deliberately NO fallback between (2) and (4). A stale/missing/corrupt proxy is
/// never selected and never approximated.
[[nodiscard]] inline ProxySourceDecision
    decideProxyPlaybackSource(const bool primaryUsable,
                              const ProxyCurrencyVerdict currency,
                              const ProxyAssetAvailability asset) noexcept
{
    if (primaryUsable)
    {
        return { ProxyPlaybackSourceState::Primary, false };
    }
    if (currency == ProxyCurrencyVerdict::NoMetadata)
    {
        return { ProxyPlaybackSourceState::MissingPrimary, false };
    }
    if (currency == ProxyCurrencyVerdict::Stale)
    {
        return { ProxyPlaybackSourceState::ProxyStale, false };
    }
    // Current generation:
    switch (asset)
    {
        case ProxyAssetAvailability::SilentGeneration:
        case ProxyAssetAvailability::Available:
            return { ProxyPlaybackSourceState::ProxyCurrent, true };
        case ProxyAssetAvailability::Missing:
            return { ProxyPlaybackSourceState::ProxyMissing, false };
        case ProxyAssetAvailability::Corrupt:
            return { ProxyPlaybackSourceState::ProxyCorrupt, false };
    }
    return { ProxyPlaybackSourceState::MissingPrimary, false };
}

} // namespace proxy_playback
