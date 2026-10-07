#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "domain/SessionHistory.h"
#include "io/ProjectFile.h"

class Session;
class PluginInsertHost;
struct PluginUndoStepSides;

/// Owns `SessionHistory` and message-thread undo/redo orchestration (shortcuts, record steps, restore).
class UndoRedoCoordinator final
{
public:
    struct Callbacks
    {
        std::function<bool()> isRecording;
        std::function<bool()> isCountInActive;
        std::function<bool()> isClipEditGestureInProgress;

        /// Stability C2B: rebuild the engine routing plan immediately after the timeline snapshot is
        /// restored (before slow plugin/instrument restore work), so the audio callback never runs a
        /// long window with a fresh snapshot and a stale plan (stale `trackIndex` values).
        std::function<void()> rebuildRoutingPlanFromSession;

        std::function<void()> cancelAllClipGesturesAndTransientUiState;
        std::function<void()> reconcileCycleBookingAfterUndoSnapshotRestore;
        std::function<void()> syncViewportFromSession;
        std::function<void()> syncTracksFromSession;
        std::function<void()> repaintRuler;
        std::function<void()> repaintLanes;
        std::function<void()> refreshInstrumentUi;
        std::function<void()> refreshInspectorFromSession;

        /// Sync main-toolbar project tempo/meter widgets after undo/redo timeline snapshot restore.
        std::function<void()> refreshArrangementMusicalToolbarFromSession;

        /// Build/sort musical undo blocks for instrument tracks (delegates to host implementation).
        std::function<std::vector<ProjectFileExperimentalInstrumentTrackV1>()>
            buildSortedInstrumentMusicalUndoSnapshot;
        std::function<void(std::vector<ProjectFileExperimentalInstrumentTrackV1>&)>
            stableSortInstrumentMusicalUndoVector;

        std::function<void(const std::vector<ProjectFileExperimentalInstrumentTrackV1>&)>
            applyInstrumentMusicalUndoVectorToAllKeyedAndStaging;

        std::function<void()> rebindMidiEditorAfterInstrumentMusicalUndo;

        /// Invoked synchronously after every applied instrument edit (before this call returns to the
        /// message loop). Lets the MIDI editor detach if the edit removed/moved the clip it was bound
        /// to, so the controller's later async change message cannot dereference a freed clip.
        std::function<void()> reconcileMidiEditorAfterInstrumentEdit;

        /// When `undo_diagnostic::kUndoDiag` is enabled, logs pre-apply instrument bundle context.
        std::function<void(bool isRedoStep)> logInstrumentMusicalUndoPreApplyDiag;

        /// Clips always play at the project tempo: re-align clip bpm after a snapshot restore may have
        /// changed the project BPM (undo/redo of "Project BPM" edits).
        std::function<void()> alignInstrumentClipTemposToProjectTempo;

        /// Stability Slice 5: invoked after every recorded edit and after undo/redo application, so
        /// instrument/plugin edits (which do not swap the session snapshot) mark the project dirty.
        std::function<void()> markProjectDirty;

        /// Delete-Track undo: recreate the instrument runtime (host + controller + plugin) for the
        /// restored session row from the captured project row, using the same restore path as
        /// project load. Invoked after the timeline snapshot (and insert chain) are applied.
        std::function<void(const InstrumentTrackDeleteUndoSides&)> restoreDeletedInstrumentTrackRuntime;
        /// Delete-Track redo: re-run the hardened instrument runtime teardown (editors, timeline UI,
        /// publish-before-destroy retire) after the timeline snapshot removed the row again.
        std::function<void(TrackId)> teardownDeletedInstrumentTrackRuntimeForRedo;
        /// Optional: true while a staged project load owns the session — every recorded edit and
        /// undo / redo is refused (the partially applied model must never become an undo step).
        std::function<bool()> isProjectLoadInProgress;

        /// Solo: invoked after undo/redo applied a narrow solo-memory step (the memory content in
        /// `Session` changed). The app re-derives and republishes the engine's `SoloMuteView` and
        /// refreshes the S buttons / memory strip — needed when the restored memory is the
        /// currently active solo set.
        std::function<void()> refreshSoloStateAfterUndoRestore;

        /// Visual track groups: invoked after undo/redo applied a narrow group-metadata step
        /// (the full group list in `Session` was replaced). The app relayouts the arrangement
        /// (markers, handles, collapsed rows) — no audio-side state is involved.
        /// NOTE: callers use positional aggregate init — keep the order.
        std::function<void()> refreshVisualTrackGroupsAfterUndoRestore;

        /// Track colours: invoked after a recorded colour edit and after undo/redo applied a narrow
        /// colour step (one track's palette key in `Session` changed). The app repaints headers and
        /// lanes (cached event rasters re-derive their fill) — no audio-side state is involved.
        /// NOTE: appended LAST on purpose — callers use positional aggregate init.
        std::function<void()> refreshTrackColoursAfterUndoRestore;
    };

    UndoRedoCoordinator(Session& session, PluginInsertHost& pluginHost, Callbacks callbacks);

    ~UndoRedoCoordinator();

    UndoRedoCoordinator(const UndoRedoCoordinator&) = delete;
    UndoRedoCoordinator& operator=(const UndoRedoCoordinator&) = delete;

    void invokeUndoFromWindowShortcut();
    void invokeRedoFromWindowShortcut();

    void executeUndoableSessionEdit(const juce::String& label, std::function<bool()> mutator);
    void executeUndoableInstrumentEdit(const juce::String& label, std::function<bool()> mutator);

    /// Solo (spec §7): ONE narrow undo step for a content change of Solo memory `memoryIndex`
    /// (an S click while that memory is active, applied by `mutator`). Captures the memory's
    /// TrackId set before/after around the mutator and records a step that carries ONLY that
    /// delta (same timeline snapshot pointer on both sides): undoing it can never revert clips,
    /// tracks, or a committed recording take. Nothing is recorded when the mutator returns false
    /// or the set is unchanged. Marks the project dirty when a step was recorded. Temporary-set
    /// edits and memory selection switches must NOT go through here (not undoable, not dirty).
    void executeUndoableSoloMemoryEdit(const juce::String& label,
                                       int memoryIndex,
                                       std::function<bool()> mutator);

    /// Visual track groups (groups spec §7): ONE narrow undo step for a bounded group-metadata
    /// edit (Create / Rename / Ungroup, applied by `mutator`). Captures the FULL group list
    /// before/after around the mutator and records a step that carries ONLY that delta (same
    /// timeline snapshot pointer on both sides): undoing it can never revert clips, tracks, or a
    /// committed recording take. Nothing is recorded when the mutator returns false or the list
    /// is unchanged. Marks the project dirty when a step was recorded. Collapse/expand must NOT
    /// go through here (display change: dirty only, no undo entry).
    void executeUndoableVisualTrackGroupsEdit(const juce::String& label,
                                              std::function<bool()> mutator);

    /// Track colour: ONE narrow undo step that changes exactly `trackId`'s palette key (same
    /// timeline snapshot pointer on both sides — never an instrument, a take or the whole
    /// Session). Nothing is recorded (and nothing dirtied) when the colour is unchanged or the
    /// track is unknown. Refused while a staged project load runs.
    void executeUndoableTrackColourEdit(TrackId trackId, TrackColourKey newColour);

    /// Recording commit: ONE undo step that may change both the timeline (an audio take clip)
    /// and instrument musical state (live-MIDI take clips on several rows). Records whichever
    /// side(s) actually changed; nothing is recorded when the mutator changed nothing (an empty
    /// take leaves no undo step). Marks the project dirty when a step was recorded.
    void executeUndoableRecordingCommit(const juce::String& label, std::function<void()> mutator);

    /// Delete Track: like `executeUndoableSessionEdit`, but the mutator may also hand back the
    /// pre-teardown insert chain (`outPluginSides`) and, for instrument tracks, the full captured
    /// project row (`outInstrumentDelete`) so undo can restore inserts and the instrument runtime.
    void executeUndoableTrackDelete(
        const juce::String& label,
        std::function<bool(std::optional<PluginUndoStepSides>& outPluginSides,
                           std::optional<InstrumentTrackDeleteUndoSides>& outInstrumentDelete)> mutator);

    void clearHistory() noexcept;
    /// [Diagnostics / stability] Current undo stack depth.
    [[nodiscard]] int undoStackSizeForDiagnostics() const noexcept { return sessionHistory_.undoStackSize(); }

private:
    void refreshAfterSessionSnapshotRestore();
    /// Instrument-runtime side of a structural step (Delete / Duplicate Track): recreate when the
    /// restored timeline contains the row, retire when it does not.
    void applyInstrumentTrackRuntimeSides(const InstrumentTrackDeleteUndoSides& sides,
                                          const SessionSnapshot& restoredTimeline);
    void onPluginUndoRecord(const juce::String& label, const PluginUndoStepSides& sides);

    /// C callback for `PluginInsertHost::setUndoRecorder` (must be a plain function pointer).
    static void pluginUndoRecorderEntry(void* context, const juce::String& label, const PluginUndoStepSides& sides);

    Session& session_;
    PluginInsertHost& pluginHost_;
    Callbacks callbacks_;
    SessionHistory sessionHistory_;
};
