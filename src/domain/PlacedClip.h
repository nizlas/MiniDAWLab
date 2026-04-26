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
// Left-edge trim: `leftTrimSamples_` = L — non-destructive offset into the material; audible
//   material is the half-open range [L, L + V) in *file* indices; the timeline event is
//   [S, S + V) in session samples, with invariants S + L + V and L + V along material unchanged
//   across a left trim gesture.
//
// THREADING
//   Immutable value after construction. The audio thread never mutates a `PlacedClip`; it receives
//   a `const SessionSnapshot` that contains copies/references to placements built on the message
//   thread. Lifetime: keep the owning `std::shared_ptr<const SessionSnapshot>` (or a `PlacedClip`
//   that lives within it) alive for the read.
//
// See also: `SessionSnapshot`, `Session`, `AudioClip`, `docs/PHASE_PLAN.md` (Phase 2 semantics).
// =============================================================================

#include <cstdint>
#include <memory>

class AudioClip;

// Stable per-placement identity for UI (selection, move) and for `SessionSnapshot::withClipMoved`.
// 0 = invalid / unused; `Session` assigns monotonic ids on the message thread.
using PlacedClipId = std::uint64_t;
inline constexpr PlacedClipId kInvalidPlacedClipId = 0;

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
    // (jassert in .cpp). `id` is assigned once by `Session` for each new row and kept across moves
    // (see `withClipMoved`); it must be non-zero. `startSampleOnTimeline` is in device samples.
    // `visibleLengthSamplesOnDiskOrDefault` = -1 means "use full decoded material from L onward"
    // (length M - L). A positive value is the audible V, clamped to [1, M - L].
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline) noexcept;
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline,
              std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept;
    // Full placement: L = left trim in material [0, M-1], last arg = right-trim / visible V or -1
    // for "use M - L".
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline,
              std::int64_t leftTrimSamples,
              std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept;

    [[nodiscard]] PlacedClipId getId() const noexcept { return id_; }

    [[nodiscard]] const AudioClip& getAudioClip() const noexcept;

    // **Placement span** on the session timeline and for playback / overlap (non-destructive: PCM is
    // unchanged; this may be shorter than remaining material (M - L) after right-edge trim).
    [[nodiscard]] std::int64_t getEffectiveLengthSamples() const noexcept;
    // Full decoded buffer length in `getAudioClip()`.
    [[nodiscard]] int getMaterialLengthSamples() const noexcept;
    // Non-destructive left edge: samples skipped at the start of the file (audible file range is
    // [L, L + getEffectiveLengthSamples())).
    [[nodiscard]] std::int64_t getLeftTrimSamples() const noexcept { return leftTrimSamples_; }

    // Sample index on the *session* timeline where this clip’s first **heard** sample begins.
    [[nodiscard]] std::int64_t getStartSample() const noexcept { return startSampleOnTimeline_; }

    // Shared ownership of the same const material the snapshot holds; use when you need lifetime
    // or sharing semantics without copying PCM.
    [[nodiscard]] std::shared_ptr<const AudioClip> getMaterial() const noexcept { return material_; }

    // Same id, material, L, and visible length, new start — used by `SessionSnapshot::withClipMoved`.
    [[nodiscard]] PlacedClip withStartSampleOnTimeline(std::int64_t newStartSampleOnTimeline) const noexcept;
    // Right-edge trim only: same start, L, and material; new V clamped to [1, M - L].
    [[nodiscard]] PlacedClip withRightEdgeVisibleLength(std::int64_t newVisibleLength) const noexcept;
    // Left-edge trim: L' clamped so V' >= 1 and S' >= 0; S and V follow invariants.
    [[nodiscard]] PlacedClip withLeftEdgeTrim(std::int64_t newLeftTrimSamples) const noexcept;

private:
    PlacedClipId id_ = kInvalidPlacedClipId;
    std::shared_ptr<const AudioClip> material_;
    std::int64_t startSampleOnTimeline_ = 0;
    // Half-open [L, L + visibleLengthSamples_) in **material** indices is audible; V =
    // visibleLengthSamples_.
    std::int64_t leftTrimSamples_ = 0;
    std::int64_t visibleLengthSamples_ = 0;
};
