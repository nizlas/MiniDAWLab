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

namespace
{
    // Half-open [a0, a1) vs [b0, b1) in session samples — true iff they share at least one sample
    // instant (aligns with `PlaybackEngine` coverage and `PlacedClip` span semantics).
    bool rangesOverlapHalfOpen(
        const std::int64_t a0, const std::int64_t a1, const std::int64_t b0, const std::int64_t b1)
    {
        return a0 < b1 && b0 < a1;
    }
} // namespace

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
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const PlacedClipId newClipId) noexcept
{
    if (material == nullptr)
    {
        // Should not happen on the normal load path; if it does, do not build a `PlacedClip` with
        // a null jassert in the tree — return the same shared empty as `clearClip`.
        return createEmpty();
    }
    if (newClipId == kInvalidPlacedClipId)
    {
        jassert (false);
        return createEmpty();
    }

    // Transitional Phase 2 factory: still a snapshot with exactly one `PlacedClip` so playback
    // and UI can stay Phase-1-like in Step 4, but the session already speaks “placement on a
    // timeline” (`startSampleOnTimeline` — 0 for the current file-load path) inside the new type.
    std::vector<PlacedClip> v;
    v.emplace_back(newClipId, std::move(material), startSampleOnTimeline);
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v)});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipAddedAsNewest(
    const SessionSnapshot& previous,
    const PlacedClipId newClipId,
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline) noexcept
{
    if (material == nullptr)
    {
        return createEmpty();
    }
    if (newClipId == kInvalidPlacedClipId)
    {
        jassert (false);
        return createEmpty();
    }

    // Product: the clip just added is *front* in the overlap list (index 0); older entries keep
    // their relative order but shift down. Copies of `PlacedClip` are cheap (shared_ptr + int).
    std::vector<PlacedClip> v;
    const int n = previous.getNumPlacedClips();
    v.reserve((size_t)n + 1U);
    v.emplace_back(newClipId, std::move(material), startSampleOnTimeline);
    for (int i = 0; i < n; ++i)
    {
        v.push_back(previous.getPlacedClip(i));
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v)});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipMoved(
    const SessionSnapshot& previous,
    const PlacedClipId movedId,
    const std::int64_t newStartSampleOnTimeline) noexcept
{
    if (movedId == kInvalidPlacedClipId)
    {
        jassert (false);
        return std::shared_ptr<const SessionSnapshot>(
            new SessionSnapshot(std::vector<PlacedClip>(previous.getPlacedClips())));
    }

    std::vector<PlacedClip> clips(previous.getPlacedClips());
    int movedIndex = -1;
    for (int i = 0; i < (int)clips.size(); ++i)
    {
        if (clips[(size_t)i].getId() == movedId)
        {
            movedIndex = i;
            break;
        }
    }
    if (movedIndex < 0)
    {
        jassert (false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot(std::move(clips)));
    }

    const std::int64_t clampedStart
        = juce::jmax(std::int64_t{0}, newStartSampleOnTimeline);
    {
        const PlacedClip& oldM = clips[(size_t)movedIndex];
        clips[(size_t)movedIndex] = oldM.withStartSampleOnTimeline(clampedStart);
    }

    const PlacedClip& m = clips[(size_t)movedIndex];
    const std::int64_t m0 = m.getStartSample();
    const std::int64_t m1 = m0
                            + static_cast<std::int64_t>(m.getAudioClip().getNumSamples());
    bool overlapWithAnyOther = false;
    for (int j = 0; j < (int)clips.size(); ++j)
    {
        if (j == movedIndex)
        {
            continue;
        }
        const PlacedClip& o = clips[(size_t)j];
        const std::int64_t o0 = o.getStartSample();
        const std::int64_t o1
            = o0 + static_cast<std::int64_t>(o.getAudioClip().getNumSamples());
        if (rangesOverlapHalfOpen(m0, m1, o0, o1))
        {
            overlapWithAnyOther = true;
            break;
        }
    }

    if (!overlapWithAnyOther)
    {
        // Promote M to index 0: remove from `movedIndex`, insert at front — only M moves.
        PlacedClip promoted = std::move(clips[(size_t)movedIndex]);
        clips.erase(clips.begin() + movedIndex);
        clips.insert(clips.begin(), std::move(promoted));
    }

    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(clips)});
}

std::int64_t SessionSnapshot::getDerivedTimelineLengthSamples() const noexcept
{
    // Product: one **exclusive** end index for “session time” (device samples) shared by
    // `Transport` seek range, the waveform view’s x-axis, and `PlaybackEngine`’s “past end”
    // check — the maximum of (placement start + material length) over all rows; empty → 0.
    std::int64_t maxEnd = 0;
    for (const PlacedClip& p : placedClips_)
    {
        const std::int64_t end = p.getStartSample()
                                 + static_cast<std::int64_t>(p.getAudioClip().getNumSamples());
        if (end > maxEnd)
        {
            maxEnd = end;
        }
    }
    return maxEnd;
}

const PlacedClip& SessionSnapshot::getPlacedClip(const int index) const
{
    jassert(index >= 0 && index < getNumPlacedClips());
    // `at` for bounds; failure here is a programming error (engine asking for a row that does
    // not exist for this snapshot’s size), not a user-facing condition.
    return placedClips_.at((size_t)index);
}
