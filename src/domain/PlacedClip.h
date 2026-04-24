#pragma once

// =============================================================================
// PlacedClip.h  —  one clip’s *material* plus *where* it sits on the session timeline
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   A `PlacedClip` is one row in the session’s edit model: the decoded `AudioClip` (PCM) *and* the
//   device-sample index on the *session timeline* where that buffer starts. `AudioClip` by design
//   only holds the contiguous samples and metadata; it does not know *when* the material is
//   played relative to other clips. Phase 2 adds that split so one snapshot can hold *many*
//   `PlacedClip` entries later without conflating “what the bytes are” with “at what time they
//   begin.”
//
// WHY “PLACEMENT” IS SEPARATE FROM `AudioClip`
//   The same `AudioClip` could theoretically be instanced on the timeline in more than one place
//   in a later phase; even before that, keeping start time in `PlacedClip` makes `SessionSnapshot`
//   a single immutable struct that the audio thread can read top-down: ordered placements, no
//   hidden offsets inside the material type.
//
// THREADING
//   Immutable value after construction. The audio thread never mutates a `PlacedClip`; it receives
//   a `const SessionSnapshot` that contains copies/references to placements built on the message
//   thread. Lifetime: keep the owning `std::shared_ptr<const SessionSnapshot>` (or a `PlacedClip`
//   that lives within it) alive for the read.
//
// Step 4–5: timeline-absolute playhead. Step 6: add-at-playhead sets `startSampleOnTimeline` from
//   a **non-audio** read of `Transport` at add time; new rows are *front* (index 0, newest) in
//   `SessionSnapshot` (see `SessionSnapshot::withClipAddedAsNewest`).
//
// See also: `SessionSnapshot`, `Session`, `AudioClip`, `docs/PHASE_PLAN.md` (Phase 2 semantics).
// =============================================================================

#include <cstdint>
#include <memory>

class AudioClip;

// ---------------------------------------------------------------------------
// PlacedClip — owns shared material + timeline start (no transport, no UI)
// ---------------------------------------------------------------------------
// Responsibility: model “this buffer, at this point on the session clock.”
// Does not: advance playhead, open files, or decide mix rules (PlaybackEngine + Transport).
// ---------------------------------------------------------------------------
class PlacedClip
{
public:
    // [Message thread, during load / when building a snapshot] `material` must be non-null
    // (jassert in .cpp). `startSampleOnTimeline` is in device samples; Step 6 sets it from the
    // playhead at add time.
    PlacedClip(std::shared_ptr<const AudioClip> material, std::int64_t startSampleOnTimeline) noexcept;

    [[nodiscard]] const AudioClip& getAudioClip() const noexcept;

    // Sample index on the *session* timeline where this clip’s first sample is heard (not an
    // offset *inside* the `AudioClip` buffer — that is still 0..length-1 for the PCM itself).
    [[nodiscard]] std::int64_t getStartSample() const noexcept { return startSampleOnTimeline_; }

    // Shared ownership of the same const material the snapshot holds; use when you need lifetime
    // or sharing semantics without copying PCM.
    [[nodiscard]] std::shared_ptr<const AudioClip> getMaterial() const noexcept { return material_; }

private:
    std::shared_ptr<const AudioClip> material_;
    std::int64_t startSampleOnTimeline_ = 0;
};
