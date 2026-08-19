#pragma once

// =============================================================================
// TrackLanesView  —  one `ClipWaveformView` per `Track` (message thread)
// =============================================================================
//
// ROLE
//   Occupies the lanes band under the menu/tool rows; its bounds include a fixed-height strip
//   aligned with the timeline row so the header column reads continuous with track headers.
//   When `Session` publishes a snapshot
//   with N tracks, this component ensures N child lanes, each created with a stable `TrackId` and
//   the same session-wide x -> sample map as the ruler. It wires a small callback so selecting a
//   clip in one lane clears selection in the others. **Cross-track drag:** `ClipWaveformLaneHost`
//   callbacks resolve which lane is under the pointer, set a **single** drop ghost on that lane, and
//   clear ghosts — **no** track-type predicate; “valid lane” is geometric only (the header strip
//   is not a lane — pointer over a header is not a valid drop). **Header drag** (track reorder) is
//   a separate gesture: `TrackHeaderView` past-threshold drags are coordinated here (insert line in
//   `paintOverChildren` only in the **header column** width (same as `kTrackHeaderWidth` cap in
//   `resized`), `Main` publishes `Session::moveTrack` inside undo via a single-row move on commit —
//   **including** the experimental Instrument lane (`TrackKind::Instrument` in `SessionSnapshot`).
//   No-op drag: red line follows pointer y; valid reorder: green line at snapped gap. **Delete track:**
//   `TrackHeaderView` posts `onDeleteTrackRequested(TrackId)` from its context menu; `Main` wires that
//   to `Session::removeTrack` (not keyboard Delete). Optional **VST3**
//   actions via `setTrackHeaderPluginHost`.
//
// See: `Session::getNumTracks` / `getTrackIdAtIndex`, `ClipWaveformView`, `TrackHeaderView`.
//   Same parent shell also owns the lane-column `PlayheadOverlay`. Optional **instrument timeline
//   rows** (`syncInstrumentTimelineAttachments`) share the stack and header-drag model.
// =============================================================================

#include "domain/Track.h"
#include "domain/PlacedClip.h"
#include "instruments/InstrumentTrackController.h"
#include "engine/RecorderService.h"
#include "ui/ClipWaveformView.h"
#include "ui/TrackHeaderView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
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
class AudioWaveformCache;

enum class VisibleTrackKind
{
    Audio,
    Instrument,
    Group,
    Master,
};

struct VisibleTrackEntry
{
    VisibleTrackKind kind;
    TrackId sessionTrackId = kInvalidTrackId;
};

/// Non-owning trio for one `TrackKind::Instrument` shell row (components owned by `Main`).
struct InstrumentTimelineAttachment
{
    TrackId sessionTrackId = kInvalidTrackId;
    InstrumentTrackController* controller = nullptr;
    TrackHeaderView* header = nullptr;
    juce::Component* midiLane = nullptr;
};

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

    /// Height of the timeline row band shared with `TimelineRulerView` / transport layout (px).
    /// Track rows scroll only below this; the header-column gutter above the first row matches this.
    static constexpr int kArrangementTimelineHeaderGutterPx = 28;

    ~TrackLanesView() override;

    // [Message thread] `session` / `transport` / `timelineViewport` / `deviceManager` / `recorder`
    // / `latencySettingsStore` / `waveformCache` outlive this view. Rebuilds child lanes in
    // `resized` to match the current `SessionSnapshot` track list. Recording preview placement uses
    // `latencySettingsStore.getCurrentRecordingOffsetSamples()`.
    TrackLanesView(
        Session& session,
        Transport& transport,
        TimelineViewportModel& timelineViewport,
        juce::AudioDeviceManager& deviceManager,
        RecorderService& recorder,
        LatencySettingsStore& latencySettingsStore,
        AudioWaveformCache& waveformCache);

    void resized() override;
    void paint(juce::Graphics& g) override;
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

    /// [Message thread] Same-track MIDI clip selection for arrangement shortcuts: prefers `Session`'s active
    /// track when it has a non-empty MIDI selection, otherwise the first instrument row with a selection.
    [[nodiscard]] std::optional<std::pair<TrackId, std::vector<InstrumentMidiClipId>>>
    getAggregatedSelectedInstrumentMidiClipSelection() const noexcept;
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

    // [Message thread] Cancel every lane’s clip gestures / ghosts / caches and clear header reorder
    // drag state. Prefer this over `clearAllPlacedClipSelections` after a session snapshot restore.
    void cancelAllClipGesturesAndTransientUiState() noexcept;

    // [Message thread] Wired once by `Main`: header context menu "Delete Track" invokes this with the
    // clicked track id (Playing/recording + validity handled by host).
    void setOnDeleteTrackRequested(std::function<void(TrackId)> onDeleteTrackRequested) noexcept;

    /// Same handler as wired by `setOnDeleteTrackRequested` (`Main` invokes from instrument-shell headers).
    void requestDeleteTrackForHeaderMenu(TrackId tid) noexcept;

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

    /// [Message thread] Undoable rename via `TrackLanesEditCoordinator` (`executeUndoableSessionEdit`).
    void setOnUndoableRenameTrackRequested(std::function<bool(TrackId, juce::String)> fn) noexcept;
    [[nodiscard]] bool invokeUndoableRenameTrackRequested(TrackId tid, juce::String proposedName) noexcept;

    /// [Message thread] Wired once by `Main`: when this returns true, audio header models force
    /// `m.active = false` regardless of `Session::getActiveTrackId()` (UI-only mutex with the
    /// experimental instrument header — no `Session` change).
    void setHeaderActiveSuppressProvider(std::function<bool()> fn) noexcept;

    /// [Message thread] Optional: block track-header structural actions (power/off, delete track, VST3
    /// menu) during recording, count-in, or **transport Playing** (`Main` installs this predicate).
    void setStructuralTimelineEditBlockedPredicate(std::function<bool()> fn) noexcept;

    /// [Message thread] Optional: block **instrument MIDI clip drag moves** on the timeline (`MidiEventLane`).
    /// Narrower than `isStructuralTimelineEditBlocked`: allows moves during playback; `Main` typically
    /// installs recording + count-in only.
    void setInstrumentMidiClipMoveBlockedPredicate(std::function<bool()> fn) noexcept;

    /// [Message thread] Wired once by `Main`: fires from any audio header's name-strip click after
    /// `Session::setActiveTrack` succeeds. `Main` uses this to clear the instrument-row active flag.
    void setOnAudioHeaderActivated(std::function<void()> fn) noexcept;

    /// Optional: after an audio clip lane clears peer waveform selections on mouse-down, invoke this
    /// so MIDI clip selections can be cleared without threading instrument details into `ClipWaveformView`.
    void setOnAudioClipMouseDownClearForeignSelections(std::function<void()> fn) noexcept;

    /// [Message thread] Audio header context menu: import WAV/etc. at playhead onto this track.
    void setOnAudioTrackImportClipAtPlayhead(std::function<void(TrackId)> fn) noexcept;

    /// [Message thread] Replace all instrument-shell UI bridges (Groove-Agent rows). Omit a `TrackId` to detach it.
    void syncInstrumentTimelineAttachments(const std::vector<InstrumentTimelineAttachment>& rows) noexcept;

    /// [Message thread] After shell id changes (`tryAddGrooveAgent…` / project restore), reinstall header-drag host.
    void refreshInstrumentHeaderReorderAttachments() noexcept;
    /// Undo-bundled reorder: publishes `session.moveTrack(movedId, destSessionIndex)` (see `Main`).
    void setCommittedHeaderDragTrackReorder(std::function<void(TrackId movedId, int destSessionIndex)> fn) noexcept;
    /// [Message thread] Rebuild visible track rows from canonical `SessionSnapshot::tracks_` order.
    void rebuildVisibleTrackEntries() noexcept;
    void rebuildMasterHeadersIfNeeded();
    void rebuildGroupHeadersIfNeeded();

    /** True when the instrument lane participates in visible layout (`hasInstrumentTrack` + bridged Instrument row). */
    [[nodiscard]] bool isInstrumentTimelineRowVisible() const noexcept;

    /** True while a clip move or trim gesture is in flight on any lane (undo/redo should no-op). */
    [[nodiscard]] bool isClipEditGestureInProgress() const noexcept;

    /// [Message thread] True when destructive header/timeline edits must not run (recording, count‑in,
    /// transport Playing, etc. — see `Main`’s installed `structuralTimelineEditBlockedPredicate`).
    [[nodiscard]] bool isStructuralTimelineEditBlocked() const noexcept;

    /// [Message thread] True when instrument MIDI clip drag-reposition should not run (see
    /// `setInstrumentMidiClipMoveBlockedPredicate`). Defaults to `RecorderService::isRecording()` when unset.
    [[nodiscard]] bool isInstrumentMidiClipMoveBlocked() const noexcept;

    /// [Message thread] Runtime-only row height drag (no undo, no persistence).
    void applyTrackRowHeightDelta(TrackId tid, int startHeightPx, int deltaPx) noexcept;

    /// Optional arrangement timeline snapping (Slice D): used by clip lanes when committing/editing.
    void setArrangementTimelineSnapFunction(std::function<std::int64_t(std::int64_t)> fn) noexcept;

    /// [Message thread] After header bottom-edge resize: snap to clean name-only or full name+buttons height.
    void snapTrackHeaderRowHeightAfterResize(TrackId tid, bool headerHasSubtitle) noexcept;

private:
    void timerCallback() override;

    /// Middle-button drag = horizontal hand-pan (grab-style: content follows the mouse). Registered
    /// with `addMouseListener(..., true)` so the gesture works over child lanes/headers too; the
    /// children themselves ignore middle-button events (see `ClipWaveformView` / `MidiEventLane`).
    struct MiddlePanMouseListener final : juce::MouseListener
    {
        explicit MiddlePanMouseListener(TrackLanesView& owner) noexcept : owner_(owner) {}
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        TrackLanesView& owner_;
    };
    void beginMiddlePan(float xInLanes) noexcept;
    void updateMiddlePan(float xInLanes) noexcept;
    void endMiddlePan() noexcept;

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

    // [Message thread] Track reorder by header drag (real `TrackId` for audio and instrument shells).
    // Green line: snapped gap index in visible row count; red line follows pointer while no-op.
    void beginHeaderTrackDrag(TrackId movedId, TrackHeaderView& sourceView);
    void updateHeaderTrackDrag(TrackId movedId, juce::Point<int> screenPos);
    void endHeaderTrackDrag(TrackId movedId);
    void clearHeaderTrackDragState() noexcept;
    [[nodiscard]] int yForVisibleInsertGapK(int k) const noexcept;
    [[nodiscard]] int audioLaneIndexFromTrackId(TrackId tid) const noexcept;
    [[nodiscard]] int rowHeightForTrack(TrackId tid) const noexcept;
    [[nodiscard]] int rowHeightForVisibleEntry(int visibleIndex) const noexcept;
    [[nodiscard]] int visibleRowPixelHeight(int visibleIndex) const noexcept;
    [[nodiscard]] int totalContentHeightPx() const noexcept;
    [[nodiscard]] int maxVerticalScrollOffsetPx() const noexcept;
    void setVerticalScrollOffsetPx(int newOffset) noexcept;
    [[nodiscard]] int findVisibleRowIndexForDragSource(TrackId movedId) const noexcept;
    [[nodiscard]] bool trackHeaderModelUsesSubtitle(TrackId tid) const noexcept;
    [[nodiscard]] int minimumRowHeightPxForTrackHeader(TrackId tid) const noexcept;

    void prunePerTrackRowHeightsNotInSession() noexcept;
    void paintHeaderColumnHorizontalRowSeparators(juce::Graphics& g) const noexcept;
    void setTrackRowHeightPx(TrackId tid, int heightPx) noexcept;

    Session& session_;
    Transport& transport_;
    TimelineViewportModel& timelineViewport_;
    juce::AudioDeviceManager& deviceManager_;
    RecorderService& recorder_;
    LatencySettingsStore& latencyStore_;
    AudioWaveformCache& waveformCache_;
    std::vector<std::unique_ptr<TrackHeaderView>> headers_;
    std::vector<std::unique_ptr<ClipWaveformView>> lanes_;
    std::vector<std::unique_ptr<TrackHeaderView>> masterHeaders_;
    std::unordered_map<TrackId, std::unique_ptr<TrackHeaderView>> groupHeaders_;

    /// Flattened snapshot order (`Audio`: `lanes_`/`headers_` indices; `Instrument`: bridged attachments).
    std::vector<VisibleTrackEntry> visibleTrackEntries_;

    int defaultRowHeightPx_ = 96;
    int maxRowHeightPx_ = 480;
    int verticalScrollOffsetPx_ = 0;

    MiddlePanMouseListener middlePanListener_{ *this };
    bool middlePanActive_ = false;
    float middlePanLastX_ = 0.0f;

    std::unordered_map<TrackId, int> perTrackRowHeightPx_;

    std::unordered_map<TrackId, InstrumentTimelineAttachment> instrumentTimelineAttachments_;
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
    int headerTrackDragInsertGapK_ = -1; // 0..V for green snapped line; -1 when using noop pointer line
    int headerTrackDragNoopLineY_ = -1;  // valid when no-op + in valid strip: pointer y for red line
    bool headerTrackDragInvalidArea_ = true;
    bool headerTrackDragNoop_ = true;

    std::optional<std::pair<TrackId, PlacedClipId>> aggregatedSelectedPlacedClip_;

    std::function<void(TrackId, int)> committedHeaderDragTrackReorder_;

    TrackHeaderPluginHost trackHeaderPluginHost_{};
    std::function<void(TrackId)> onDeleteTrackRequested_;
    std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> onUndoableClipMoveRequested_;
    std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> onUndoableClipTrimRequested_;
    std::function<EditTool()> activeEditToolProvider_;
    std::function<void(PlacedClipId, std::int64_t, bool)> onUndoableClipSplitRequested_;
    std::function<bool()> headerActiveSuppressProvider_;
    std::function<void()> onAudioHeaderActivated_;
    std::function<void()> onAudioClipMouseDownClearForeignSelections_;
    std::function<bool()> structuralTimelineEditBlockedPredicate_;
    std::function<bool()> instrumentMidiClipMoveBlockedPredicate_;
    std::function<std::int64_t(std::int64_t)> arrangementTimelineSnap_;
    std::function<void(TrackId)> onAudioTrackImportClipAtPlayhead_;
    std::function<bool(TrackId, juce::String)> onUndoableRenameTrackRequested_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackLanesView)
};
