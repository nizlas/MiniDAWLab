#pragma once

// =============================================================================
// SessionSnapshot.h  —  immutable view of the session the audio thread is allowed to read
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   `SessionSnapshot` is the **read-only** aggregate that replaces the old “single
//   `std::shared_ptr<const AudioClip>` in `Session`” handoff. It answers: *which* clips (as
//   `PlacedClip` rows), in *what front-to-back order* for overlap, for **this** moment in the
//   program. The audio callback does not mutate the session: it `acquire`‑loads
//   `std::shared_ptr<const SessionSnapshot>` and only reads. The message thread **release**‑stores
//   a new pointer after a successful decode or a clear, same cross-thread contract as before,
//   with a larger but still immutable shape so Phase 2 can add more placements without giving the
//   callback mutable shared state.
//
// WHY IMMUTABLE
//   Realtime code cannot take locks or do structural edits to “the session” mid-block. Freezing
//   the answer to “what is on the timeline?” into one const object lets the engine read
//   placements and material pointers without coordinating with the UI, while the UI on the
//   message thread builds the *next* snapshot at leisure.
//
// WHY A SHARED *EMPTY* SNAPSHOT
//   `createEmpty()` returns one process-wide (lazy-initialized) empty snapshot so “clear” and
//   error paths can point at a **stable, allocation-free** “no clips” value each time, instead
//   of allocating a new empty `vector` for every `clearClip` or failed edge case. Pointer
//   identity is not a product contract; the contract is *semantic* emptiness and safe sharing.
//
// FACTORIES (see .cpp)
//   • `withSinglePlacedClip` is the **transitional** Step 4 builder: one `PlacedClip` at a chosen
//     start time so the app still *behaves* like Phase 1, but the data already lives in the
//     timeline snapshot shape. Later steps add more entries and engine coverage without
//     redesigning the handoff type.
//
// See also: `Session`, `PlacedClip`, `PlaybackEngine`, `docs/ARCHITECTURE_PRINCIPLES.md` (Phase 2
//   snapshot), `status/DECISION_LOG.md` (2026-04-23 Phase 2 steering).
// =============================================================================

#include "domain/PlacedClip.h"

#include <memory>
#include <vector>

class AudioClip;

// ---------------------------------------------------------------------------
// SessionSnapshot — ordered `PlacedClip` list, const after construction
// ---------------------------------------------------------------------------
// Ownership: each snapshot is owned by `std::shared_ptr<const SessionSnapshot>`; `Session` holds
// the atomic to the *current* snapshot. `PlacedClip` entries hold `shared_ptr` to const `AudioClip`
// material, so the snapshot and the per-clip buffers share one ref-counted graph.
// Threading: publish only from message / editor paths; read from any thread with an acquired
// `shared_ptr` (audio callback: once per block is enough).
// ---------------------------------------------------------------------------
class SessionSnapshot
{
public:
    // [Message thread and static init] Returns a single shared `SessionSnapshot` with no
    // placements, reused for every “empty session” store — see file header (why not allocate
    // empty each time). Safe to read from any thread; immutable.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> createEmpty() noexcept;

    // [Message thread] Build a *new* snapshot with exactly one row (Step 4 load path: same
    // audible result as old single-clip publish, new container shape). Null material yields
    // empty via the same shared empty object — defensive; normal callers pass decoded clips.
    [[nodiscard]] static std::shared_ptr<const SessionSnapshot> withSinglePlacedClip(
        std::shared_ptr<const AudioClip> material, std::int64_t startSampleOnTimeline) noexcept;

    [[nodiscard]] bool isEmpty() const noexcept { return placedClips_.empty(); }

    [[nodiscard]] int getNumPlacedClips() const noexcept
    {
        return static_cast<int>(placedClips_.size());
    }

    // Bounds-checked; index 0 = front-most clip for Phase 2 overlap order (see `PHASE_PLAN`).
    [[nodiscard]] const PlacedClip& getPlacedClip(int index) const;
    [[nodiscard]] const std::vector<PlacedClip>& getPlacedClips() const noexcept
    {
        return placedClips_;
    }

private:
    explicit SessionSnapshot(std::vector<PlacedClip> placedClips) noexcept;

    std::vector<PlacedClip> placedClips_;
};
