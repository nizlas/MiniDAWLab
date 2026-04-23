// =============================================================================
// SessionSnapshot.cpp  —  immutable session state published to the audio thread
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   `SessionSnapshot` is the read-only object that `Session` publishes across the
//   **message-thread → audio-thread** boundary. It generalizes the old single-`AudioClip` pointer
//   to a small, placement-aware, **const** snapshot: the callback never sees a live “session
//   editor” object — only a completed picture of what to play, built on the non-realtime side.
//
// WHY THIS FILE EXISTS
//   Phase 1 handed one `const AudioClip` to the engine. Phase 2 needs a **timeline** structure
//   (ordered `PlacedClip` rows) *before* all multi-clip product rules are wired. The factories
//   below build those immutable snapshots on the message thread; `std::atomic_store` to a
//   `shared_ptr<const SessionSnapshot>` is the same handoff pattern as the old `atomic<shared_ptr
//   <const AudioClip>>`, with a larger payload class.
//
// BEHAVIOR
//   No mutexes inside. Construction runs off the audio callback; the hot path is load shared_ptr
//   (acquire) + read const fields. Refcounts drop when the last reader releases.
// =============================================================================

#include "domain/SessionSnapshot.h"

#include "domain/AudioClip.h"

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

SessionSnapshot::SessionSnapshot(std::vector<PlacedClip> placedClips) noexcept
    : placedClips_(std::move(placedClips))
{
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::createEmpty() noexcept
{
    // Reuse one immutable empty snapshot so “clear the session” (and the defensive `material ==
    // nullptr` path in `withSinglePlacedClip`) can publish a stable, shared “no clips” value
    // without a fresh `vector` allocation on every clear — same semantics, lower churn.
    static const auto empty = std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::vector<PlacedClip>{}));
    return empty;
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withSinglePlacedClip(
    std::shared_ptr<const AudioClip> material, const std::int64_t startSampleOnTimeline) noexcept
{
    if (material == nullptr)
    {
        // Should not happen on the normal load path; if it does, do not build a `PlacedClip` with
        // a null jassert in the tree — return the same shared empty as `clearClip`.
        return createEmpty();
    }

    // Transitional Phase 2 factory: still a snapshot with exactly one `PlacedClip` so playback
    // and UI can stay Phase-1-like in Step 4, but the session already speaks “placement on a
    // timeline” (`startSampleOnTimeline` — 0 for the current file-load path) inside the new type.
    std::vector<PlacedClip> v;
    v.emplace_back(std::move(material), startSampleOnTimeline);
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v)});
}

const PlacedClip& SessionSnapshot::getPlacedClip(const int index) const
{
    jassert(index >= 0 && index < getNumPlacedClips());
    // `at` for bounds; failure here is a programming error (engine asking for a row that does
    // not exist for this snapshot’s size), not a user-facing condition.
    return placedClips_.at((size_t)index);
}
