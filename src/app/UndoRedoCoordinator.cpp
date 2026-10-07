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
    if (callbacks_.isProjectLoadInProgress && callbacks_.isProjectLoadInProgress())
    {
        return; // a staged project load owns the session until it finalizes
    }
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
            const auto liveAfter = session_.loadSessionSnapshotForAudioThread();
            writeUndoDiagnosticLogLine("[UndoDiag] invokeUndo restored timeline="
                                       + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " (tracks="
                                       + juce::String(bundle->timelineSnapshot->getNumTracks())
                                       + ") liveBeforeRestore=" + undoDiagSnapPtr(live.get())
                                       + " (tracks=" + juce::String(live ? live->getNumTracks() : -1)
                                       + ") liveAfterRestore tracks="
                                       + juce::String(liveAfter ? liveAfter->getNumTracks() : -1));
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
    if (bundle->instrumentTrackDelete.has_value())
    {
        applyInstrumentTrackRuntimeSides(*bundle->instrumentTrackDelete, *bundle->timelineSnapshot);
    }
    if (bundle->soloMemorySides.has_value())
    {
        // Narrow solo-memory step: write the recorded side back into exactly that memory
        // (regardless of which memory is active now), then let the app republish the derived view.
        session_.setSoloMemoryTrackIds(bundle->soloMemorySides->memoryIndex,
                                       bundle->soloMemorySides->before);
        if (callbacks_.refreshSoloStateAfterUndoRestore)
        {
            callbacks_.refreshSoloStateAfterUndoRestore();
        }
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
    if (callbacks_.isProjectLoadInProgress && callbacks_.isProjectLoadInProgress())
    {
        return; // a staged project load owns the session until it finalizes
    }
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
    if (bundle->instrumentTrackDelete.has_value())
    {
        applyInstrumentTrackRuntimeSides(*bundle->instrumentTrackDelete, *bundle->timelineSnapshot);
    }
    if (bundle->soloMemorySides.has_value())
    {
        session_.setSoloMemoryTrackIds(bundle->soloMemorySides->memoryIndex,
                                       bundle->soloMemorySides->after);
        if (callbacks_.refreshSoloStateAfterUndoRestore)
        {
            callbacks_.refreshSoloStateAfterUndoRestore();
        }
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

void UndoRedoCoordinator::applyInstrumentTrackRuntimeSides(const InstrumentTrackDeleteUndoSides& sides,
                                                           const SessionSnapshot& restoredTimeline)
{
    // One rule for Delete Track AND Duplicate Track steps: the restored timeline decides. When the
    // row exists in it, the instrument runtime is recreated from the captured project row (undo of
    // a delete, redo of a duplicate); when it does not, the recreated runtime is retired with the
    // hardened teardown (redo of a delete, undo of a duplicate). The insert chain was already
    // imported / evicted by `pluginSides` before this call.
    const bool rowPresent = restoredTimeline.findTrackIndexById(sides.trackId) >= 0;
    if (rowPresent)
    {
        if (callbacks_.restoreDeletedInstrumentTrackRuntime)
        {
            callbacks_.restoreDeletedInstrumentTrackRuntime(sides);
        }
    }
    else if (callbacks_.teardownDeletedInstrumentTrackRuntimeForRedo)
    {
        callbacks_.teardownDeletedInstrumentTrackRuntimeForRedo(sides.trackId);
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
    if (callbacks_.isProjectLoadInProgress && callbacks_.isProjectLoadInProgress())
    {
        return; // a staged project load owns the session until it finalizes
    }
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
    if (callbacks_.isProjectLoadInProgress && callbacks_.isProjectLoadInProgress())
    {
        return; // a staged project load owns the session until it finalizes
    }
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

void UndoRedoCoordinator::executeUndoableRecordingCommit(const juce::String& label,
                                                        std::function<void()> mutator)
{
    std::shared_ptr<const SessionSnapshot> before = session_.loadSessionSnapshotForAudioThread();
    std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical;
    if (callbacks_.buildSortedInstrumentMusicalUndoSnapshot)
    {
        beforeMusical = callbacks_.buildSortedInstrumentMusicalUndoSnapshot();
    }
    if (callbacks_.stableSortInstrumentMusicalUndoVector)
    {
        callbacks_.stableSortInstrumentMusicalUndoVector(beforeMusical);
    }

    if (mutator)
    {
        mutator();
    }
    if (callbacks_.reconcileMidiEditorAfterInstrumentEdit)
    {
        callbacks_.reconcileMidiEditorAfterInstrumentEdit();
    }

    std::shared_ptr<const SessionSnapshot> after = session_.loadSessionSnapshotForAudioThread();
    std::vector<ProjectFileExperimentalInstrumentTrackV1> afterMusical;
    if (callbacks_.buildSortedInstrumentMusicalUndoSnapshot)
    {
        afterMusical = callbacks_.buildSortedInstrumentMusicalUndoSnapshot();
    }
    if (callbacks_.stableSortInstrumentMusicalUndoVector)
    {
        callbacks_.stableSortInstrumentMusicalUndoVector(afterMusical);
    }

    const bool sessionChanged = before != nullptr && after != nullptr && before.get() != after.get();
    const bool musicalChanged = !beforeMusical.empty() && !afterMusical.empty()
                                && !experimentalInstrumentTracksMusicalUndoEqual(beforeMusical, afterMusical);
    if (!sessionChanged && !musicalChanged)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableRecordingCommit skip: nothing changed label=\""
                                       + label + "\"");
        }
        return;
    }
    if (before == nullptr || after == nullptr)
    {
        return;
    }
    std::optional<InstrumentUndoStepSides> instrumentSides;
    if (musicalChanged)
    {
        instrumentSides = InstrumentUndoStepSides{ std::move(beforeMusical), std::move(afterMusical) };
    }
    sessionHistory_.record(label, std::move(before), std::move(after), std::nullopt, std::move(instrumentSides));
    if (callbacks_.markProjectDirty)
    {
        callbacks_.markProjectDirty();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableRecordingCommit recorded label=\"" + label
                                   + "\" session=" + juce::String(sessionChanged ? "Y" : "n")
                                   + " musical=" + juce::String(musicalChanged ? "Y" : "n")
                                   + " undoSize=" + juce::String(sessionHistory_.undoStackSize()));
    }
}

void UndoRedoCoordinator::executeUndoableInstrumentEdit(const juce::String& label,
                                                       std::function<bool()> mutator)
{
    if (callbacks_.isProjectLoadInProgress && callbacks_.isProjectLoadInProgress())
    {
        return; // a staged project load owns the session until it finalizes
    }
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
    // The clips vector has now been mutated. If that removed/moved the clip the MIDI editor is bound
    // to, detach it here — synchronously, before the controller's queued change message runs — so no
    // later callback dereferences a freed clip (the cross-track move use-after-free).
    if (callbacks_.reconcileMidiEditorAfterInstrumentEdit)
    {
        callbacks_.reconcileMidiEditorAfterInstrumentEdit();
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

void UndoRedoCoordinator::executeUndoableSoloMemoryEdit(const juce::String& label,
                                                        const int memoryIndex,
                                                        std::function<bool()> mutator)
{
    if (callbacks_.isProjectLoadInProgress && callbacks_.isProjectLoadInProgress())
    {
        return; // a staged project load owns the session until it finalizes
    }
    if (memoryIndex < 0 || memoryIndex >= Session::kSoloMemoryCount)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    std::vector<TrackId> before = session_.getSoloMemoryTrackIds(memoryIndex);
    if (!mutator())
    {
        return;
    }
    std::vector<TrackId> after = session_.getSoloMemoryTrackIds(memoryIndex);
    if (before == after)
    {
        return;
    }
    sessionHistory_.record(label,
                           snap,
                           snap,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           SoloMemoryUndoSides{ memoryIndex, std::move(before), std::move(after) });
    if (callbacks_.markProjectDirty)
    {
        callbacks_.markProjectDirty();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSoloMemoryEdit recorded label=\"" + label
                                   + "\" memory=" + juce::String(memoryIndex) + " undoSize="
                                   + juce::String(sessionHistory_.undoStackSize()));
    }
}

void UndoRedoCoordinator::clearHistory() noexcept
{
    sessionHistory_.clear();
}
