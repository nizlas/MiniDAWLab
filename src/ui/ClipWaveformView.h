#pragma once

// =============================================================================
// ClipWaveformView  —  session-timeline *events* + playhead (message thread only)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Read-only view: `Session` snapshot and derived timeline length, `Transport` for playhead.
//   No `PlaybackEngine`, no device-side audio. ARCHITECTURE_PRINCIPLES: presentation separate from
//   playback; UI does not own transport truth. Seek is not initiated here; use `TimelineRulerView`.
//
// PRESENTATION (DAW / Cubase direction, Phase 2)
//   **Each** `PlacedClip` row: one event **frame**; **peaks** are drawn in timeline regions that are
//   *not* covered in session time by any row in front of it (index `< r` in the snapshot / paint
//   stack). In **covered** regions on a back row, no readable peak sketch. The overlap *hint* (dark
//   wash + very thin diagonals) is drawn on **whichever** row is the **local** topmost in time over
//   at least one **older** row (higher index): not “row 0 only,” but the row that is visually on
//   top in that x-range. **No** global “always dim because behind somewhere.” Same `Transport` axis,
//   view only.
//
// THREADING
//   juce::Component and Timer: [Message thread] only. Timer repaints; no audio path.
//
// NOT RESPONSIBLE FOR
//   File decode, or ordering *policy* (Session / SessionSnapshot). The view **invokes** `Session
//   ::moveClip` / `Session::moveClipToTrack` on committed drag; it does not decide promote-vs-
//   preserve. Transport truth stays in `Transport`.
//
// See: ClipWaveformView.cpp (paint pipeline, overlap handling, JUCE `Graphics` notes).
// =============================================================================

#include "domain/Session.h"
#include "domain/Track.h"
#include "engine/RecorderService.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class Transport;
class TimelineViewportModel;
class ClipWaveformView;

// [Message thread] At the start of a lane `mouseDown`, clear selection on **other** lanes only.
using PeerLaneInteraction = std::function<void(ClipWaveformView&)>;

// [Message thread] Optional cross-lane coordination from `TrackLanesView`: find which lane is under
// a screen point, show a **single** drag ghost on that lane, clear all ghosts. Empty `std::function`
// = same-lane-only behavior (legacy).
struct ClipWaveformLaneHost
{
    PeerLaneInteraction onBeginMouseDown;
    std::function<ClipWaveformView*(juce::Point<int> screenPos)> findLaneAtScreen;
    std::function<void(ClipWaveformView* target, std::int64_t startSample, std::int64_t lengthSamples)>
        setGhostOnLane;
    std::function<void()> clearAllGhosts;

    /** When this lane's clip selection (`selectedPlacedId_`) changes. See `publishPlacedClipSelectionToLaneHost`. */
    std::function<void(TrackId laneTrackId, std::optional<PlacedClipId> placedId)> onPlacedClipSelectionChanged;

    /** Undo-2: completed clip move only. Host wraps `Session::moveClip` / `moveClipToTrack` in history.
        `destTrackOrNullForSameLane` is `nullopt` when the clip stays on this lane; else target track.
        Returns false when the move was not applied (e.g. host blocked by transport). */
    std::function<bool(PlacedClipId clipId,
                       std::int64_t newStartSamples,
                       std::optional<TrackId> destTrackOrNullForSameLane)>
        commitClipMoveAsUndoable;
};

// ---------------------------------------------------------------------------
// ClipWaveformView — multi-clip timeline *view* (Session snapshot + Transport playhead)
// ---------------------------------------------------------------------------
// Responsibility: turn the current `SessionSnapshot` into rectangles + downsampled peaks and a
// playhead line. Owns no audio, no `Transport` truth, no clip ordering: it **reads** published
// state only. Phase 2: per-row z-order (index 0 = newest) matches engine coverage **semantics** in
// spirit (top row wins) but the **shading** rules are a **separate** product choice for legibility
// (see .cpp: local topmost + underlap hint).
//
// Threading: [Message thread] for construction, `paint`, mouse, and timer. Never called from the
// audio callback.
//
// Not responsible for: file decode, clip ordering *rules*, or transport ownership.
// ---------------------------------------------------------------------------
class ClipWaveformView : public juce::Component, private juce::Timer
{
public:
    // [Message thread] session/transport/timelineViewport outlive the view. `trackId` scopes this
    // lane to one `Track` in the snapshot. `laneHost` is normally from `TrackLanesView` (cross-lane
    // ghost + drop find). Default = only within-lane `moveClip` on drag commit.
    ClipWaveformView(Session& session,
                     Transport& transport,
                     TrackId trackId,
                     TimelineViewportModel& timelineViewport,
                     ClipWaveformLaneHost laneHost = {});
    ~ClipWaveformView() override;

    [[nodiscard]] TrackId getTrackId() const noexcept { return trackId_; }

    // [Message thread] True while a move or trim is active (block timeline wheel pan in parent).
    [[nodiscard]] bool isTimelineEditGestureInProgress() const noexcept;

    // [Message thread] True during an in-flight **clip move** (mouse down on body … mouse up). Narrows
    // `isTimelineEditGestureInProgress` so trim drags do not block undo/redo shortcuts.
    [[nodiscard]] bool isClipMoveGestureInProgress() const noexcept;

    // [Message thread] Clear UI selection without starting a move (used from `TrackLanesView`).
    void clearSelectionOnly();

    // [Message thread] Select a placement from host code (`TrackLanesView` after paste).
    void applyExternalPlacedClipSelection(std::optional<PlacedClipId> id) noexcept;
    // [Message thread] Cross-lane drag ghost (one lane at a time); called by `TrackLanesView` only.
    void setDragGhost(std::int64_t startSampleOnTimeline, std::int64_t lengthSamples);
    void clearDragGhost();

    // [Message thread] Live recording preview only (not `SessionSnapshot`). `TrackLanesView` owns
    // draining + passes a copy here for the armed track while `RecorderService::isRecording()`.
    void setRecordingPreviewOverlay(
        std::int64_t startSampleOnTimeline,
        std::int64_t lengthSamples,
        const std::vector<RecordingPreviewPeakBlock>& peakBlocks);
    void clearRecordingPreviewOverlay();

    // Cycle OD: stacked completed-pass previews + current-pass overlay on top.
    // The very first completed pass (`completedPassesOlderFirst[0]`, oldest = "segment 0") is
    // anchored at `firstSegmentTimelineStart` with width `firstSegmentLengthSamples`. All later
    // completed passes ([1..]) sit at `loopLeftSample` with width `passWindowSamples` (one normal
    // wrapped pass each). This split is needed because cycle recording can start at S != L: the
    // first segment spans [S, R) while every wrapped pass spans [L, R).
    void setRecordingCyclePassPreviewLayers(
        const std::vector<std::vector<RecordingPreviewPeakBlock>>& completedPassesOlderFirst,
        std::int64_t firstSegmentTimelineStart,
        std::int64_t firstSegmentLengthSamples,
        std::int64_t loopLeftSample,
        std::int64_t passWindowSamples,
        std::int64_t currentStartSampleOnTimeline,
        std::int64_t currentVisibleLengthSamples,
        const std::vector<RecordingPreviewPeakBlock>& currentPeaks);
    void clearRecordingCyclePassPreviewLayers() noexcept;

    // [Message thread] Paints: background, back→front one **event** per `PlacedClip` (peaks in
    // *uncovered* time only), then the same overlap shading (per row, where that row is locally
    // topmost over something behind), then playhead. No audio thread.
    void paint(juce::Graphics& g) override;

    // [Message thread] Click on *event* → select; click on *empty lane* → clear selection (seek is
    // on `TimelineRulerView` only). Drag on event → in-flight move preview; release → `Session::moveClip` (commit).
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

private:
    // [Message thread] Timer: schedules full `repaint` at a low fixed rate (see .cpp); playhead is sampled in
    // `paint` so the line tracks `Transport` without storing a cached position on the view.
    void timerCallback() override;

    // [Message thread] Rebuilds per-clip `peaks` when snapshot identity or view width changes.
    // O(view width) per clip, bounded columns — same contract as before.
    void rebuildPeaksIfNeeded();

    // [Message thread] A timeline sample t is *covered* (for painting a lower row r) if any row
    // with a lower index in the snapshot — drawn later, “in front” — owns that half-open sample.
    // Used only to **suppress** underlap peak columns locally; not transport truth.
    bool isTimelineSampleCoveredByPriorRows(int row, std::int64_t t) const;

    // [Message thread] Half-open [L,R) intervals in session time where this row is the *minimum
    // index* covering t (it wins the paint stack) and some row with higher index also covers t —
    // the only regions where the overlap *hint* belongs on *this* event. View-only; merged output.
    void computeLocalOverlapShadeHalfOpenIntervalsForRow(
        int row,
        std::vector<std::pair<std::int64_t, std::int64_t>>& outMerged) const;

    // [Message thread] If the selected id no longer exists in the snapshot, clear selection.
    void clearSelectionIfIdMissing(const std::shared_ptr<const SessionSnapshot>& snap);

    /** Forwards current `selectedPlacedId_` to `laneHost_.onPlacedClipSelectionChanged` when set. */
    void publishPlacedClipSelectionToLaneHost() noexcept;

    // [Message thread] Invalid-drop cursor on the **source** lane when the pointer leaves the lane
    // stack during a drag; `getForbiddenNoDropMouseCursor` (`ForbiddenCursor.h`), not a JUCE
    // `StandardCursorType` no-drop.
    // Always restored with `NormalCursor` on re-entry to a lane and on `mouseUp`.
    void setInvalidDropCursor();
    void restoreNormalCursorAfterInvalidDrop();

    // [Message thread] Trim hover: bottom-right handle cue and resize cursor; no session publish.
    void updateTrimHoverAndCursor(juce::Point<float> localPosition) noexcept;
    // Which clip shows the static trim **cue** (on event body) / per-edge handle (resize on that edge only).
    std::optional<PlacedClipId> hoverEventTrimCueId_;
    std::optional<PlacedClipId> hoverLeftTrimHandleId_;
    std::optional<PlacedClipId> hoverRightTrimHandleId_;

    // Which `Track` this lane paints; overlap + paint order are **only** within this list.
    TrackId trackId_ = kInvalidTrackId;
    ClipWaveformLaneHost laneHost_;
    Session& session_;
    Transport& transport_;
    TimelineViewportModel& timelineViewport_;

    // Snapshot `shared_ptr` raw address — a new publish yields a new `const SessionSnapshot` and
    // a different pointer, so the cache key stays correct without touching Session.
    const void* lastSnapshotKey_ = nullptr;
    int lastWidth_ = 0;
    std::int64_t lastVisibleStartForPeaks_ = 0;
    std::int64_t lastVisibleLengthForPeaks_ = 0;

    // Cached paint inputs: one entry per `getPlacedClip(i)` row (0 = front / newest in snapshot).
    struct TimelineStrip
    {
        PlacedClipId clipId{ kInvalidPlacedClipId };
        std::int64_t startOnTimeline = 0;
        // Effective audible length V; material indices for PCM are [L, L + V).
        std::int64_t leftTrimSamples = 0;
        // Effective (placement) span: right-edge trim shortens the audible/painted region; not material size.
        int materialNumSamples = 0;
        std::vector<float> peaks;
    };
    std::vector<TimelineStrip> clipStrips_;
    std::uint64_t lastPeaksFingerprint_ = 0;

    // UI-local selection; never published in `SessionSnapshot` (see `PHASE_PLAN` / `ARCHITECTURE_…`).
    std::optional<PlacedClipId> selectedPlacedId_;

    // Move vs. trim: trim never calls `Session::moveClip`.
    enum class PointerLaneMode { None, MoveClip, TrimRight, TrimLeft };
    PointerLaneMode pointerLaneMode_ = PointerLaneMode::None;
    // Trim in flight (separate from move preview).
    std::optional<PlacedClipId> trimPlacedId_;
    std::int64_t trimStartSample_ = 0; // S0 (left edge of event on timeline)
    // Right trim: [0, M - L) tail length for view mapping. Left trim: origin L0 at mousedown.
    int trimMaterialNumSamples_ = 0;
    std::int64_t trimOriginLeft_ = 0;
    std::int64_t trimClickDownVisibleLen_ = 0;
    std::int64_t trimRightEdgeToMouseOffsetSamples_ = 0; // (start+visible) - sampleAt(clickX) [right trim]
    std::int64_t trimMouseOffsetToTimelineAtClick_ = 0;  // sampleAt(click) - S0 [left trim]
    std::int64_t trimPreviewVisibleLen_ = 0;
    std::int64_t trimPreviewLeft_ = 0;    // Lp (left trim preview)
    std::int64_t trimPreviewStart_ = 0;   // Sp

    // In-flight single-clip drag: preview uses `tentativeStartOnTimeline_` for the event rect only
    // after a movement threshold. Commit calls `Session::moveClip` on mouse up.
    std::optional<PlacedClipId> mouseDownPlacedId_;
    float clickDownX_ = 0.0f;
    std::int64_t clickDownStartSample_ = 0;
    std::int64_t tentativeStartOnTimeline_ = 0;
    bool dragMovementBeyondThreshold_ = false;
    // Effective span at mousedown (cross-lane ghost, same-lane move); not always full material.
    std::int64_t mouseDownEffectiveNumSamples_ = 0;

    // Drop ghost (this component may be the non-source lane showing a placeholder only).
    bool hasDragGhost_ = false;
    std::int64_t dragGhostStartOnTimeline_ = 0;
    std::int64_t dragGhostLengthSamples_ = 0;

    bool cursorOverriddenForInvalidDrop_ = false;

    // UI-only live take preview (see `setRecordingPreviewOverlay`); cleared when recording stops.
    bool recordingPreviewActive_ = false;
    std::int64_t recordingPreviewStartSample_ = 0;
    std::int64_t recordingPreviewLengthSamples_ = 0;
    std::vector<RecordingPreviewPeakBlock> recordingPreviewPeaks_;

    bool recordingCycleBehindLayersActive_ = false;
    std::vector<std::vector<RecordingPreviewPeakBlock>> recordingCycleBehindPasses_;
    std::int64_t recordingCycleLoopAnchorL_ = 0;
    std::int64_t recordingCyclePassWindowLenSamples_ = 0;
    /// Anchor + length used **only** for `recordingCycleBehindPasses_[0]` (the variable-length
    /// segment 0 that begins at the actual recording start S, ends at R). Indices 1..N-1 use the
    /// shared loop anchor + pass window.
    std::int64_t recordingCycleFirstSegmentStart_ = 0;
    std::int64_t recordingCycleFirstSegmentLength_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipWaveformView)
};
