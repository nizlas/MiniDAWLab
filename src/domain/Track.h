#pragma once

// =============================================================================
// Track  —  one timeline lane in the session (Phase 3 minimal multi-track)
// =============================================================================
//
// ROLE
//   A **track** is a stable identity (`TrackId`) and an **ordered** list of `PlacedClip` rows on
//   the **same** session timeline (device samples) as all other tracks. **Overlap** (which clip
//   wins at an instant) is **only** defined among clips *on this track* — the same "index 0 =
//   front / newest" rule as late Phase 2, but the list does not span the whole project.
//
// CROSS-TRACK AUDIO
//   The engine may **add** the audible output of several tracks. That is **not** a mixer UI and
//   not a routing graph; it is a minimal sum for multi-track hearing (see `PlaybackEngine` and
//   `PHASE_PLAN` Phase 3).
//
// LIFECYCLE
//   A `Track` is held **by value** inside an immutable `SessionSnapshot` — edits happen only by
//   building a **new** snapshot on the message thread, same pattern as pre-track session state.
// =============================================================================

#include "domain/PlacedClip.h"

#include <cstdint>
#include <vector>

using TrackId = std::uint64_t;

inline constexpr TrackId kInvalidTrackId = 0;

// ---------------------------------------------------------------------------
// Track — one lane’s clips (session timeline samples; front-most at index 0 within this track)
// ---------------------------------------------------------------------------
class Track
{
public:
    // [Message thread, snapshot build] `id` must be non-zero. `placedClips` is front = newest for
    // this lane only; ownership of shared `AudioClip` follows `PlacedClip` as before.
    explicit Track(TrackId id, std::vector<PlacedClip> placedClips) noexcept;

    [[nodiscard]] TrackId getId() const noexcept { return id_; }
    [[nodiscard]] int getNumPlacedClips() const noexcept;
    [[nodiscard]] const PlacedClip& getPlacedClip(int index) const;
    [[nodiscard]] const std::vector<PlacedClip>& getPlacedClips() const noexcept
    {
        return placedClips_;
    }

private:
    TrackId id_ = kInvalidTrackId;
    std::vector<PlacedClip> placedClips_;
};
