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
    };

    UndoRedoCoordinator(Session& session, PluginInsertHost& pluginHost, Callbacks callbacks);

    ~UndoRedoCoordinator();

    UndoRedoCoordinator(const UndoRedoCoordinator&) = delete;
    UndoRedoCoordinator& operator=(const UndoRedoCoordinator&) = delete;

    void invokeUndoFromWindowShortcut();
    void invokeRedoFromWindowShortcut();

    void executeUndoableSessionEdit(const juce::String& label, std::function<bool()> mutator);
    void executeUndoableInstrumentEdit(const juce::String& label, std::function<bool()> mutator);

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
