#include "app/UndoRedoCoordinator.h"

#include <cstdint>
#include <optional>

#include "diagnostics/StabilityInvariants.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "io/ProjectFile.h"
#include "plugins/PluginEditorWindows.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/PluginTrackSlot.h"

namespace
{
    [[nodiscard]] juce::String undoDiagSnapPtr(const SessionSnapshot* p)
    {
        if (p == nullptr)
        {
            return "null";
        }
        return "0x" + juce::String::toHexString(reinterpret_cast<juce::pointer_sized_int>(p));
    }

} // namespace

void UndoRedoCoordinator::pluginUndoRecorderEntry(void* context, const juce::String& label, const PluginUndoStepSides& sides)
{
    static_cast<UndoRedoCoordinator*>(context)->onPluginUndoRecord(label, sides);
}

UndoRedoCoordinator::UndoRedoCoordinator(Session& session, PluginInsertHost& pluginHost, Callbacks callbacks)
    : session_(session)
    , pluginHost_(pluginHost)
    , callbacks_(std::move(callbacks))
{
    pluginHost_.setUndoRecorder(this, &UndoRedoCoordinator::pluginUndoRecorderEntry);
    pluginHost_.setEditorShortcutCallbacks(PluginEditorWindowHostShortcuts{
        [this] { invokeUndoFromWindowShortcut(); },
        [this] { invokeRedoFromWindowShortcut(); },
    });
}

UndoRedoCoordinator::~UndoRedoCoordinator()
{
    pluginHost_.setUndoRecorder(nullptr, nullptr);
    pluginHost_.setEditorShortcutCallbacks(PluginEditorWindowHostShortcuts{});
}

void UndoRedoCoordinator::onPluginUndoRecord(const juce::String& label, const PluginUndoStepSides& sides)
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    PluginUndoStepSides copy = sides;
    sessionHistory_.record(label, snap, snap, std::move(copy), std::nullopt);
    if (callbacks_.markProjectDirty)
    {
        callbacks_.markProjectDirty();
    }
}

void UndoRedoCoordinator::invokeUndoFromWindowShortcut()
{
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] invokeUndoFromWindowShortcut entered undoSize="
            + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
            + juce::String(sessionHistory_.redoStackSize()));
    }
    if (callbacks_.isRecording && callbacks_.isRecording())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeUndo bail: recordingOrCountIn recording="
                + juce::String(callbacks_.isRecording() ? "Y" : "n")
                + " countIn="
                + juce::String(callbacks_.isCountInActive && callbacks_.isCountInActive() ? "Y" : "n"));
        }
        return;
    }
    if (callbacks_.isCountInActive && callbacks_.isCountInActive())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeUndo bail: recordingOrCountIn recording="
                + juce::String(callbacks_.isRecording && callbacks_.isRecording() ? "Y" : "n")
                + " countIn="
                + juce::String(callbacks_.isCountInActive() ? "Y" : "n"));
        }
        return;
    }
    if (callbacks_.isClipEditGestureInProgress && callbacks_.isClipEditGestureInProgress())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] invokeUndo bail: gestureInProgress");
        }
        return;
    }
    pluginHost_.flushOpenEditorParameterUndoSteps();
    const std::optional<SessionHistoryRestoreBundle> bundle = sessionHistory_.popUndo();
    if (!bundle.has_value() || bundle->timelineSnapshot == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeUndo bail: emptyOrNullBundle hasValue="
                + juce::String(bundle.has_value() ? "Y" : "n") + " timelineNull="
                + juce::String(
                    (bundle.has_value() && bundle->timelineSnapshot == nullptr) ? "Y" : "n"));
        }
        return;
    }
    {
        const std::shared_ptr<const SessionSnapshot> live = session_.loadSessionSnapshotForAudioThread();
        const std::int64_t curL = live ? live->getLeftLocatorSamples() : 0;
        const std::int64_t curR = live ? live->getRightLocatorSamples() : 0;
        const std::shared_ptr<const SessionSnapshot> restoredWithLocators
            = SessionSnapshot::withLocators(*bundle->timelineSnapshot, curL, curR);
        session_.restoreSessionSnapshotForUndo(restoredWithLocators);
        // C2B: republish the routing plan against the restored snapshot *now* — the plugin/
        // instrument restore below can take seconds, and a stale plan across that window feeds
        // stale track indices to the audio callback.
        if (callbacks_.rebuildRoutingPlanFromSession)
        {
            callbacks_.rebuildRoutingPlanFromSession();
        }
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] invokeUndo restored timeline="
                                       + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " liveBeforeRestore="
                                       + undoDiagSnapPtr(live.get()));
        }
    }
    if (bundle->pluginSides.has_value())
    {
        pluginHost_.importChain(bundle->pluginSides->trackId, bundle->pluginSides->before);
    }
    if (bundle->instrumentSides.has_value())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            if (callbacks_.logInstrumentMusicalUndoPreApplyDiag)
            {
                callbacks_.logInstrumentMusicalUndoPreApplyDiag(false);
            }
        }
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& mus
            = bundle->isRedo ? bundle->instrumentSides->after : bundle->instrumentSides->before;
        if (callbacks_.applyInstrumentMusicalUndoVectorToAllKeyedAndStaging)
        {
            callbacks_.applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(mus);
        }
        if (callbacks_.rebindMidiEditorAfterInstrumentMusicalUndo)
        {
            callbacks_.rebindMidiEditorAfterInstrumentMusicalUndo();
        }
    }
    if (bundle->instrumentTrackDelete.has_value() && callbacks_.restoreDeletedInstrumentTrackRuntime)
    {
        // Undo of Delete Track: the timeline snapshot restored the session row; now recreate the
        // instrument runtime from the captured project row (same restore path as project load).
        callbacks_.restoreDeletedInstrumentTrackRuntime(*bundle->instrumentTrackDelete);
    }
    refreshAfterSessionSnapshotRestore();
    // Stability C3: verify runtime invariants right after the undo completed.
    (void) stability_invariants::runRegisteredStabilityInvariantsCheck("undo-end");
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        const auto liveNow = session_.loadSessionSnapshotForAudioThread();
        writeUndoDiagnosticLogLine(
            "[UndoDiag] invokeUndo complete liveNow=" + undoDiagSnapPtr(liveNow.get()) + " undoSize="
            + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
            + juce::String(sessionHistory_.redoStackSize()));
    }
}

void UndoRedoCoordinator::invokeRedoFromWindowShortcut()
{
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] invokeRedoFromWindowShortcut entered undoSize="
            + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
            + juce::String(sessionHistory_.redoStackSize()));
    }
    if (callbacks_.isRecording && callbacks_.isRecording())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeRedo bail: recordingOrCountIn recording="
                + juce::String(callbacks_.isRecording() ? "Y" : "n")
                + " countIn="
                + juce::String(callbacks_.isCountInActive && callbacks_.isCountInActive() ? "Y" : "n"));
        }
        return;
    }
    if (callbacks_.isCountInActive && callbacks_.isCountInActive())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeRedo bail: recordingOrCountIn recording="
                + juce::String(callbacks_.isRecording && callbacks_.isRecording() ? "Y" : "n")
                + " countIn=" + juce::String(callbacks_.isCountInActive() ? "Y" : "n"));
        }
        return;
    }
    if (callbacks_.isClipEditGestureInProgress && callbacks_.isClipEditGestureInProgress())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] invokeRedo bail: gestureInProgress");
        }
        return;
    }
    const std::optional<SessionHistoryRestoreBundle> bundle = sessionHistory_.popRedo();
    if (!bundle.has_value() || bundle->timelineSnapshot == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeRedo bail: emptyOrNullBundle hasValue="
                + juce::String(bundle.has_value() ? "Y" : "n") + " timelineNull="
                + juce::String(
                    (bundle.has_value() && bundle->timelineSnapshot == nullptr) ? "Y" : "n"));
        }
        return;
    }
    {
        const std::shared_ptr<const SessionSnapshot> live = session_.loadSessionSnapshotForAudioThread();
        const std::int64_t curL = live ? live->getLeftLocatorSamples() : 0;
        const std::int64_t curR = live ? live->getRightLocatorSamples() : 0;
        const std::shared_ptr<const SessionSnapshot> restoredWithLocators
            = SessionSnapshot::withLocators(*bundle->timelineSnapshot, curL, curR);
        session_.restoreSessionSnapshotForUndo(restoredWithLocators);
        // C2B: same immediate plan republish as the undo path (see invokeUndo).
        if (callbacks_.rebuildRoutingPlanFromSession)
        {
            callbacks_.rebuildRoutingPlanFromSession();
        }
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] invokeRedo restored timeline="
                                       + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " liveBeforeRestore="
                                       + undoDiagSnapPtr(live.get()));
        }
    }
    if (bundle->pluginSides.has_value())
    {
        pluginHost_.importChain(bundle->pluginSides->trackId, bundle->pluginSides->after);
    }
    if (bundle->instrumentSides.has_value())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            if (callbacks_.logInstrumentMusicalUndoPreApplyDiag)
            {
                callbacks_.logInstrumentMusicalUndoPreApplyDiag(true);
            }
        }
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& mus
            = bundle->isRedo ? bundle->instrumentSides->after : bundle->instrumentSides->before;
        if (callbacks_.applyInstrumentMusicalUndoVectorToAllKeyedAndStaging)
        {
            callbacks_.applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(mus);
        }
        if (callbacks_.rebindMidiEditorAfterInstrumentMusicalUndo)
        {
            callbacks_.rebindMidiEditorAfterInstrumentMusicalUndo();
        }
    }
    if (bundle->instrumentTrackDelete.has_value()
        && callbacks_.teardownDeletedInstrumentTrackRuntimeForRedo)
    {
        // Redo of Delete Track: the timeline snapshot removed the row again (and pluginSides->after
        // evicted inserts); retire the recreated instrument runtime with the hardened teardown.
        callbacks_.teardownDeletedInstrumentTrackRuntimeForRedo(bundle->instrumentTrackDelete->trackId);
    }
    refreshAfterSessionSnapshotRestore();
    // Stability C3: verify runtime invariants right after the redo completed.
    (void) stability_invariants::runRegisteredStabilityInvariantsCheck("redo-end");
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        const auto liveNow = session_.loadSessionSnapshotForAudioThread();
        writeUndoDiagnosticLogLine(
            "[UndoDiag] invokeRedo complete liveNow=" + undoDiagSnapPtr(liveNow.get()) + " undoSize="
            + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
            + juce::String(sessionHistory_.redoStackSize()));
    }
}

void UndoRedoCoordinator::refreshAfterSessionSnapshotRestore()
{
    // Undo/redo moved the project away from its last saved state (possibly instrument-side only).
    if (callbacks_.markProjectDirty)
    {
        callbacks_.markProjectDirty();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] refreshAfterSessionSnapshotRestore: cancel UI for snapshot restore");
    }
    if (callbacks_.cancelAllClipGesturesAndTransientUiState)
    {
        callbacks_.cancelAllClipGesturesAndTransientUiState();
    }
    if (callbacks_.reconcileCycleBookingAfterUndoSnapshotRestore)
    {
        callbacks_.reconcileCycleBookingAfterUndoSnapshotRestore();
    }
    if (callbacks_.alignInstrumentClipTemposToProjectTempo)
    {
        callbacks_.alignInstrumentClipTemposToProjectTempo();
    }
    if (callbacks_.syncViewportFromSession)
    {
        callbacks_.syncViewportFromSession();
    }
    if (callbacks_.syncTracksFromSession)
    {
        callbacks_.syncTracksFromSession();
    }
    if (callbacks_.repaintRuler)
    {
        callbacks_.repaintRuler();
    }
    if (callbacks_.repaintLanes)
    {
        callbacks_.repaintLanes();
    }
    if (callbacks_.refreshInstrumentUi)
    {
        callbacks_.refreshInstrumentUi();
    }
    if (callbacks_.refreshInspectorFromSession)
    {
        callbacks_.refreshInspectorFromSession();
    }
    if (callbacks_.refreshArrangementMusicalToolbarFromSession)
    {
        callbacks_.refreshArrangementMusicalToolbarFromSession();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] refreshAfterSessionSnapshotRestore complete");
    }
}

void UndoRedoCoordinator::executeUndoableSessionEdit(const juce::String& label, std::function<bool()> mutator)
{
    std::shared_ptr<const SessionSnapshot> before = session_.loadSessionSnapshotForAudioThread();
    if (before == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit skip: null before label=\""
                                       + label + "\"");
        }
        return;
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit mutator run label=\"" + label
                                   + "\" before=" + undoDiagSnapPtr(before.get()) + " undoSize="
                                   + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                                   + juce::String(sessionHistory_.redoStackSize()));
    }
    if (!mutator())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] executeUndoableSessionEdit mutator=false label=\"" + label + "\"");
        }
        return;
    }
    std::shared_ptr<const SessionSnapshot> after = session_.loadSessionSnapshotForAudioThread();
    if (after == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit skip: null after label=\""
                                       + label + "\"");
        }
        return;
    }
    const SessionSnapshot* const afterPtrForDiag = after.get();
    sessionHistory_.record(label, std::move(before), std::move(after));
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit after record label=\"" + label
                                   + "\" after=" + undoDiagSnapPtr(afterPtrForDiag) + " undoSize="
                                   + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                                   + juce::String(sessionHistory_.redoStackSize()));
    }
}

void UndoRedoCoordinator::executeUndoableTrackDelete(
    const juce::String& label,
    std::function<bool(std::optional<PluginUndoStepSides>& outPluginSides,
                       std::optional<InstrumentTrackDeleteUndoSides>& outInstrumentDelete)> mutator)
{
    std::shared_ptr<const SessionSnapshot> before = session_.loadSessionSnapshotForAudioThread();
    if (before == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableTrackDelete skip: null before label=\""
                                       + label + "\"");
        }
        return;
    }
    std::optional<PluginUndoStepSides> pluginSides;
    std::optional<InstrumentTrackDeleteUndoSides> instrumentDelete;
    if (!mutator(pluginSides, instrumentDelete))
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableTrackDelete mutator=false label=\""
                                       + label + "\"");
        }
        return;
    }
    std::shared_ptr<const SessionSnapshot> after = session_.loadSessionSnapshotForAudioThread();
    if (after == nullptr || after.get() == before.get())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] executeUndoableTrackDelete skip: null/unchanged after label=\"" + label
                + "\"");
        }
        return;
    }
    sessionHistory_.record(label,
                           std::move(before),
                           std::move(after),
                           std::move(pluginSides),
                           std::nullopt,
                           std::move(instrumentDelete));
    if (callbacks_.markProjectDirty)
    {
        callbacks_.markProjectDirty();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableTrackDelete recorded label=\"" + label
                                   + "\" undoSize=" + juce::String(sessionHistory_.undoStackSize())
                                   + " redoSize=" + juce::String(sessionHistory_.redoStackSize()));
    }
}

void UndoRedoCoordinator::executeUndoableInstrumentEdit(const juce::String& label,
                                                       std::function<bool()> mutator)
{
    std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] executeUndoableInstrumentEdit skip: null snap label=\"" + label + "\"");
        }
        return;
    }
    std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical;
    if (callbacks_.buildSortedInstrumentMusicalUndoSnapshot)
    {
        beforeMusical = callbacks_.buildSortedInstrumentMusicalUndoSnapshot();
    }
    if (callbacks_.stableSortInstrumentMusicalUndoVector)
    {
        callbacks_.stableSortInstrumentMusicalUndoVector(beforeMusical);
    }
    if (beforeMusical.empty())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] executeUndoableInstrumentEdit skip: empty before label=\"" + label + "\"");
        }
        return;
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit mutator run label=\"" + label
                                   + "\"");
    }
    if (!mutator())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] executeUndoableInstrumentEdit mutator=false label=\"" + label + "\"");
        }
        return;
    }
    std::vector<ProjectFileExperimentalInstrumentTrackV1> afterMusical;
    if (callbacks_.buildSortedInstrumentMusicalUndoSnapshot)
    {
        afterMusical = callbacks_.buildSortedInstrumentMusicalUndoSnapshot();
    }
    if (callbacks_.stableSortInstrumentMusicalUndoVector)
    {
        callbacks_.stableSortInstrumentMusicalUndoVector(afterMusical);
    }
    if (afterMusical.empty())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit skip: empty after label=\""
                                       + label + "\"");
        }
        return;
    }
    if (experimentalInstrumentTracksMusicalUndoEqual(beforeMusical, afterMusical))
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] executeUndoableInstrumentEdit skip: musicalEqual label=\"" + label + "\"");
        }
        return;
    }
    sessionHistory_.record(label,
                           snap,
                           snap,
                           std::nullopt,
                           InstrumentUndoStepSides{ std::move(beforeMusical), std::move(afterMusical) });
    if (callbacks_.markProjectDirty)
    {
        callbacks_.markProjectDirty();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit recorded label=\"" + label
                                   + "\" undoSize=" + juce::String(sessionHistory_.undoStackSize())
                                   + " redoSize=" + juce::String(sessionHistory_.redoStackSize()));
    }
}

void UndoRedoCoordinator::clearHistory() noexcept
{
    sessionHistory_.clear();
}
