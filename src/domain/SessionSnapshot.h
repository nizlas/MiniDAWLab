#pragma once

// =============================================================================
// SessionSnapshot.h  —  immutable view of the session the audio thread is allowed to read
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   `SessionSnapshot` is the **read-only** aggregate published across the message / audio
//   boundary. Phase 3: it holds an **ordered** list of **tracks** — each `Track` is its own
//   `PlacedClip` list on the same session timeline. Overlap is resolved **per track**; the
//   engine may **add** the audible result across tracks (see `PlaybackEngine`, `PHASE_PLAN`
//   Phase 3). The callback still only acquire-loads a `const` snapshot, never mutates.
//
// WHY IMMUTABLE
//   Unchanged: realtime code reads a single completed picture, no structural edits in the
//   callback, lock-free `shared_ptr` handoff.
//
// FACTORIES (see .cpp)
//   `withSingleEmptyTrack` — one lane, no clips (default session *shape* after clear / startup).
//   `withTrackAdded` — append a new empty track.
//   `withClipAddedAsNewestOnTargetTrack` — prepend a clip on a given `TrackId` (newest in that
//     lane, index 0 of that track).
//   `withClipMoved` — move one `PlacedClip` in **its** track; committed end-state rule only among
//     clips on that track.
//   `withSinglePlacedClip` — transitional: one track, one clip.
//
// See also: `Track`, `PlacedClip`, `Session`, `PlaybackEngine`, `docs/ARCHITECTURE_PRINCIPLES.md`.
// =============================================================================

#include "domain/PlacedClip.h"
#include "domain/Track.h"

#include <memory>
#include <vector>

class AudioClip;

// ---------------------------------------------------------------------------
// SessionSnapshot — ordered `Track` list, const after construction
// ---------------------------------------------------------------------------
class SessionSnapshot
{
public:
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> createEmpty() noexcept;

    // [Message thread] One empty lane (e.g. default session and after clear) — *not* the same
    // object as `createEmpty()` (zero tracks); see `Session` usage.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withSingleEmptyTrack(
        TrackId trackId) noexcept;

    // [Message thread] `newClipId` must be non-zero. Null material or invalid id defends to
    // `createEmpty()` in debug where noted in .cpp.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withSinglePlacedClip(
        std::shared_ptr<const AudioClip> material,
        std::int64_t startSampleOnTimeline,
        PlacedClipId newClipId) noexcept;

    // [Message thread] `targetTrackId` is the lane receiving the new front-most clip. If
    // `previous` has no tracks, a single track with that id is created holding only the new clip.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipAddedAsNewestOnTargetTrack(
        const SessionSnapshot& previous,
        PlacedClipId newClipId,
        std::shared_ptr<const AudioClip> material,
        std::int64_t startSampleOnTimeline,
        TrackId targetTrackId) noexcept;

    // [Message thread] Append an empty track at the end. `newTrackId` must be non-zero and not
    // already present in `previous`.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackAdded(
        const SessionSnapshot& previous,
        TrackId newTrackId) noexcept;

    // [Message thread] Only affects the track that contains `movedId` — same committed-move policy
    // as pre-track `withClipMoved`, but overlap is computed only **within that track**.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipMoved(
        const SessionSnapshot& previous,
        PlacedClipId movedId,
        std::int64_t newStartSampleOnTimeline) noexcept;

    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] int getNumTracks() const noexcept
    {
        return static_cast<int>(tracks_.size());
    }
    // Bounds-checked; no cross-track "global index" — use a track and then clip index in that track.
    [[nodiscard]] const Track& getTrack(int index) const;
    // -1 if not found.
    [[nodiscard]] int findTrackIndexById(TrackId id) const noexcept;

    [[nodiscard]] std::int64_t getDerivedTimelineLengthSamples() const noexcept;

private:
    explicit SessionSnapshot(std::vector<Track> tracks) noexcept;

    std::vector<Track> tracks_;
};
