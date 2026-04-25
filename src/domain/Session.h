#pragma once

// =============================================================================
// Session — message-thread owner of the *current* immutable session snapshot; no transport, no UI
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   `Session` is the **sole publisher** of the timeline answer for playback: an atomic pointer to
//   a `const SessionSnapshot` that replaces the earlier “one `const AudioClip`” field. The
//   **audio thread** does not “open the session for editing” — it only **acquire**‑loads the
//   shared_ptr and reads placements + PCM. The **message thread** (file chooser, future editors)
//   **release**‑stores a **new** snapshot after decode succeeds, or points at the **shared empty**
//   snapshot on clear, using the same lock-free `shared_ptr` idiom as Phase 1 with a larger
//   immutable value type. That is how the steering “immutable atomic snapshot” handoff is
//   realized in code: no mutex on the hot path, no half-published session graph.
//
// RELATION TO `PlacedClip` / `SessionSnapshot` / `AudioClip`
//   `AudioFileLoader` still produces `AudioClip` only. `Session` assembles *snapshots* that wrap
//   that material in `PlacedClip` rows (start time on the session timeline) inside a
//   `SessionSnapshot`. Separation keeps decode concerns out of the snapshot type and leaves
//   placement policy in one place (`Session` / future session commands) rather than inside PCM.
//
// THREAD MODEL
//   • addClipFromFileAtPlayhead / clearClip: [Message thread]; decode may block on load.
//   • loadSessionSnapshotForAudioThread: [Audio thread] or [Message thread] — acquire-load;
//     refcount only on the hot path; no decode.
//   • getCurrentClip: [Message thread] **bridge** API — front clip’s `AudioClip` only; the
//     timeline **view** uses `loadSessionSnapshotForAudioThread` + `getPlacedClips` (Step 7).
//
// OWNERSHIP
//   `Session` owns `std::atomic<std::shared_ptr<const SessionSnapshot>>` only. It does not own
//   `Transport` or the audio device. Clip PCM lifetime is through `shared_ptr` inside the snapshot.
//
// In-body: `Session.cpp` explains failure vs success at the atomic store and what “empty” means.
// See also: `SessionSnapshot`, `PlacedClip`, `AudioFileLoader`, `PlaybackEngine`, `status/DECISION_LOG.md`.
// =============================================================================

#include "domain/PlacedClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

class AudioClip;

// ---------------------------------------------------------------------------
// Session — sole publisher of `std::shared_ptr<const SessionSnapshot>` to readers (engine + UI)
// ---------------------------------------------------------------------------
// Responsibility: after decode (or on clear), **release**-store a new immutable snapshot; readers
// **acquire**-load. Ordering rules are documented in `Session.cpp` at each `atomic_store` / load.
// Does not own `Transport` or the audio device. Clip **ordering** in the snapshot (newest at 0) is
// defined when building snapshots, not in this class’s public API text — see
// `SessionSnapshot::withClipAddedAsNewest`.
// ---------------------------------------------------------------------------
class Session
{
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // [Message thread] Decode file → on success, append to *current* session by prepending a new
    // `PlacedClip` at `startSampleOnTimeline` (Phase 2: **newest** = index 0). On failure, **do not**
    // replace the pointer — the last known-good snapshot remains.
    juce::Result addClipFromFileAtPlayhead(const juce::File& file,
                                         double deviceSampleRate,
                                         std::int64_t startSampleOnTimeline);

    // [Message thread] Append a new **empty** track and make it the active track for
    // `addClipFromFileAtPlayhead` (newest = front within that track when a clip is added).
    void addTrack() noexcept;

    // [Message thread] `activeTrackId_` is the lane that receives the next "Add clip" (see
    // `addClipFromFileAtPlayhead`); it becomes the new id after `addTrack()`.
    [[nodiscard]] TrackId getActiveTrackId() const noexcept;

    // [Message thread] How many `Track` rows exist in the current snapshot (UI lane count).
    [[nodiscard]] int getNumTracks() const noexcept;
    // [Message thread] The id of the i-th `Track` in snapshot order, or `kInvalidTrackId` if out
    // of range.
    [[nodiscard]] TrackId getTrackIdAtIndex(int index) const noexcept;

    // [Message thread] Move one placed clip in **timeline sample** space. Ordering (promote to 0
    // if isolated) is **only** in `SessionSnapshot::withClipMoved` **within the clip’s own track** —
    // the UI does not implement policy. Invalid or unknown id: no publish (see factory jasserts).
    void moveClip(PlacedClipId id, std::int64_t newStartSampleOnTimeline) noexcept;

    // [Message thread] Move a clip to a **different** `TrackId` as front-most in that track (see
    // `SessionSnapshot::withClipMovedToTrack`). **Does not** change `activeTrackId_` (Add clip
    // target). Same-track moves must use `moveClip` only — this path is a no-op if the clip
    // already lives on `targetTrackId` (defensive, see .cpp).
    void moveClipToTrack(
        PlacedClipId id, std::int64_t newStartSampleOnTimeline, TrackId targetTrackId) noexcept;

    // [Message thread] Publish the *shared* empty `SessionSnapshot` (see
    // `SessionSnapshot::createEmpty`) — no clips, nothing to play or paint as waveform material.
    void clearClip() noexcept;

    // [Message thread] Front clip’s `AudioClip` (index 0); **bridge** for legacy call sites.
    // `ClipWaveformView` reads the full snapshot for multi-clip layout (Step 7).
    [[nodiscard]] const AudioClip* getCurrentClip() const noexcept;

    // [Message thread] Derived session timeline extent (max of start+length over placed clips);
    // matches transport playhead + seek + end-of-content clamp. Zero when empty.
    [[nodiscard]] std::int64_t getTimelineLengthSamples() const noexcept;

    // [Audio thread] and [Message thread] Acquire the current `SessionSnapshot` pointer; no
    // decode, no session mutation. This is the main handoff the engine uses each block.
    [[nodiscard]] std::shared_ptr<const SessionSnapshot> loadSessionSnapshotForAudioThread() const noexcept;

private:
    // [Message thread only] Monotonic ids for new `PlacedClip` rows (add path). Not reset on clear
    // so a long edit session does not reuse ids while UI might still hold an old `PlacedClipId`.
    PlacedClipId nextPlacedClipId_ = 1;
    // [Message thread] Monotonic `TrackId` for the **next** `addTrack` (default session already has
    // track 1).
    TrackId nextTrackId_ = 2;
    // [Message thread] `addClipFromFileAtPlayhead` places on this lane (default 1; `addTrack` updates).
    TrackId activeTrackId_ = 1;

    // Current world picture for the audio thread: always either the shared empty snapshot or a
    // user-built snapshot; swapped only from the message thread, read with acquire from any thread.
    mutable std::atomic<std::shared_ptr<const SessionSnapshot>> sessionSnapshot_;
};
