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
class Transport;

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

    // [Message thread] Decode `file` and prepend a clip on `targetTrackId` (not `activeTrackId`).
    // L = 0, V = `intendedVisibleLengthSamples` (clamped in `PlacedClip` to material). Publishes
    // a new snapshot on success.
    juce::Result addRecordedTakeAtSample(const juce::File& file,
                                      double deviceSampleRate,
                                      std::int64_t startSampleOnTimeline,
                                      TrackId targetTrackId,
                                      std::int64_t intendedVisibleLengthSamples);

    // [Message thread] Append a new **empty** track and make it the active track for
    // `addClipFromFileAtPlayhead` (newest = front within that track when a clip is added).
    void addTrack() noexcept;

    // [Message thread] `activeTrackId_` is the lane that receives the next "Add clip" (see
    // `addClipFromFileAtPlayhead`); it becomes the new id after `addTrack()`. **Does not** publish
    // a new snapshot — UI-only / command targeting; keep separate from `SessionSnapshot`.
    [[nodiscard]] TrackId getActiveTrackId() const noexcept;
    // [Message thread] Make `id` the add-clip target if it exists in the current snapshot. No-op if
    // unknown. **No** `sessionSnapshot_` republish; audio thread is unaffected.
    void setActiveTrack(TrackId id) noexcept;

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

    // [Message thread] Non-destructive right-edge **trim** (shorter or longer `visible` window in
    // [0, material) on the `PlacedClip` only; PCM unchanged). One snapshot publish; no lane reorder.
    void setClipRightEdgeVisibleLength(PlacedClipId id, std::int64_t newVisibleLengthSamples) noexcept;

    // [Message thread] Non-destructive left-edge trim (material offset L); one snapshot publish.
    void setClipLeftEdgeTrim(PlacedClipId id, std::int64_t newLeftTrimSamples) noexcept;

    // [Message thread] Reorder the **track list** only. Each track’s clips and name are unchanged.
    // **Does not** change `activeTrackId_` (add-clip target follows the same id in the new row).
    void moveTrack(TrackId movedTrackId, int destIndex) noexcept;

    // [Message thread] Publish the *shared* empty `SessionSnapshot` (see
    // `SessionSnapshot::createEmpty`) — no clips, nothing to play or paint as waveform material.
    void clearClip() noexcept;

    // [Message thread] Front clip’s `AudioClip` (index 0); **bridge** for legacy call sites.
    // `ClipWaveformView` reads the full snapshot for multi-clip layout (Step 7).
    [[nodiscard]] const AudioClip* getCurrentClip() const noexcept;

    // [Message thread] **Content end** — max of (start+length) over placed clips (derived). Zero
    // with no material. For backward compat, `getTimelineLengthSamples()` is an alias; prefer
    // `getContentEndSamples()`.
    [[nodiscard]] std::int64_t getContentEndSamples() const noexcept;
    // Deprecated: use `getContentEndSamples()`.
    [[nodiscard]] std::int64_t getTimelineLengthSamples() const noexcept
    {
        return getContentEndSamples();
    }

    // [Message thread] **Arrangement** extent (playable / navigable): max of stored snapshot
    // `arrangementExtentSamples` and `getContentEndSamples()`. Also used by the audio engine run-end.
    [[nodiscard]] std::int64_t getArrangementExtentSamples() const noexcept;
    // Raw stored floor on the snapshot (for default seeding: do not clobber a loaded v3 value).
    [[nodiscard]] std::int64_t getStoredArrangementExtentSamples() const noexcept;
    // [Message thread] Grow-only stored arrangement extent; publishes a new snapshot. No-op if
    // `v` is not greater than the current stored value.
    void setArrangementExtentSamples(std::int64_t v) noexcept;

    // [Audio thread] and [Message thread] Acquire the current `SessionSnapshot` pointer; no
    // decode, no session mutation. This is the main handoff the engine uses each block.
    [[nodiscard]] std::shared_ptr<const SessionSnapshot> loadSessionSnapshotForAudioThread() const noexcept;

    // [Message thread] Last successfully **saved** or **loaded** project file (empty if never set).
    // Used by the app for project-relative paths (e.g. takes/). Not part of the snapshot.
    [[nodiscard]] juce::File getCurrentProjectFile() const noexcept { return currentProjectFile_; }
    // [Message thread] True if the user has a known on-disk project (save or load completed).
    [[nodiscard]] bool hasKnownProjectFile() const noexcept;

    // [Message thread] Write minimal project v1 (tracks, clip placements, absolute source paths,
    // monotonic id seeds, active track, playhead and device rate metadata). `transport` is read
    // for the playhead only (single owner of playhead state).
    [[nodiscard]] juce::Result saveProjectToFile(
        Transport& transport, const juce::File& file, double deviceSampleRate);

    // [Message thread] Parse and decode in one new snapshot, single `atomic_store` on success.
    // `Transport&` is used only to `requestSeek` after publish (clamped; playhead is not read from
    // the file for anything else). Clips that fail to load are skipped; each becomes one line in
    // `outSkippedClipDetails` (path + reason). `outInfoNote` is non-empty e.g. when device rate at
    // save differs from `deviceSampleRate` (user-visible context for partial load).
    [[nodiscard]] juce::Result loadProjectFromFile(
        Transport& transport,
        const juce::File& file,
        double deviceSampleRate,
        juce::StringArray& outSkippedClipDetails,
        juce::String& outInfoNote);

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

    juce::File currentProjectFile_;
};
