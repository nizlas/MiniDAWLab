#pragma once

// =============================================================================
// SoloUiHooks — app→UI seam for the Solo feature (headers, mixer strips)
// =============================================================================
// The app layer (`SoloCoordinator`) owns the solo commands and the derived
// `SoloMuteView`; the arrangement headers and the mixer strips only DISPLAY the
// per-track state and forward S clicks. Both views consume this same seam so
// their S/M rendering can never drift apart. Unwired hooks (early startup,
// focused UI tests) simply hide the S cell and leave Mute behavior unchanged.
//
// THREADING: [Message thread] only — called from model providers during paint
// and from click dispatch.
// =============================================================================

#include "domain/Track.h"

#include <functional>

/// Per-track display state derived from the CURRENT solo set and the published
/// `SoloMuteView` (never from stored Mute flags — those stay untouched by solo).
struct TrackSoloDisplayState
{
    /// Explicit member of the current solo set → red S. Derived pass-through
    /// (e.g. a bus that merely carries a soloed source) is never marked.
    bool soloed = false;
    /// Silenced BY SOLO while not stored-muted → the M face shows the distinct
    /// dimmed tint instead of the neutral face.
    bool soloSilenced = false;
    /// Solo active anywhere → Mute edits are locked in the command path; the M
    /// cell shows the effective state plus a small lock and ignores clicks.
    bool muteLocked = false;
};

struct SoloUiHooks
{
    std::function<TrackSoloDisplayState(TrackId)> displayState;
    /// Toggle explicit solo in the current set (temporary or active memory).
    /// May refuse atomically (proxy isolation impossible) — the coordinator
    /// reports that itself; the UI just repaints from the provider afterwards.
    std::function<void(TrackId)> toggleSolo;
};
