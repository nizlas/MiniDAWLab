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

        /// When `undo_diagnostic::kUndoDiag` is enabled, logs pre-apply instrument bundle context.
        std::function<void(bool isRedoStep)> logInstrumentMusicalUndoPreApplyDiag;
    };

    UndoRedoCoordinator(Session& session, PluginInsertHost& pluginHost, Callbacks callbacks);

    ~UndoRedoCoordinator();

    UndoRedoCoordinator(const UndoRedoCoordinator&) = delete;
    UndoRedoCoordinator& operator=(const UndoRedoCoordinator&) = delete;

    void invokeUndoFromWindowShortcut();
    void invokeRedoFromWindowShortcut();

    void executeUndoableSessionEdit(const juce::String& label, std::function<bool()> mutator);
    void executeUndoableInstrumentEdit(const juce::String& label, std::function<bool()> mutator);
    void commitInstrumentMusicalUndoPair(const juce::String& label,
                                         std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical);

    void clearHistory() noexcept;

private:
    void refreshAfterSessionSnapshotRestore();
    void onPluginUndoRecord(const juce::String& label, const PluginUndoStepSides& sides);

    /// C callback for `PluginInsertHost::setUndoRecorder` (must be a plain function pointer).
    static void pluginUndoRecorderEntry(void* context, const juce::String& label, const PluginUndoStepSides& sides);

    Session& session_;
    PluginInsertHost& pluginHost_;
    Callbacks callbacks_;
    SessionHistory sessionHistory_;
};
