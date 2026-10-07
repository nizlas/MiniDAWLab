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
//   `paintOverChildren` only in the **header column** width (same `headerColumnWidthPx()` as in
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
#include "ui/SoloUiHooks.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackRowHeightPresets.h"

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

/// [Message thread] Visual-track-group commands the owner (`Main`) wires: Create / Rename /
/// Ungroup run inside the narrow group-metadata undo; `setCollapsed` is a display change (dirty
/// only, no undo entry). All are layout metadata — never a snapshot publish.
struct VisualTrackGroupUiHooks
{
    std::function<void(juce::String name, std::vector<TrackId> memberTrackIds)> createGroup;
    std::function<void(int groupId, juce::String newName)> renameGroup;
    std::function<void(int groupId)> ungroup;
    std::function<void(int groupId, bool collapsed)> setCollapsed;
};

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
    // -------------------------------------------------------------------------------------------
    // Header column width — ONE shared boundary for every row kind (audio, instrument, MIDI, group,
    // master), the lane-area x-origin, the ruler inset and the playhead overlay (`Main` reads
    // `effectiveTrackHeaderColumnWidthPxForTotalWidth` in `applyTransportControlsLayout`). Runtime
    // state, user-resizable with the drag handle on the header/timeline boundary; the owner
    // persists it app-wide (`UiLayoutSettingsStore`). Never per track, never changed by names.
    // Limits are logical (DPI-independent) px derived from the real control-strip layout.
    // -------------------------------------------------------------------------------------------
    /// 154 px: the widest button row ([Instrument][Power][Mute][Solo][Monitor][Arm]) fully inside
    /// the chrome. Saved preferences below this (pre-Solo layouts stored 132 … 144) clamp UP on
    /// load/display via `clampHeaderColumnWidthForTotalWidth`; larger saved widths are preserved.
    static constexpr int kTrackHeaderColumnMinWidthPx = TrackHeaderView::kMinimumHeaderColumnWidthPx;
    /// 166 px: minimum + 12 px margin.
    static constexpr int kTrackHeaderColumnDefaultWidthPx = TrackHeaderView::kDefaultHeaderColumnWidthPx;
    static constexpr int kTrackHeaderColumnMaxWidthPx = 480;
    /// Lane area kept visible right of the column on narrow windows (effective width clamps to it).
    static constexpr int kMinimumLaneAreaWidthPx = 160;
    /// Drag handle: a band centred on the boundary (3 px on each side) that owns its mouse events,
    /// so a width drag can never start a clip move, a row-height drag or a header reorder.
    static constexpr int kHeaderColumnResizeHandleWidthPx = 6;

    /// [Message thread] Stored preference, clamped to [min, max].
    [[nodiscard]] int getTrackHeaderColumnWidthPx() const noexcept { return trackHeaderColumnWidthPx_; }
    /// [Message thread] Clamps, re-lays out this view and (when `notifyOwner`) fires
    /// `onTrackHeaderColumnWidthChanged(width, dragEnded=false)` so the owner re-lays out ruler/overlay.
    void setTrackHeaderColumnWidthPx(int widthPx, bool notifyOwner = true) noexcept;
    /// The one clamp formula (pure, testable): the preference, but never leaving less than
    /// `kMinimumLaneAreaWidthPx` of lane area, never below the minimum unless the whole view is
    /// narrower than that, never wider than the view.
    [[nodiscard]] static constexpr int clampHeaderColumnWidthForTotalWidth(const int preferredWidthPx,
                                                                           const int totalWidthPx) noexcept
    {
        if (totalWidthPx <= 0)
        {
            return 0;
        }
        const int roomForColumn = totalWidthPx - kMinimumLaneAreaWidthPx > kTrackHeaderColumnMinWidthPx
                                      ? totalWidthPx - kMinimumLaneAreaWidthPx
                                      : kTrackHeaderColumnMinWidthPx;
        const int clamped = preferredWidthPx < kTrackHeaderColumnMinWidthPx ? kTrackHeaderColumnMinWidthPx
                            : preferredWidthPx > roomForColumn              ? roomForColumn
                                                                            : preferredWidthPx;
        return clamped < totalWidthPx ? clamped : totalWidthPx;
    }
    /// Effective width for a view of `totalWidthPx` (`clampHeaderColumnWidthForTotalWidth` of the
    /// stored preference). Same formula everywhere: this view, the transport layout, the overlay.
    [[nodiscard]] int effectiveTrackHeaderColumnWidthPxForTotalWidth(int totalWidthPx) const noexcept;
    /// Effective width for the current bounds (what `resized()` / paint / hit-testing use).
    [[nodiscard]] int headerColumnWidthPx() const noexcept
    {
        return effectiveTrackHeaderColumnWidthPxForTotalWidth(getWidth());
    }
    /// [Message thread] Owner hook: every change (`dragEnded == false`) + once on handle release
    /// (`dragEnded == true`, the moment to persist — no disk I/O per mouse move).
    void setOnTrackHeaderColumnWidthChanged(std::function<void(int widthPx, bool dragEnded)> fn) noexcept;
    /// Diagnostics / UI tests: bounds of the resize handle in this view's coordinates.
    [[nodiscard]] juce::Rectangle<int> getHeaderColumnResizeHandleBoundsForDiagnostics() const noexcept;
    /// Stability runner: the exact sequence the handle performs for one drag — anchor at the
    /// current effective width, apply `deltaPx`, then the drag-ended notification (persist).
    void simulateHeaderColumnHandleDragForStabilityTest(int deltaPx) noexcept;
    /// [Diagnostics] The attached instrument / MIDI row header for `tid`, or null.
    [[nodiscard]] const TrackHeaderView* findInstrumentRowHeaderForDiagnostics(TrackId tid) const noexcept;
    /// [Stability] Mutable access to the same header (to click its cells like the mouse does).
    [[nodiscard]] TrackHeaderView* findInstrumentRowHeaderForStabilityTest(TrackId tid) noexcept
    {
        return const_cast<TrackHeaderView*>(findInstrumentRowHeaderForDiagnostics(tid));
    }
    /// Diagnostics (stability runner): checks every visible header of every row kind against the
    /// shared boundary — header width == effective column width, every present strip cell /
    /// alternatives button fully inside the header, and every lane starting exactly at the boundary.
    /// Appends one line per header to `report`; returns false with `failReason` on the first violation.
    [[nodiscard]] bool verifyHeaderColumnLayoutForDiagnostics(juce::String& report, juce::String& failReason) const;

    /// [Stability] Vertical layout check: offset within range, every row at its model position,
    /// header + lane of one row sharing y / height, scrolled-out rows collapsed, heights summing
    /// to the model's content height. Appends a per-row report.
    [[nodiscard]] bool verifyVerticalScrollLayoutForDiagnostics(juce::String& report, juce::String& failReason) const;
    /// [Stability] Visible-row index of a track (-1 when absent) and its top offset in content px.
    [[nodiscard]] int visibleRowIndexForTrackForDiagnostics(TrackId tid) const noexcept;
    [[nodiscard]] int rowTopOffsetPxForTrackForDiagnostics(TrackId tid) const noexcept;
    /// [Stability] Open the header context menu of a row like a right click (audio / group /
    /// instrument / MIDI-content headers). False when the row has no laid-out header.
    bool openHeaderContextMenuForStabilityTest(TrackId tid);
    /// [Stability] Same setter the row-height drag ends in (clamped to the row's minimum / max).
    void setTrackRowHeightPxForStabilityTest(const TrackId tid, const int heightPx) noexcept { setTrackRowHeightPx(tid, heightPx); }

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

    // -------------------------------------------------------------------------------------------
    // Vertical scroll model (the ONE model behind the wheel, the arrangement scrollbar and the
    // header + lane layout). Offsets are px of the row stack below the timeline gutter.
    // -------------------------------------------------------------------------------------------
    struct VerticalScrollModel
    {
        int contentHeightPx = 0;  ///< sum of the visible rows' heights (varying row heights included)
        int viewportHeightPx = 0; ///< view height minus `kArrangementTimelineHeaderGutterPx`
        int offsetPx = 0;         ///< current clamped offset (0 .. max(0, content - viewport))

        [[nodiscard]] int maxOffsetPx() const noexcept { return juce::jmax(0, contentHeightPx - viewportHeightPx); }
        [[nodiscard]] bool everythingFits() const noexcept { return contentHeightPx <= viewportHeightPx; }
        [[nodiscard]] bool operator==(const VerticalScrollModel& o) const noexcept
        {
            return contentHeightPx == o.contentHeightPx && viewportHeightPx == o.viewportHeightPx
                && offsetPx == o.offsetPx;
        }
    };

    /// Current model as laid out by the last `resized()` (clamped).
    [[nodiscard]] VerticalScrollModel verticalScrollModel() const noexcept;

    /// Default (un-dragged) row height in px — the wheel scrolls by half of it per notch.
    [[nodiscard]] int defaultRowHeightPx() const noexcept { return defaultRowHeightPx_; }

    /// [Message thread] Scrollbar / external drive: same clamp and layout path as the wheel.
    void scrollVerticallyToOffsetPx(int offsetPx) noexcept;

    /// [Message thread] Fired (after layout) whenever content height, viewport height or offset
    /// changed — resize, wheel, row-height drag, track add / duplicate / delete / undo / project
    /// switch. Handlers must not call back into layout synchronously (sync a scrollbar with
    /// `dontSendNotification`).
    void setOnVerticalScrollModelChanged(std::function<void()> fn) noexcept;

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

    // [Message thread] Wired once by `Main`: header context menu "Duplicate Track" invokes this with
    // the clicked track id (explicit source, independent of the active track).
    void setOnDuplicateTrackRequested(std::function<void(TrackId)> onDuplicateTrackRequested) noexcept;

    /// Same handler as wired by `setOnDuplicateTrackRequested`; refused while structural edits are
    /// blocked (recording / count-in / playing / project load). Used by every header menu.
    void requestDuplicateTrackForHeaderMenu(TrackId tid) noexcept;

    /// The "Duplicate Track" item every header menu shows right after "Delete Track" (the item text
    /// states why it is unavailable while playing / recording / loading).
    [[nodiscard]] static juce::PopupMenu::Item makeDuplicateTrackMenuItem(int itemId, bool editLocked);

    // [Message thread] Wired once by `Main`: header context menu VST3 / editor / remove (optional).
    void setTrackHeaderPluginHost(TrackHeaderPluginHost host) noexcept;

    // [Message thread] Wired once by `Main`: committed clip move (real gesture only; see `ClipWaveformView`).
    void setOnUndoableClipMoveRequested(
        std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> fn) noexcept;

    void setOnUndoableClipTrimRequested(
        std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> fn) noexcept;

    /// [Message thread] Undoable clip display-name rename ("Rename clip"); metadata only.
    void setOnUndoableClipRenameRequested(
        std::function<bool(PlacedClipId, juce::String)> fn) noexcept;

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

    /// Input-monitoring hooks for audio-track headers (Monitor speaker cell). `isMonitored` feeds
    /// the model (orange while on); `toggleMonitor` flips runtime engine state (no undo entry,
    /// never persisted). Both unset ⇒ the Monitor cell is omitted entirely.
    void setInputMonitoringHooks(std::function<bool(TrackId)> isMonitored,
                                 std::function<void(TrackId)> toggleMonitor) noexcept;

    /// Solo hooks for audio / group / master headers (`SoloUiHooks`): display state feeds the S
    /// cell + locked-M rendering, `toggleSolo` forwards S clicks to the app's SoloCoordinator.
    /// Unwired ⇒ no S cell and unchanged Mute chrome. Instrument/Midi rows get the same hooks via
    /// `InstrumentTimelineRowCoordinator`.
    void setSoloUiHooks(SoloUiHooks hooks) noexcept;

    /// Optional: after an audio clip lane clears peer waveform selections on mouse-down, invoke this
    /// so MIDI clip selections can be cleared without threading instrument details into `ClipWaveformView`.
    void setOnAudioClipMouseDownClearForeignSelections(std::function<void()> fn) noexcept;

    /// [Message thread] Audio header context menu: import WAV/etc. at playhead onto this track.
    void setOnAudioTrackImportClipAtPlayhead(std::function<void(TrackId)> fn) noexcept;

    /// [Message thread] Replace all instrument-shell UI bridges (Groove-Agent rows). Omit a `TrackId` to detach it.
    void syncInstrumentTimelineAttachments(const std::vector<InstrumentTimelineAttachment>& rows) noexcept;

    /// [Message thread] Drop the attachment for one track **before** its header/lane components are
    /// destroyed (track delete). The attachment map stores raw pointers; detaching after destruction
    /// would call into freed components (UAF crash in `syncInstrumentTimelineAttachments`).
    void detachInstrumentTimelineRowForTrack(TrackId tid) noexcept;

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

    /// [Message thread] Row height drag (bottom-edge resize of ONE row; min = the shared Small
    /// preset, max unchanged). No undo step (existing row-height policy); heights are persisted
    /// per project (v26) via `allTrackRowHeightsPxForProjectSave` / `applyTrackRowHeightsFromLoadedProject`.
    void applyTrackRowHeightDelta(TrackId tid, int startHeightPx, int deltaPx) noexcept;

    // -------------------------------------------------------------------------------------------
    // Shared track heights Small / Medium / Large (`ui/TrackRowHeightPresets.h`).
    // -------------------------------------------------------------------------------------------

    /// [Message thread] One-shot preset command: sets EVERY arrangement row (all kinds, including
    /// Group and Stereo Out, including scrolled-out rows) to the preset height in ONE gathered
    /// layout pass, clears all individual overrides, records the preset as the default height for
    /// new tracks, and preserves the previously topmost visible track as far as clamping allows.
    void applyTrackRowHeightPreset(track_row_heights::TrackRowHeightPreset preset) noexcept;

    /// [Message thread] Dropdown status: the preset ALL rows' effective heights exactly match, or
    /// nullopt = "Custom" (mixed heights or a uniform non-preset height). Status only — derived
    /// from the actual heights, independent of `lastChosenTrackRowHeightPreset()`.
    [[nodiscard]] std::optional<track_row_heights::TrackRowHeightPreset>
    uniformTrackRowHeightPresetStatus() const noexcept;

    /// [Message thread] The last explicitly chosen preset (project default for NEW tracks even
    /// after individual drags). Loaded projects restore it; missing/unknown key = Medium.
    [[nodiscard]] track_row_heights::TrackRowHeightPreset lastChosenTrackRowHeightPreset() const noexcept
    {
        return lastChosenRowHeightPreset_;
    }

    /// [Message thread] Project save: every session row's ACTUAL effective height in px (clamped),
    /// in session order. Written per track (v26 `rowHeight`) together with the preset key.
    [[nodiscard]] std::vector<std::pair<TrackId, int>> allTrackRowHeightsPxForProjectSave() const;

    /// [Message thread] Project load: adopt the saved preset (absent/unknown → Medium) as default +
    /// new-track height, then the saved per-track heights clamped to [Small, max]. Rows without a
    /// saved height (older projects) use the default. Never marks the project dirty.
    void applyTrackRowHeightsFromLoadedProject(
        const juce::String& presetKey,
        const std::vector<std::pair<TrackId, int>>& perTrackPx) noexcept;

    /// [Message thread] Duplicate Track keeps the SOURCE row's height for the copy; the project
    /// default preset is never touched. Call before the post-duplicate `syncTracksFromSession`.
    void copyRowHeightForDuplicatedTrack(TrackId sourceTid, TrackId newTid) noexcept;

    /// [Message thread] Fired after any row-height change. `byUserEdit` = true for drags / preset
    /// commands (callers mark the project dirty), false for the project-load apply.
    void setOnTrackRowHeightsChanged(std::function<void(bool byUserEdit)> fn) noexcept
    {
        onTrackRowHeightsChanged_ = std::move(fn);
    }

    // -------------------------------------------------------------------------------------------
    // Visual track groups (collapsible, purely visual — `domain/VisualTrackGroup.h`) and the
    // header multi-selection used to create them.
    // -------------------------------------------------------------------------------------------

    /// Collapsed mini-view: each effective member of a collapsed displayable group shows as a
    /// strip of EXACTLY this many logical px (the Small 64 px minimum deliberately does not apply
    /// to the collapsed display; the stored normal height is untouched).
    static constexpr int kCollapsedGroupMemberRowHeightPx = 4;
    /// Group handle tab geometry (the "inverted golf club": the vertical member marker continues
    /// into this short tab extending right over the header area at the group's top boundary).
    static constexpr int kVisualGroupHandleHeightPx = 16;
    static constexpr int kVisualGroupHandleMaxWidthPx = 140;
    /// Long-press threshold on the handle: rename instead of collapse-toggle (spec §3).
    static constexpr int kVisualGroupHandleLongPressMs = 500;

    /// [Message thread] Wired once by `Main` (see `VisualTrackGroupUiHooks`).
    void setVisualTrackGroupUiHooks(VisualTrackGroupUiHooks hooks) noexcept;

    /// [Message thread] Re-read group state from `Session` (after create/rename/ungroup/collapse,
    /// undo/redo, or project load): rebuild the display cache + handle components, relayout, repaint.
    void refreshVisualTrackGroupsFromSession() noexcept;

    /// [Message thread] Header multi-selection click (plain = exactly this header + anchor here;
    /// `shiftRange` = contiguous visible range anchor … `tid`). Never changes the active track by
    /// itself and never touches clip selection.
    void handleHeaderSelectionClick(TrackId tid, bool shiftRange) noexcept;
    [[nodiscard]] bool isHeaderMultiSelected(TrackId tid) const noexcept;
    void clearHeaderMultiSelection() noexcept;
    /// Right-click policy (spec §2): inside the selection → keep it; outside → select clicked row.
    void applyHeaderRightClickSelectionPolicy(TrackId clickedTid) noexcept;
    /// Current multi-selection in VISIBLE row order (diagnostics + group creation).
    [[nodiscard]] std::vector<TrackId> selectedHeaderTrackIdsInVisibleOrder() const;

    /// True when the current multi-selection qualifies for "Create collapsible group…": ≥2
    /// existing tracks, adjacent in session order, none Stereo Out, none already grouped.
    [[nodiscard]] bool canCreateCollapsibleGroupFromCurrentSelection() const;
    /// Appends the "Skapa hopfällbar grupp…" item (disabled when the selection does not qualify).
    void appendCreateCollapsibleGroupMenuItem(juce::PopupMenu& menu, int itemId);
    /// Menu action: name prompt (sensible default; OK → `hooks.createGroup`). No-op when invalid.
    void requestCreateCollapsibleGroupFromSelection();

    /// True when `tid` is an effective member of a displayable group (expanded or collapsed) —
    /// feeds the header's left-edge group marker. / collapsed variant for layout and hit policy.
    [[nodiscard]] bool isTrackInDisplayableVisualGroup(TrackId tid) const noexcept;
    [[nodiscard]] bool isTrackInCollapsedVisualGroup(TrackId tid) const noexcept;

    /// [Test] Handle introspection + the exact mouse-equivalent actions (short click toggles,
    /// long press begins inline rename). False / empty when the group has no laid-out handle.
    [[nodiscard]] juce::Rectangle<int> visualGroupHandleBoundsForTest(int groupId) const noexcept;
    bool shortClickVisualGroupHandleLikeMouseForTest(int groupId);
    bool beginRenameOnVisualGroupHandleLikeLongPressForTest(int groupId);
    bool commitVisualGroupHandleRenameForTest(int groupId, const juce::String& newName);

    /// Optional arrangement timeline snapping (Slice D): used by clip lanes when committing/editing.
    void setArrangementTimelineSnapFunction(std::function<std::int64_t(std::int64_t)> fn) noexcept;

    /// [Message thread] After header bottom-edge resize: clamp/snap via the shared static rule.
    /// With the global minimum = the Small preset (>= every row kind's full name+buttons ideal)
    /// this passes heights through clamped; the old name-only collapse is no longer reachable.
    void snapTrackHeaderRowHeightAfterResize(TrackId tid, bool headerHasSubtitle) noexcept;

    /// [Message thread] Stability C3 introspection: trackIds of all instrument timeline
    /// attachments (each should map to a live Instrument session row). Diagnostics only.
    [[nodiscard]] std::vector<TrackId> exportInstrumentTimelineAttachmentTrackIdsForDiagnostics() const
    {
        std::vector<TrackId> out;
        out.reserve(instrumentTimelineAttachments_.size());
        for (const auto& [tid, att] : instrumentTimelineAttachments_)
        {
            juce::ignoreUnused(att);
            out.push_back(tid);
        }
        return out;
    }

    /// [Message thread] Stability C3: true when the header-drag source pointer survived past the
    /// drag (the stale-pointer class guarded by `cancelHeaderDragIfSourceIs`). Diagnostics only.
    [[nodiscard]] bool hasStaleHeaderDragSourceForDiagnostics() const noexcept
    {
        return !headerTrackDragActive_ && headerTrackDragSourceView_ != nullptr;
    }

private:
    void timerCallback() override;

    /// Vertical drag handle on the header/timeline boundary (see `kHeaderColumnResizeHandleWidthPx`).
    /// Left-button drag resizes the shared header column; other buttons fall through to the
    /// middle-pan listener. Paints a faint boundary highlight only while hovered or dragging.
    class HeaderColumnResizeHandle final : public juce::Component
    {
    public:
        explicit HeaderColumnResizeHandle(TrackLanesView& owner) noexcept;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void paint(juce::Graphics& g) override;

    private:
        TrackLanesView& owner_;
        int anchorWidthPx_ = 0;
        bool dragging_ = false;
    };
    void notifyTrackHeaderColumnWidthDragEnded() noexcept;

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
    /// Call before destroying any `TrackHeaderView` that could be the active drag source:
    /// `headerTrackDragSourceView_` is a raw pointer, so destroying the source mid-drag would
    /// otherwise leave a stale pointer and stuck drag-overlay state.
    void cancelHeaderDragIfSourceIs(const TrackHeaderView* header) noexcept;
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

    // ------------------------------------------------------ Visual track groups (private side)
    /// Group handle child component: the short tab at the group's top boundary. Owns the short-
    /// click (toggle), long-press (inline rename) and right-click (menu) gestures; swallows its
    /// mouse events so the row underneath (previous track / resize band) is never activated.
    class VisualGroupHandleView;

    /// One displayable group's current visible-row run (display cache; rebuilt with the entries).
    struct VisualGroupDisplayRun
    {
        int groupId = 0;
        juce::String name;
        bool collapsed = false;
        int firstVisibleIndex = -1;
        int lastVisibleIndex = -1;
    };

    /// Rebuild `visualGroupDisplayRuns_` + membership map from `Session` (displayable groups only).
    void rebuildVisualGroupDisplayCache();
    /// Ensure one handle component per displayable run (created/removed as groups change).
    void rebuildVisualGroupHandles();
    /// Position every handle for the current layout (called at the end of `resized()`).
    void layoutVisualGroupHandles() noexcept;
    /// Collapsed mini strips + collapsed-run header chrome (called from `paint()`).
    void paintCollapsedGroupContent(juce::Graphics& g) const;
    [[nodiscard]] const VisualGroupDisplayRun* findVisualGroupRun(int groupId) const noexcept;
    /// Top y (view coords) of visible row `vi` under the current scroll offset.
    [[nodiscard]] int yTopForVisibleIndex(int vi) const noexcept;
    /// True when the horizontal separator UNDER visible row `vi` must be skipped (both `vi` and
    /// `vi`+1 are strips of the SAME collapsed group — zero separator between member strips).
    [[nodiscard]] bool suppressSeparatorBelowVisibleIndex(int vi) const noexcept;
    void showVisualGroupHandleContextMenu(int groupId);
    void toggleVisualGroupCollapsedFromHandle(int groupId);
    void beginVisualGroupRenameFromHandle(int groupId);

    struct VisualGroupMembershipCacheEntry
    {
        int groupId = 0;
        bool collapsed = false;
    };
    std::unordered_map<TrackId, VisualGroupMembershipCacheEntry> visualGroupMembershipByTrackId_;
    std::vector<VisualGroupDisplayRun> visualGroupDisplayRuns_;
    std::unordered_map<int, std::unique_ptr<VisualGroupHandleView>> visualGroupHandles_;
    VisualTrackGroupUiHooks visualGroupUiHooks_{};

    /// Header multi-selection (visual-group creation): selected ids + the shift anchor. UI-only —
    /// never session state, never clip selection, never the active track.
    std::vector<TrackId> headerMultiSelection_;
    TrackId headerSelectionAnchorTid_ = kInvalidTrackId;

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

    /// Default (un-dragged) row height = the last chosen preset's px (Medium on a fresh session).
    int defaultRowHeightPx_ = track_row_heights::kMediumRowHeightPx;
    int maxRowHeightPx_ = 480;
    track_row_heights::TrackRowHeightPreset lastChosenRowHeightPreset_
        = track_row_heights::TrackRowHeightPreset::Medium;
    std::function<void(bool)> onTrackRowHeightsChanged_;
    void notifyTrackRowHeightsChanged(bool byUserEdit) noexcept;
    [[nodiscard]] TrackId topVisibleTrackIdForCurrentOffset() const noexcept;
    int verticalScrollOffsetPx_ = 0;
    VerticalScrollModel lastPublishedVerticalScrollModel_{};
    std::function<void()> onVerticalScrollModelChanged_;
    void publishVerticalScrollModelIfChanged() noexcept;

    int trackHeaderColumnWidthPx_ = kTrackHeaderColumnDefaultWidthPx;
    HeaderColumnResizeHandle headerColumnResizeHandle_{ *this };
    std::function<void(int, bool)> onTrackHeaderColumnWidthChanged_;

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
    std::function<void(TrackId)> onDuplicateTrackRequested_;
    std::function<bool(TrackId)> isTrackInputMonitoredFn_;
    std::function<void(TrackId)> toggleTrackInputMonitorFn_;
    SoloUiHooks soloUiHooks_{};
    std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> onUndoableClipMoveRequested_;
    std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> onUndoableClipTrimRequested_;
    std::function<bool(PlacedClipId, juce::String)> onUndoableClipRenameRequested_;
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
