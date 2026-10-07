#pragma once

// =============================================================================
// VisualTrackGroup — purely visual arrangement grouping metadata (no audio semantics)
// =============================================================================
//
// A visual track group names a set of ADJACENT arrangement tracks so the UI can draw a shared
// marker and collapse the members into a compact 4 px/member content overview. It is layout
// metadata ONLY and must never be confused with the routing `TrackKind::Group` bus row:
//   • Creating / renaming / collapsing / ungrouping NEVER creates or removes tracks, never moves
//     clips, never changes routing, sends, plugins, instruments, Mute/Solo/Off/Monitor/Arm, and
//     never affects playback, recording, export, or mixer visibility.
//   • Membership references stable `TrackId`s (never indices). Stored member lists may contain
//     ids of deleted tracks — exactly like the Solo sets — so undoing a track deletion restores
//     membership without extra bookkeeping. Display/persistence paths filter to EXISTING tracks
//     ("effective members") and treat a group as displayable only when its ≥2 effective members
//     are CONTIGUOUS in current snapshot order (safe fallback: members render as normal tracks).
//   • Groups never nest, never overlap (per effective membership), and never contain the Master
//     (Stereo Out) row.
//
// Owned by `Session` on the message thread, OUTSIDE `SessionSnapshot` (like the Solo sets):
// whole-session undo snapshots never carry group state; create/rename/ungroup use the narrow
// `VisualTrackGroupsUndoSides` metadata undo instead, and collapse/expand is a display change
// (project dirty, no undo entry). Persisted from project v27 (`visualTrackGroups`).
// =============================================================================

#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <vector>

struct VisualTrackGroup
{
    /// Stable per-session group id (monotonic, message thread; not persisted — reassigned on load).
    int id = 0;
    juce::String name;
    /// Stored membership (session-order filtered copies are built by Session helpers). May contain
    /// stale ids of deleted tracks on purpose; see header comment.
    std::vector<TrackId> memberTrackIds;
    /// Display state: collapsed = every effective member shows as a 4 px mini strip.
    bool collapsed = false;

    [[nodiscard]] bool operator==(const VisualTrackGroup& other) const noexcept
    {
        return id == other.id && name == other.name && memberTrackIds == other.memberTrackIds
               && collapsed == other.collapsed;
    }
    [[nodiscard]] bool operator!=(const VisualTrackGroup& other) const noexcept
    {
        return !(*this == other);
    }
};
