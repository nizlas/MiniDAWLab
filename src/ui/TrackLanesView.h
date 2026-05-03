#pragma once

// =============================================================================
// TrackLanesView  —  one `ClipWaveformView` per `Track` (message thread)
// =============================================================================
//
// ROLE
//   Sits in the main layout **below** `TimelineRulerView`. When `Session` publishes a snapshot
//   with N tracks, this component ensures N child lanes, each created with a stable `TrackId` and
//   the same session-wide x -> sample map as the ruler. It wires a small callback so selecting a
//   clip in one lane clears selection in the others. **Cross-track drag:** `ClipWaveformLaneHost`
//   callbacks resolve which lane is under the pointer, set a **single** drop ghost on that lane, and
//   clear ghosts — **no** track-type predicate; “valid lane” is geometric only (the header strip
//   is not a lane — pointer over a header is not a valid drop). **Header drag** (track reorder) is
//   a separate gesture: `TrackHeaderView` past-threshold drags are coordinated here (insert line in
//   `paintOverChildren` only in the **header column** width (same as `kTrackHeaderWidth` cap in
//   `resized`), `Session::moveTrack` on commit; lane / clip drag unchanged). No-op drag: red line
//   follows pointer y; valid reorder: green line at snapped gap. **Delete track:** `TrackHeaderView`
//   posts `onDeleteTrackRequested(TrackId)` from its context menu; `Main` wires that to
//   `Session::removeTrack` (not keyboard Delete). Optional **VST3** actions via `setTrackHeaderPluginHost`.
//
// See: `Session::getNumTracks` / `getTrackIdAtIndex`, `ClipWaveformView`, `TrackHeaderView`.
// =============================================================================

#include "domain/Track.h"
#include "domain/PlacedClip.h"
#include "engine/RecorderService.h"
#include "ui/ClipWaveformView.h"
#include "ui/TrackHeaderView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace juce
{
    class AudioDeviceManager;
} // namespace juce

class Session;
class Transport;
class TimelineViewportModel;
class LatencySettingsStore;

// ---------------------------------------------------------------------------
// TrackLanesView — vertical stack of per-track event lanes
// ---------------------------------------------------------------------------
// Drains `RecorderService` preview-peak SPSC **once** (this timer) so lanes do not compete for the
// same preview FIFO. Passes a copy to the one lane whose `TrackId` matches the active take.
// ---------------------------------------------------------------------------
class TrackLanesView : public juce::Component, private juce::Timer
{
public:
    // Width of the left name/active strip. `Main` insets the timeline ruler by the same value so
    // the ruler’s x <-> session-sample map matches the lane area.
    static constexpr int kTrackHeaderWidth = 120;

    ~TrackLanesView() override;

    // [Message thread] `session` / `transport` / `timelineViewport` / `deviceManager` / `recorder`
    // / `latencySettingsStore` outlive this view. Rebuilds child lanes in `resized` to match the
    // current `SessionSnapshot` track list. Recording preview placement uses
    // `latencySettingsStore.getCurrentRecordingOffsetSamples()`.
    TrackLanesView(
        Session& session,
        Transport& transport,
        TimelineViewportModel& timelineViewport,
        juce::AudioDeviceManager& deviceManager,
        RecorderService& recorder,
        LatencySettingsStore& latencySettingsStore);

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;
    void mouseWheelMove(
        const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // [Message thread] `Main` can call this after `Session::addTrack` so a new `ClipWaveformView`
    // is created before layout without waiting for a user resize.
    void syncTracksFromSession();

    // Cycle recording preview: segment 0 spans [S, R); each later wrapped pass spans [L, R).
    // `actualRecordingStart` is the playhead at the moment recording began (may be < L, in [L,R),
    // or >= R). When >= R the live preview is linear from S (no wrap will be signalled).
    void setCycleRecordingPreviewContext(bool active,
                                         std::int64_t loopLeftSample,
                                         std::int64_t loopRightSample,
                                         std::int64_t actualRecordingStart,
                                         std::uint32_t wrapPassCountBaselineAtRecordingStart) noexcept;

    void clearCycleRecordingPreviewContext() noexcept;

    // [Message thread] Last clip the user selected on any lane (`TrackId` + placement id).
    [[nodiscard]] std::optional<std::pair<TrackId, PlacedClipId>> getAggregatedSelectedClip()
        const noexcept;
    // [Message thread] Clear other lanes, then select clip index 0 on `tid` (paste / host actions).
    void selectFrontPlacedClipOnTrack(TrackId tid) noexcept;

    // [Message thread] Select a specific placement on `tid` (clear other lanes). Used after split.
    void selectPlacedClipOnTrack(TrackId tid, PlacedClipId clipId) noexcept;

    // [Message thread] After a placement is removed from the session (e.g. Delete): clear aggregate
    // selection if it pointed at that clip and clear per-lane UI selection on that track.
    void notifyPlacedClipRemoved(TrackId trackId, PlacedClipId clipId) noexcept;

    // [Message thread] Clear all lane clip selections and the aggregate selection (e.g. after
    // `Session::restoreSessionSnapshotForUndo` when prior `PlacedClipId`s may be invalid).
    void clearAllPlacedClipSelections() noexcept;

    // [Message thread] Wired once by `Main`: header context menu "Delete Track" invokes this with the
    // clicked track id (Playing/recording + validity handled by host).
    void setOnDeleteTrackRequested(std::function<void(TrackId)> onDeleteTrackRequested) noexcept;

    // [Message thread] Wired once by `Main`: header context menu VST3 / editor / remove (optional).
    void setTrackHeaderPluginHost(TrackHeaderPluginHost host) noexcept;

    // [Message thread] Wired once by `Main`: committed clip move (real gesture only; see `ClipWaveformView`).
    void setOnUndoableClipMoveRequested(
        std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> fn) noexcept;

    void setOnUndoableClipTrimRequested(
        std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> fn) noexcept;

    void setActiveEditToolProvider(std::function<EditTool()> fn) noexcept;

    void setOnUndoableClipSplitRequested(
        std::function<void(PlacedClipId, std::int64_t, bool)> fn) noexcept;

    /** True while a clip move or trim gesture is in flight on any lane (undo/redo should no-op). */
    [[nodiscard]] bool isClipEditGestureInProgress() const noexcept;

private:
    void timerCallback() override;

    // [Message thread] When not recording, clears accumulated preview; when recording, drains the
    // FIFO to `recordingPreviewPeaks_` and updates the matching lane’s overlay.
    void updateRecordingPreviewOverlaysFromRecorder();
    // [Message thread] Match `std::vector` size and `TrackId` order to the session snapshot; id-
    // order changes (not in this project) would rebuild every lane.
    void rebuildChildLanesIfNeeded();

    void onLanePlacedClipSelectionChanged(TrackId laneTrackId, std::optional<PlacedClipId> id) noexcept;

    // [Message thread] Screen point → which child `ClipWaveformView` (lane) that point falls in, or
    // `nullptr` if outside this view’s bounds (e.g. over the ruler or chrome).
    [[nodiscard]] ClipWaveformView* findLaneAtScreenPosition(juce::Point<int> screenPos);
    void setGhostOnLaneImpl(ClipWaveformView* target, std::int64_t startSample, std::int64_t lengthSamples);
    void clearAllGhostsImpl();

    // [Message thread] Track-reorder by header drag: `movedId` is stable; recompute `s` from
    // snapshot on each move. Green line: y from `insertGapK_` (0..N). Red no-op line: `noopLineY_`
    // tracks pointer (valid header strip only).
    void beginHeaderTrackDrag(TrackId movedId, TrackHeaderView& sourceView);
    void updateHeaderTrackDrag(TrackId movedId, juce::Point<int> screenPos);
    void endHeaderTrackDrag(TrackId movedId);
    void clearHeaderTrackDragState() noexcept;
    [[nodiscard]] int yForInsertGapK(int k) const noexcept;

    Session& session_;
    Transport& transport_;
    TimelineViewportModel& timelineViewport_;
    juce::AudioDeviceManager& deviceManager_;
    RecorderService& recorder_;
    LatencySettingsStore& latencyStore_;
    std::vector<std::unique_ptr<TrackHeaderView>> headers_;
    std::vector<std::unique_ptr<ClipWaveformView>> lanes_;

    // In-order preview blocks for the current take; cleared whenever `!isRecording()`; appended
    // while recording as `drainNextPreviewBlock` returns data. Not session state.
    std::vector<RecordingPreviewPeakBlock> recordingPreviewPeaksAccum_;

    // Cycle recording: one peak-block vector per completed loop pass (oldest first). View-only;
    // cleared when recording stops or cycle preview context clears.
    std::vector<std::vector<RecordingPreviewPeakBlock>> cycleRecordingCompletedPassPeaks_;

    bool cyclePreviewActive_ = false;
    std::int64_t cyclePreviewLocL_ = 0;
    std::int64_t cyclePreviewLocR_ = 0;
    /// Playhead at cycle-recording start, captured by `Main`. Used to anchor segment 0 at S
    /// (length R−S) instead of at L. When S >= R the live preview falls back to linear.
    std::int64_t cyclePreviewActualStart_ = 0;
    std::uint32_t cyclePreviewWrapBaseline_ = 0;
    std::uint32_t cyclePreviewLastSeenWrap_ = 0;

    // Header-drag reorder (UI only until commit)
    bool headerTrackDragActive_ = false;
    TrackId headerTrackDragId_ = kInvalidTrackId;
    TrackHeaderView* headerTrackDragSourceView_ = nullptr;
    int headerTrackDragInsertGapK_ = -1; // 0..N for green snapped line; -1 when using noop pointer line
    int headerTrackDragNoopLineY_ = -1;  // valid when no-op + in valid strip: pointer y for red line
    bool headerTrackDragInvalidArea_ = true;
    bool headerTrackDragNoop_ = true;
    int headerTrackDragDestIndex_ = -1; // for commit; valid when !invalid && !noop

    std::optional<std::pair<TrackId, PlacedClipId>> aggregatedSelectedPlacedClip_;

    TrackHeaderPluginHost trackHeaderPluginHost_{};
    std::function<void(TrackId)> onDeleteTrackRequested_;
    std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> onUndoableClipMoveRequested_;
    std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> onUndoableClipTrimRequested_;
    std::function<EditTool()> activeEditToolProvider_;
    std::function<void(PlacedClipId, std::int64_t, bool)> onUndoableClipSplitRequested_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackLanesView)
};
