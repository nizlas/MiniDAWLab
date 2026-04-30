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
// **Material window (optional narrowing):** for shared `AudioClip` (e.g. cycle takes), each
//   placement may restrict trims to a sub-range [materialWindowStartSamples_,
//   materialWindowEndExclusiveSamples_) within the full buffer. Legacy clips use [0, M).
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
    // (length min(M - L, window end - L)). A positive value is the audible V, clamped.
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline) noexcept;
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline,
              std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept;
    // Full placement: L = left trim in material, last arg = right-trim / visible V or -1
    // for "use window tail from L".
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline,
              std::int64_t leftTrimSamples,
              std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept;

    // Same as above with explicit **material window** [ws, we) in file indices (cycle takes).
    // Invariant after normalize: ws <= L < we (when V>0), L + V <= we, we <= M.
    PlacedClip(PlacedClipId id,
              std::shared_ptr<const AudioClip> material,
              std::int64_t startSampleOnTimeline,
              std::int64_t leftTrimSamples,
              std::int64_t visibleLengthSamplesOnDiskOrDefault,
              std::int64_t materialWindowStartSamples,
              std::int64_t materialWindowEndExclusiveSamples) noexcept;

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

    // Allowed material sub-range for this placement (half-open [start, end) in file indices).
    [[nodiscard]] std::int64_t getMaterialWindowStartSamples() const noexcept
    {
        return materialWindowStartSamples_;
    }
    [[nodiscard]] std::int64_t getMaterialWindowEndExclusiveSamples() const noexcept
    {
        return materialWindowEndExclusiveSamples_;
    }

    // Sample index on the *session* timeline where this clip’s first **heard** sample begins.
    [[nodiscard]] std::int64_t getStartSample() const noexcept { return startSampleOnTimeline_; }

    // Shared ownership of the same const material the snapshot holds; use when you need lifetime
    // or sharing semantics without copying PCM.
    [[nodiscard]] std::shared_ptr<const AudioClip> getMaterial() const noexcept { return material_; }

    // Same id, material, L, window, and visible length, new start — used by `SessionSnapshot::withClipMoved`.
    [[nodiscard]] PlacedClip withStartSampleOnTimeline(std::int64_t newStartSampleOnTimeline) const noexcept;
    [[nodiscard]] PlacedClip withRightEdgeVisibleLength(std::int64_t newVisibleLength) const noexcept;
    [[nodiscard]] PlacedClip withLeftEdgeTrim(std::int64_t newLeftTrimSamples) const noexcept;

private:
    [[nodiscard]] PlacedClip replicatedWith(std::int64_t startSampleOnTimeline,
                                            std::int64_t leftTrimSamples,
                                            std::int64_t visibleLengthSamples) const noexcept;

    PlacedClipId id_ = kInvalidPlacedClipId;
    std::shared_ptr<const AudioClip> material_;
    std::int64_t startSampleOnTimeline_ = 0;
    std::int64_t leftTrimSamples_ = 0;
    std::int64_t visibleLengthSamples_ = 0;
    std::int64_t materialWindowStartSamples_ = 0;
    std::int64_t materialWindowEndExclusiveSamples_ = 0;
};
