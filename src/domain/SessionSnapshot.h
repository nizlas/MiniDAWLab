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
//   `withSingleEmptyTrack` — one lane, no clips (default session *shape* after clear / startup;
//     `trackName` is the new `Track`’s `getName()`).
//   `withTrackAdded` — append a new empty track (caller supplies `newTrackName` for the new lane).
//   `withClipAddedAsNewestOnTargetTrack` — prepend a clip on a given `TrackId` (newest in that
//     lane, index 0 of that track).
//   `withClipMoved` — move one `PlacedClip` in **its** track; committed end-state rule only among
//     clips on that track.
//   `withClipMovedToTrack` — move one `PlacedClip` to **another** track: remove from source lane,
//     insert as **index 0** (front-most) on the target; identity preserved. Not for same source
//     and target track — use `withClipMoved` in that case.
//   `withTrackReordered` — move one `Track` row in the session’s **track list** order; each track’s
//     `PlacedClip` list and `name_` are unchanged; audio summing is order-independent. No-op
//     if the track is not found or `destIndex` equals the current index.
//   `withTracks` — **load-only:** replace the session with a full pre-built track list (one publish).
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
        TrackId trackId, juce::String trackName) noexcept;

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

    // Same as above but with explicit L / V for recorded takes (L often 0; V = visible window).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipAddedAsNewestOnTargetTrack(
        const SessionSnapshot& previous,
        PlacedClipId newClipId,
        std::shared_ptr<const AudioClip> material,
        std::int64_t startSampleOnTimeline,
        TrackId targetTrackId,
        std::int64_t leftTrimSamples,
        std::int64_t visibleLengthSamples) noexcept;

    // Same + material window bounds (cycle takes sharing one WAV).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipAddedAsNewestOnTargetTrack(
        const SessionSnapshot& previous,
        PlacedClipId newClipId,
        std::shared_ptr<const AudioClip> material,
        std::int64_t startSampleOnTimeline,
        TrackId targetTrackId,
        std::int64_t leftTrimSamples,
        std::int64_t visibleLengthSamples,
        std::int64_t materialWindowStartSamples,
        std::int64_t materialWindowEndExclusiveSamples) noexcept;

    // [Message thread] Append an empty track at the end. `newTrackId` must be non-zero and not
    // already present in `previous`.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackAdded(
        const SessionSnapshot& previous,
        TrackId newTrackId,
        juce::String newTrackName) noexcept;

    // [Message thread] Drops the `TrackId` row and all its `PlacedClip`s. Unknown id: same snapshot
    // pointer shape (tracks copied verbatim). Removing the last lane yields `createEmpty()`.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackRemoved(
        const SessionSnapshot& previous,
        TrackId removedTrackId) noexcept;

    // [Message thread] Drops one `PlacedClip` on `trackId` only if `placedClipId` exists there.
    // Other tracks untouched; lane keeps name/off/muted/fader. Unknown lane or placement: no-op copy.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withPlacedClipRemoved(
        const SessionSnapshot& previous,
        TrackId trackId,
        PlacedClipId placedClipId) noexcept;

    // [Message thread] Only affects the track that contains `movedId` — same committed-move policy
    // as pre-track `withClipMoved`, but overlap is computed only **within that track**.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipMoved(
        const SessionSnapshot& previous,
        PlacedClipId movedId,
        std::int64_t newStartSampleOnTimeline) noexcept;

    // [Message thread] Remove `movedId` from its current track, insert that row at index 0 of
    // `targetTrackId` with timeline start `newStartSampleOnTimeline` (clamped in .cpp). Source and
    // target must be **different** tracks; if equal, the factory is a no-op (debug jassert).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipMovedToTrack(
        const SessionSnapshot& previous,
        PlacedClipId movedId,
        std::int64_t newStartSampleOnTimeline,
        TrackId targetTrackId) noexcept;

    // [Message thread] Reorder tracks only: the track named by `movedTrackId` ends at `destIndex`
    // in the resulting vector, where `0 <= destIndex < getNumTracks()`. Defensive: same snapshot if
    // the id is not found, `destIndex` is out of range, or it equals the current index (no-op).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackReordered(
        const SessionSnapshot& previous,
        TrackId movedTrackId,
        int destIndex) noexcept;

    // [Message thread] Right-edge **trim** (non-destructive window on material). Replaces the
    // `PlacedClip` with matching id; **does not** reorder the lane. Unknown id: debug jassert, same
    // snapshot. Split/cut use different factories.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipRightEdgeTrimmed(
        const SessionSnapshot& previous,
        PlacedClipId id,
        std::int64_t newVisibleLengthSamples) noexcept;

    // [Message thread] Left-edge trim (non-destructive). Unknown id: debug jassert, same snapshot.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipLeftEdgeTrimmed(
        const SessionSnapshot& previous,
        PlacedClipId id,
        std::int64_t newLeftTrimSamples) noexcept;

    // [Message thread] Replace one `PlacedClip` with two adjacent clips (same material + window).
    // `splitSampleOnTimeline` must be strictly inside (start, start + visible); unknown `targetId`:
    // debug jassert, verbatim copy. `leftNewId` / `rightNewId` must be non-zero and distinct.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withClipSplit(
        const SessionSnapshot& previous,
        PlacedClipId targetId,
        std::int64_t splitSampleOnTimeline,
        PlacedClipId leftNewId,
        PlacedClipId rightNewId) noexcept;

    // [Message thread] Replace one track's `channelFaderGain`; other tracks unchanged. Unknown id: no-op
    // snapshotcopy of `previous` (debug jassert).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackChannelFaderGain(
        const SessionSnapshot& previous,
        TrackId trackId,
        float channelFaderGainLinear) noexcept;

    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackOff(
        const SessionSnapshot& previous,
        TrackId trackId,
        bool trackOff) noexcept;

    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTrackMuted(
        const SessionSnapshot& previous,
        TrackId trackId,
        bool trackMuted) noexcept;

    // [Message thread] **Load-only:** publish a full pre-built track list in one step (e.g. project
    // open). Does not add clips incrementally. `tracks` must be non-empty. Stored arrangement extent
    // is 0 (use `withTracks` overload for v3 load).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTracks(
        std::vector<Track> tracks) noexcept;

    // [Message thread] **Load-only:** like `withTracks` but sets persisted `arrangementExtentSamples`
    // (0 = as-if absent; `getArrangementExtentSamples` still floored by derived content end).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTracks(
        std::vector<Track> tracks,
        std::int64_t arrangementExtentSamples) noexcept;

    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withTracks(
        std::vector<Track> tracks,
        std::int64_t arrangementExtentSamples,
        std::int64_t leftLocatorSamples,
        std::int64_t rightLocatorSamples) noexcept;

    // [Message thread] Same tracks, new **stored** arrangement extent (e.g. `Session::setArrangementExtentSamples`).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withArrangementExtent(
        const SessionSnapshot& previous,
        std::int64_t newArrangementExtentSamples) noexcept;

    /// [Message thread] Timeline left/right locators (`right == 0` = right locator unset sentinel).
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withLocators(
        const SessionSnapshot& previous,
        std::int64_t leftLocatorSamples,
        std::int64_t rightLocatorSamples) noexcept;

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

    // Where **clips** end on the session timeline; independent of the navigable / playable extent.
    // Same as the historical “logical timeline length” used before arrangement extent.
    // Stored arrangement may be 0: effective = max(stored, content end).
    [[nodiscard]] std::int64_t getArrangementExtentSamples() const noexcept;

    // Raw persisted value (message-thread use for `Session::setArrangementExtentSamples` monotonic
    // rule; audio should use `getArrangementExtentSamples()`).
    [[nodiscard]] std::int64_t getStoredArrangementExtentSamples() const noexcept
    {
        return arrangementExtentSamples_;
    }

    [[nodiscard]] std::int64_t getLeftLocatorSamples() const noexcept { return leftLocatorSamples_; }
    [[nodiscard]] std::int64_t getRightLocatorSamples() const noexcept { return rightLocatorSamples_; }

private:
    explicit SessionSnapshot(std::vector<Track> tracks,
                             std::int64_t arrangementExtentSamples,
                             std::int64_t leftLocatorSamples,
                             std::int64_t rightLocatorSamples) noexcept;

    std::vector<Track> tracks_;
    std::int64_t arrangementExtentSamples_ = 0;
    std::int64_t leftLocatorSamples_ = 0;
    std::int64_t rightLocatorSamples_ = 0;
};
