#include "app/TrackLanesEditCoordinator.h"

#include <memory>
#include <optional>
#include <utility>

#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"
#include "domain/Session.h"
#include "domain/SessionRouting.h"
#include "engine/PlaybackEngine.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

namespace
{
    void afterSessionRoutingPresentationChanged(TrackLanesEditCoordinator::Callbacks& callbacks,
                                                TrackLanesView& trackLanesView,
                                                TimelineRulerView& rulerView,
                                                InspectorView& inspectorView) noexcept
    {
        if (callbacks.syncViewportFromSession)
        {
            callbacks.syncViewportFromSession();
        }
        trackLanesView.syncTracksFromSession();
        rulerView.repaint();
        trackLanesView.repaint();
        inspectorView.refreshFromSession();
    }

    [[nodiscard]] bool applySendDestinationAtUiSlot(Session& session,
                                                    const TrackId trackId,
                                                    const int uiSlotIndex,
                                                    const TrackId destTrackId) noexcept
    {
        if (uiSlotIndex < 0 || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
        {
            return false;
        }
        const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return false;
        }
        const int tix = snap->findTrackIndexById(trackId);
        if (tix < 0)
        {
            return false;
        }
        const int sendIndex
            = findTrackSendVectorIndexForUiSlot(snap->getTrack(tix).getSends(), uiSlotIndex);

        if (destTrackId == kInvalidTrackId)
        {
            if (sendIndex < 0)
            {
                return false;
            }
            return session.removeTrackSend(trackId, uiSlotIndex);
        }
        if (sendIndex >= 0)
        {
            return session.setTrackSendDestination(trackId, uiSlotIndex, destTrackId);
        }
        return session.insertTrackSend(trackId, uiSlotIndex, destTrackId, kSendAmountUnityLinear);
    }

    /// RAII realtime silence gate for the track-delete transaction (Stability Slice 3). Reuses the
    /// Slice 2 offline-render gate: while held, the device callback outputs silence and touches no
    /// plugin hosts / instrument runtimes / scratch buffers; the ctor drains any in-flight callback.
    /// Playback intent is untouched — audio resumes when the gate destructs (all return paths).
    class ScopedTrackDeleteRealtimeGate final
    {
    public:
        explicit ScopedTrackDeleteRealtimeGate(PlaybackEngine& engine) noexcept
            : engine_(engine)
        {
            const bool outermost = engine_.beginOfflineRenderGate();
            double waitedMs = 0.0;
            const bool drained = engine_.waitForAudioCallbackExit(250.0, &waitedMs);
            if (!drained)
            {
                // Stability C2B: identify where the callback is stuck when the drain times out.
                appendTrackDeleteDiagnosticLine("drain timeout state: "
                                                + engine_.describeAudioCallbackStateForDiagnostics());
            }
            appendTrackDeleteDiagnosticLine(
                juce::String("realtime gate enter (outermost=") + (outermost ? "yes" : "no")
                + ") drain waitedMs=" + juce::String(waitedMs, 2)
                + " timeout=" + (drained ? "no" : "YES (proceeding anyway)"));
        }

        ~ScopedTrackDeleteRealtimeGate()
        {
            const bool resumed = engine_.endOfflineRenderGate();
            appendTrackDeleteDiagnosticLine(resumed ? "realtime gate exit; realtime resumed"
                                                    : "realtime gate exit (nested)");
        }

        ScopedTrackDeleteRealtimeGate(const ScopedTrackDeleteRealtimeGate&) = delete;
        ScopedTrackDeleteRealtimeGate& operator=(const ScopedTrackDeleteRealtimeGate&) = delete;

    private:
        PlaybackEngine& engine_;
    };

    [[nodiscard]] const char* trackKindDiagnosticName(const TrackKind kind) noexcept
    {
        switch (kind)
        {
        case TrackKind::Audio: return "Audio";
        case TrackKind::Instrument: return "Instrument";
        case TrackKind::Group: return "Group";
        case TrackKind::Master: return "Master";
        case TrackKind::Midi: return "Midi";
        }
        return "Unknown";
    }
} // namespace

TrackLanesEditCoordinator::TrackLanesEditCoordinator(Session& session,
                                                     PlaybackEngine& playbackEngine,
                                                     PluginInsertHost& pluginHost,
                                                     TrackLanesView& trackLanesView,
                                                     TimelineRulerView& rulerView,
                                                     InspectorView& inspectorView,
                                                     Callbacks callbacks)
    : session_(session)
    , playbackEngine_(playbackEngine)
    , pluginHost_(pluginHost)
    , trackLanesView_(trackLanesView)
    , rulerView_(rulerView)
    , inspectorView_(inspectorView)
    , callbacks_(std::move(callbacks))
{
}

void TrackLanesEditCoordinator::install()
{
    trackLanesView_.setOnDeleteTrackRequested([this](const TrackId tid) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            appendTrackDeleteDiagnosticLine("delete refused (recording/count-in) trackId="
                                            + juce::String((juce::int64)tid));
            return;
        }
        if (tid == kInvalidTrackId)
        {
            return;
        }
        if (!callbacks_.executeUndoableTrackDelete)
        {
            return;
        }
        callbacks_.executeUndoableTrackDelete(
            "Delete track",
            [this, tid](std::optional<PluginUndoStepSides>& outPluginSides,
                        std::optional<InstrumentTrackDeleteUndoSides>& outInstrumentDelete) -> bool {
                const std::shared_ptr<const SessionSnapshot> snap
                    = session_.loadSessionSnapshotForAudioThread();
                if (snap == nullptr || snap->findTrackIndexById(tid) < 0)
                {
                    appendTrackDeleteDiagnosticLine("delete abort (track not found) trackId="
                                                    + juce::String((juce::int64)tid));
                    return false;
                }
                const int ix = snap->findTrackIndexById(tid);
                if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Master)
                {
                    appendTrackDeleteDiagnosticLine("delete abort (master track) trackId="
                                                    + juce::String((juce::int64)tid));
                    return false;
                }
                const Track& delTrack = snap->getTrack(ix);
                const bool hasInstrumentRuntime = callbacks_.getInstrumentHostForTrack
                                                  && callbacks_.getInstrumentHostForTrack(tid) != nullptr;
                const bool hasInserts = pluginHost_.hasAnyInsertOnTrack(tid);
                appendTrackDeleteDiagnosticLine(
                    "delete begin trackId=" + juce::String((juce::int64)tid)
                    + " kind=" + trackKindDiagnosticName(delTrack.getKind())
                    + " name=\"" + delTrack.getName() + "\""
                    + " instrumentRuntime=" + (hasInstrumentRuntime ? "yes" : "no")
                    + " inserts=" + (hasInserts ? "yes" : "no"));
                writeLastOperationBreadcrumb("track delete start id=" + juce::String((juce::int64)tid)
                                             + " kind=" + trackKindDiagnosticName(delTrack.getKind()));

                // ---- Undo capture (before any teardown) --------------------------------------
                appendTrackDeleteDiagnosticLine("undo capture begin trackId="
                                                + juce::String((juce::int64)tid));
                if (hasInserts)
                {
                    PluginUndoStepSides sides;
                    sides.trackId = tid;
                    sides.before = pluginHost_.exportChain(tid);
                    sides.after = PluginTrackChain{};
                    appendTrackDeleteDiagnosticLine(
                        "undo capture: insert chain slots="
                        + juce::String(static_cast<int>(sides.before.slots.size())));
                    outPluginSides = std::move(sides);
                }
                if ((delTrack.getKind() == TrackKind::Instrument
                     || delTrack.getKind() == TrackKind::Midi)
                    && callbacks_.getInstrumentControllerForTrack)
                {
                    InstrumentTrackController* const ctl = callbacks_.getInstrumentControllerForTrack(tid);
                    if (ctl != nullptr && ctl->hasInstrumentTrack())
                    {
                        InstrumentTrackDeleteUndoSides s;
                        s.trackId = tid;
                        s.row = ctl->buildExperimentalInstrumentProjectBlock();
                        if (s.row.enabled)
                        {
                            s.row.trackId = tid;
                            appendTrackDeleteDiagnosticLine(
                                "undo capture: instrument row kind=" + s.row.instrumentKind
                                + " clips=" + juce::String(static_cast<int>(s.row.clips.size()))
                                + " stateBase64Len=" + juce::String(s.row.pluginStateBase64.length())
                                + " descriptor=" + (s.row.hasGenericVst3Descriptor ? "yes" : "no")
                                + " pluginWasLoaded=" + (s.row.pluginWasLoadedOnSave ? "yes" : "no"));
                            outInstrumentDelete = std::move(s);
                        }
                        else
                        {
                            appendTrackDeleteDiagnosticLine(
                                "undo capture: instrument row unavailable (controller inactive); "
                                "undo will restore session row without runtime");
                        }
                    }
                    else
                    {
                        appendTrackDeleteDiagnosticLine(
                            "undo capture: no instrument controller; undo will restore session row "
                            "without runtime");
                    }
                }
                appendTrackDeleteDiagnosticLine(
                    "undo capture end trackId=" + juce::String((juce::int64)tid)
                    + " insertChain=" + (outPluginSides.has_value() ? "yes" : "no")
                    + " instrumentRow=" + (outInstrumentDelete.has_value() ? "yes" : "no"));

                if (callbacks_.disarmRecorderIfTrack)
                {
                    callbacks_.disarmRecorderIfTrack(tid);
                }

                // Stability Slice 3: silence + drain the realtime callback for the whole teardown
                // transaction (editor closes, insert eviction, session mutation, runtime removal).
                const ScopedTrackDeleteRealtimeGate realtimeGate(playbackEngine_);

                if (ix >= 0
                    && (snap->getTrack(ix).getKind() == TrackKind::Instrument
                        || snap->getTrack(ix).getKind() == TrackKind::Midi))
                {
                    if (ExperimentalInstrumentHost* mh = callbacks_.getInstrumentHostForTrack(tid))
                    {
                        appendTrackDeleteDiagnosticLine("phase: close native plugin editor");
                        mh->closeNativeEditor();
                    }
                    appendTrackDeleteDiagnosticLine("phase: detach MIDI editor booking");
                    callbacks_.resetMidiEditorBookingIfOpenOnTrack(tid);
                    appendTrackDeleteDiagnosticLine("phase: tear down instrument timeline UI");
                    callbacks_.tearDownInstrumentTimelineUiForTrack(tid);
                    appendTrackDeleteDiagnosticLine("phase: evict inserts (publish-before-destroy)");
                    pluginHost_.evictPluginForTrackNoUndo(tid);
                    appendTrackDeleteDiagnosticLine("phase: remove session track row");
                    session_.removeTrack(tid);
                    appendTrackDeleteDiagnosticLine(
                        "phase: retire instrument runtime (bridge republish + drain)");
                    callbacks_.removeInstrumentRuntimeForTrack(tid);
                    callbacks_.refreshInstrumentUi();
                }
                else
                {
                    appendTrackDeleteDiagnosticLine("phase: evict inserts (publish-before-destroy)");
                    pluginHost_.evictPluginForTrackNoUndo(tid);
                    appendTrackDeleteDiagnosticLine("phase: remove session track row");
                    session_.removeTrack(tid);
                }
                callbacks_.syncViewportFromSession();
                trackLanesView_.syncTracksFromSession();
                rulerView_.repaint();
                trackLanesView_.repaint();
                inspectorView_.refreshFromSession();
                appendTrackDeleteDiagnosticLine("delete end ok trackId=" + juce::String((juce::int64)tid));
                writeLastOperationBreadcrumb("track delete end ok id=" + juce::String((juce::int64)tid));
                // Stability C3: verify runtime invariants right after the delete completed.
                (void) stability_invariants::runRegisteredStabilityInvariantsCheck("track-delete-end");
                return true;
            });
    });

    trackLanesView_.setOnDuplicateTrackRequested([this](const TrackId tid) { duplicateTrack(tid); });

    trackLanesView_.setCommittedHeaderDragTrackReorder([this](const TrackId movedId,
                                                              const int destSessionIndex) {
        if (movedId == kInvalidTrackId || destSessionIndex < 0)
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Reorder track",
            [this, movedId, destSessionIndex]() -> bool {
                const std::shared_ptr<const SessionSnapshot> before
                    = session_.loadSessionSnapshotForAudioThread();
                if (before == nullptr)
                {
                    return false;
                }
                session_.moveTrack(movedId, destSessionIndex);
                const std::shared_ptr<const SessionSnapshot> after
                    = session_.loadSessionSnapshotForAudioThread();
                if (after == nullptr || after == before)
                {
                    return false;
                }
                callbacks_.syncViewportFromSession();
                trackLanesView_.syncTracksFromSession();
                rulerView_.repaint();
                trackLanesView_.repaint();
                inspectorView_.refreshFromSession();
                return true;
            });
    });

    trackLanesView_.setOnUndoableClipMoveRequested(
        [this](const PlacedClipId clipId,
               const std::int64_t newStart,
               const std::optional<TrackId> destTrack) -> bool {
            if (callbacks_.isRecording() || callbacks_.isCountInActive())
            {
                return false;
            }
            if (clipId == kInvalidPlacedClipId)
            {
                return false;
            }
            bool committed = false;
            callbacks_.executeUndoableSessionEdit(
                "Move clip",
                [this, clipId, newStart, destTrack, &committed]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> snapBefore
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapBefore == nullptr)
                    {
                        return false;
                    }
                    bool found = false;
                    for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                    {
                        const Track& tr = snapBefore->getTrack(ti);
                        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                        {
                            if (tr.getPlacedClip(ci).getId() == clipId)
                            {
                                found = true;
                                break;
                            }
                        }
                        if (found)
                        {
                            break;
                        }
                    }
                    if (!found)
                    {
                        return false;
                    }
                    if (destTrack.has_value())
                    {
                        if (*destTrack == kInvalidTrackId
                            || snapBefore->findTrackIndexById(*destTrack) < 0)
                        {
                            return false;
                        }
                        const int destIx = snapBefore->findTrackIndexById(*destTrack);
                        if (!trackKindAcceptsTimelineAudioClips(snapBefore->getTrack(destIx).getKind()))
                        {
                            return false;
                        }
                        session_.moveClipToTrack(clipId, newStart, *destTrack);
                    }
                    else
                    {
                        session_.moveClip(clipId, newStart);
                    }
                    const std::shared_ptr<const SessionSnapshot> snapAfter
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapAfter == snapBefore)
                    {
                        return false;
                    }
                    callbacks_.syncViewportFromSession();
                    trackLanesView_.syncTracksFromSession();
                    rulerView_.repaint();
                    trackLanesView_.repaint();
                    inspectorView_.refreshFromSession();
                    committed = true;
                    return true;
                });
            return committed;
        });

    trackLanesView_.setOnUndoableClipTrimRequested(
        [this](const PlacedClipId clipId, const ClipTrimEdge edge, const std::int64_t newVal) -> bool {
            if (callbacks_.isRecording() || callbacks_.isCountInActive())
            {
                return false;
            }
            if (clipId == kInvalidPlacedClipId)
            {
                return false;
            }
            bool committed = false;
            callbacks_.executeUndoableSessionEdit(
                "Trim clip",
                [this, clipId, edge, newVal, &committed]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> snapBefore
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapBefore == nullptr)
                    {
                        return false;
                    }
                    bool found = false;
                    for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                    {
                        const Track& tr = snapBefore->getTrack(ti);
                        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                        {
                            if (tr.getPlacedClip(ci).getId() == clipId)
                            {
                                found = true;
                                break;
                            }
                        }
                        if (found)
                        {
                            break;
                        }
                    }
                    if (!found)
                    {
                        return false;
                    }
                    if (edge == ClipTrimEdge::Left)
                    {
                        session_.setClipLeftEdgeTrim(clipId, newVal);
                    }
                    else
                    {
                        session_.setClipRightEdgeVisibleLength(clipId, newVal);
                    }
                    const std::shared_ptr<const SessionSnapshot> snapAfter
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapAfter == snapBefore)
                    {
                        return false;
                    }
                    callbacks_.syncViewportFromSession();
                    trackLanesView_.syncTracksFromSession();
                    rulerView_.repaint();
                    trackLanesView_.repaint();
                    inspectorView_.refreshFromSession();
                    committed = true;
                    return true;
                });
            return committed;
        });

    trackLanesView_.setOnUndoableClipRenameRequested(
        [this](const PlacedClipId clipId, juce::String newName) -> bool {
            if (clipId == kInvalidPlacedClipId)
            {
                return false;
            }
            bool committed = false;
            callbacks_.executeUndoableSessionEdit(
                "Rename clip",
                [this, clipId, name = std::move(newName), &committed]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> snapBefore
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapBefore == nullptr)
                    {
                        return false;
                    }
                    session_.setPlacedClipName(clipId, name);
                    const std::shared_ptr<const SessionSnapshot> snapAfter
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapAfter == snapBefore)
                    {
                        return false;
                    }
                    trackLanesView_.syncTracksFromSession();
                    trackLanesView_.repaint();
                    inspectorView_.refreshFromSession();
                    committed = true;
                    return true;
                });
            return committed;
        });

    trackLanesView_.setOnUndoableClipSplitRequested(
        [this](const PlacedClipId clipId,
               const std::int64_t splitSample,
               const bool clipWasSelected) {
            if (callbacks_.isRecording() || callbacks_.isCountInActive())
            {
                return;
            }
            if (clipId == kInvalidPlacedClipId)
            {
                return;
            }
            std::optional<std::pair<PlacedClipId, PlacedClipId>> splitIds;
            callbacks_.executeUndoableSessionEdit(
                "Split clip",
                [this, clipId, splitSample, &splitIds]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> snapBefore
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapBefore == nullptr)
                    {
                        return false;
                    }
                    bool found = false;
                    for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                    {
                        const Track& tr = snapBefore->getTrack(ti);
                        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                        {
                            if (tr.getPlacedClip(ci).getId() == clipId)
                            {
                                found = true;
                                break;
                            }
                        }
                        if (found)
                        {
                            break;
                        }
                    }
                    if (!found)
                    {
                        return false;
                    }
                    const auto maybe = session_.splitClip(clipId, splitSample);
                    if (!maybe.has_value())
                    {
                        return false;
                    }
                    const std::shared_ptr<const SessionSnapshot> snapAfter
                        = session_.loadSessionSnapshotForAudioThread();
                    if (snapAfter == snapBefore)
                    {
                        return false;
                    }
                    splitIds = *maybe;
                    callbacks_.syncViewportFromSession();
                    trackLanesView_.syncTracksFromSession();
                    rulerView_.repaint();
                    trackLanesView_.repaint();
                    inspectorView_.refreshFromSession();
                    return true;
                });
            if (!clipWasSelected || !splitIds.has_value())
            {
                return;
            }
            const PlacedClipId rightId = splitIds->second;
            const std::shared_ptr<const SessionSnapshot> snap
                = session_.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return;
            }
            for (int ti = 0; ti < snap->getNumTracks(); ++ti)
            {
                const Track& tr = snap->getTrack(ti);
                for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                {
                    if (tr.getPlacedClip(ci).getId() == rightId)
                    {
                        trackLanesView_.selectPlacedClipOnTrack(tr.getId(), rightId);
                        return;
                    }
                }
            }
        });

    auto renameTrack = [this](const TrackId tid, const juce::String& raw) -> bool {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return false;
        }
        if (tid == kInvalidTrackId)
        {
            return false;
        }
        const juce::String trimmed = raw.trim();
        if (trimmed.isEmpty())
        {
            return false;
        }
        const std::shared_ptr<const SessionSnapshot> snapBefore
            = session_.loadSessionSnapshotForAudioThread();
        if (snapBefore == nullptr)
        {
            return false;
        }
        const int ix = snapBefore->findTrackIndexById(tid);
        if (ix < 0)
        {
            return false;
        }
        if (snapBefore->getTrack(ix).getKind() == TrackKind::Master)
        {
            return false;
        }
        if (trimmed == snapBefore->getTrack(ix).getName())
        {
            return false;
        }
        bool committed = false;
        callbacks_.executeUndoableSessionEdit(
            "Rename track",
            [this, tid, trimmed, &committed]() -> bool {
                const std::shared_ptr<const SessionSnapshot> before
                    = session_.loadSessionSnapshotForAudioThread();
                if (before == nullptr)
                {
                    return false;
                }
                session_.setTrackName(tid, trimmed);
                const std::shared_ptr<const SessionSnapshot> after
                    = session_.loadSessionSnapshotForAudioThread();
                if (after == nullptr || after == before)
                {
                    return false;
                }
                callbacks_.syncViewportFromSession();
                trackLanesView_.syncTracksFromSession();
                rulerView_.repaint();
                trackLanesView_.repaint();
                inspectorView_.refreshFromSession();
                committed = true;
                return true;
            });
        return committed;
    };

    trackLanesView_.setOnUndoableRenameTrackRequested(renameTrack);
    trackEditActions_.renameTrack = renameTrack;
    inspectorView_.setRenameTrackHandler(renameTrack);

    // Every handler below is stored in `trackEditActions_` FIRST and then handed to the
    // Inspector, so the mixer strips (bound by explicit TrackId) and the Inspector run the
    // identical undoable path.
    trackEditActions_.setRoutedOutput = [this](const TrackId trackId, const TrackId destId) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Route track output",
            [this, trackId, destId]() -> bool {
                if (session_.loadSessionSnapshotForAudioThread() == nullptr)
                {
                    return false;
                }
                if (!session_.setTrackRoutedOutput(trackId, destId))
                {
                    return false;
                }
                if (session_.loadSessionSnapshotForAudioThread() == nullptr)
                {
                    return false;
                }
                afterSessionRoutingPresentationChanged(
                    callbacks_, trackLanesView_, rulerView_, inspectorView_);
                return true;
            });
    };
    inspectorView_.setRoutedOutputHandler(trackEditActions_.setRoutedOutput);

    // Pre-gain (audio rows): one text commit / one Ctrl+click reset = one undo step. A no-op
    // value (setter returns false) records nothing, so repeated commits of the same number
    // do not pollute the undo stack.
    trackEditActions_.setPreGainDb = [this](const TrackId trackId, const float preGainDb) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Set pre-gain",
            [this, trackId, preGainDb]() -> bool {
                return session_.setTrackPreGainDb(trackId, preGainDb);
            });
    };
    inspectorView_.setPreGainHandler(trackEditActions_.setPreGainDb);

    // Audio input assignment (audio rows): a normal undoable project edit. A no-op pick (setter
    // returns false) records nothing. Blocked while recording so an active take's capture source
    // cannot change mid-flight.
    trackEditActions_.setAudioInput = [this](const TrackId trackId, const TrackInputAssignment assignment) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Set audio input",
            [this, trackId, assignment]() -> bool {
                if (!session_.setTrackInputAssignment(trackId, assignment))
                {
                    return false;
                }
                inspectorView_.refreshFromSession();
                return true;
            });
    };
    inspectorView_.setAudioInputHandler(trackEditActions_.setAudioInput);

    trackEditActions_.setMidiOutputChannel = [this](const TrackId trackId, const int channel) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Set MIDI channel",
            [this, trackId, channel]() -> bool {
                if (!session_.setTrackMidiOutputChannel(trackId, channel))
                {
                    return false;
                }
                // The channel lives on the session row, but the events are baked in the controller's
                // render snapshot, so that snapshot has to be rebuilt or already-placed notes would
                // keep playing on the old channel until the next unrelated edit.
                if (callbacks_.getInstrumentControllerForTrack != nullptr)
                {
                    if (InstrumentTrackController* ctl
                        = callbacks_.getInstrumentControllerForTrack(trackId))
                    {
                        ctl->refreshMidiOutputChannelFromSession();
                    }
                }
                inspectorView_.refreshFromSession();
                return true;
            });
    };
    inspectorView_.setMidiOutputChannelHandler(trackEditActions_.setMidiOutputChannel);

    trackEditActions_.setMidiInput = [this](const TrackId trackId, const TrackMidiInputAssignment assignment) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Set MIDI input",
            [this, trackId, assignment]() -> bool {
                if (!session_.setTrackMidiInputAssignment(trackId, assignment))
                {
                    return false;
                }
                // Runtime follows the UI immediately: devices (re)opened and routing republished
                // for this very pick (the coordinator's 30 Hz tick would also catch it, but the
                // user must never see a configured row that is not yet routed).
                if (callbacks_.onMidiInputAssignmentChanged)
                {
                    callbacks_.onMidiInputAssignmentChanged();
                }
                inspectorView_.refreshFromSession();
                return true;
            });
    };
    inspectorView_.setMidiInputHandler(trackEditActions_.setMidiInput);

    trackEditActions_.setMidiDestination = [this](const TrackId trackId, const TrackId destTrackId) {
        if (callbacks_.isRecording() || callbacks_.isCountInActive())
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Set MIDI destination",
            [this, trackId, destTrackId]() -> bool {
                if (!session_.setTrackMidiDestination(trackId, destTrackId))
                {
                    return false;
                }
                // No render-snapshot rebuild needed: the audio thread resolves the destination
                // from the session snapshot per block. But an open MIDI editor on this track
                // borrowed the *old* destination's host for audition, so close it.
                if (callbacks_.resetMidiEditorBookingIfOpenOnTrack)
                {
                    callbacks_.resetMidiEditorBookingIfOpenOnTrack(trackId);
                }
                inspectorView_.refreshFromSession();
                return true;
            });
    };
    inspectorView_.setMidiDestinationHandler(trackEditActions_.setMidiDestination);

    trackEditActions_.setSendDestination
        = [this](const TrackId trackId, const int sendRowIndex, const TrackId destTrackId) {
            if (callbacks_.isRecording() || callbacks_.isCountInActive())
            {
                return;
            }
            callbacks_.executeUndoableSessionEdit(
                "Set send destination",
                [this, trackId, sendRowIndex, destTrackId]() -> bool {
                    if (session_.loadSessionSnapshotForAudioThread() == nullptr)
                    {
                        return false;
                    }
                    if (!applySendDestinationAtUiSlot(session_, trackId, sendRowIndex, destTrackId))
                    {
                        return false;
                    }
                    afterSessionRoutingPresentationChanged(
                        callbacks_, trackLanesView_, rulerView_, inspectorView_);
                    return true;
                });
        };
    trackEditActions_.setSendAmount
        = [this](const TrackId trackId, const int sendRowIndex, const float amountLinear) {
            if (callbacks_.isRecording() || callbacks_.isCountInActive())
            {
                return;
            }
            callbacks_.executeUndoableSessionEdit(
                "Set send amount",
                [this, trackId, sendRowIndex, amountLinear]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> before
                        = session_.loadSessionSnapshotForAudioThread();
                    if (before == nullptr)
                    {
                        return false;
                    }
                    const int tix = before->findTrackIndexById(trackId);
                    if (tix < 0
                        || findTrackSendVectorIndexForUiSlot(before->getTrack(tix).getSends(), sendRowIndex)
                               < 0)
                    {
                        return false;
                    }
                    if (!session_.setTrackSendAmount(trackId, sendRowIndex, amountLinear))
                    {
                        return false;
                    }
                    afterSessionRoutingPresentationChanged(
                        callbacks_, trackLanesView_, rulerView_, inspectorView_);
                    return true;
                });
        };
    trackEditActions_.setSendEnabled
        = [this](const TrackId trackId, const int sendRowIndex, const bool enabled) {
            if (callbacks_.isRecording() || callbacks_.isCountInActive())
            {
                return;
            }
            callbacks_.executeUndoableSessionEdit(
                "Set send enabled",
                [this, trackId, sendRowIndex, enabled]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> before
                        = session_.loadSessionSnapshotForAudioThread();
                    if (before == nullptr)
                    {
                        return false;
                    }
                    const int tix = before->findTrackIndexById(trackId);
                    if (tix < 0
                        || findTrackSendVectorIndexForUiSlot(before->getTrack(tix).getSends(), sendRowIndex)
                               < 0)
                    {
                        return false;
                    }
                    if (!session_.setTrackSendEnabled(trackId, sendRowIndex, enabled))
                    {
                        return false;
                    }
                    afterSessionRoutingPresentationChanged(
                        callbacks_, trackLanesView_, rulerView_, inspectorView_);
                    return true;
                });
        };
    inspectorView_.setTrackSendHandlers(trackEditActions_.setSendDestination,
                                        trackEditActions_.setSendAmount,
                                        trackEditActions_.setSendEnabled);

    // UI-only mutex with the instrument timeline header row. Audio headers paint inactive
    // when the instrument row is the UI-active row; clicking any audio header clears it.
    // No `Session` change — `Session::activeTrackId_` semantics for Add Clip etc. unchanged.
    trackLanesView_.setHeaderActiveSuppressProvider(
        [this] { return callbacks_.hasAnyKeyedInstrumentControllerActive(); });
    trackLanesView_.setOnAudioHeaderActivated([this] {
        callbacks_.deactivateKeyedInstrumentControllersOnly();
        inspectorView_.refreshFromSession();
    });

    // Input monitoring (Monitor speaker cell on audio headers): runtime-only engine state —
    // deliberately NOT an undoable session edit and never persisted (defaults OFF on open).
    trackLanesView_.setInputMonitoringHooks(
        [this](const TrackId tid) { return playbackEngine_.isTrackInputMonitoringEnabled(tid); },
        [this](const TrackId tid) {
            playbackEngine_.setTrackInputMonitoringEnabled(
                tid, !playbackEngine_.isTrackInputMonitoringEnabled(tid));
        });
}

void TrackLanesEditCoordinator::restoreDeletedInstrumentTrackForUndo(
    const InstrumentTrackDeleteUndoSides& sides)
{
    const TrackId tid = sides.trackId;
    appendTrackDeleteDiagnosticLine("undo restore begin trackId=" + juce::String((juce::int64)tid)
                                    + " kind=" + sides.row.instrumentKind
                                    + " clips=" + juce::String(static_cast<int>(sides.row.clips.size())));
    writeLastOperationBreadcrumb("track delete undo restore start id=" + juce::String((juce::int64)tid));

    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->findTrackIndexById(tid) < 0)
    {
        appendTrackDeleteDiagnosticLine(
            "undo restore abort (restored session row missing) trackId="
            + juce::String((juce::int64)tid));
        return;
    }

    if (sides.row.instrumentKind == "MidiContent")
    {
        // Phase B: plugin-less path — recreate the MIDI content controller and reapply the clips.
        if (!callbacks_.getOrCreateMidiContentControllerForTrack)
        {
            appendTrackDeleteDiagnosticLine("undo restore abort (no midi content factory callback)");
            return;
        }
        const ScopedTrackDeleteRealtimeGate realtimeGateMidi(playbackEngine_);
        InstrumentTrackController* const midiCtl = callbacks_.getOrCreateMidiContentControllerForTrack(tid);
        if (midiCtl == nullptr)
        {
            appendTrackDeleteDiagnosticLine("undo restore abort (midi content create failed) trackId="
                                            + juce::String((juce::int64)tid));
            return;
        }
        if (callbacks_.getDeviceSampleRate)
        {
            const double sr = callbacks_.getDeviceSampleRate();
            if (sr > 0.0)
            {
                midiCtl->setTimelineSampleRate(sr);
            }
        }
        midiCtl->restoreExperimentalInstrumentSingleProjectRow(sides.row, nullptr);
        appendTrackDeleteDiagnosticLine("undo restore end (midi content) trackId="
                                        + juce::String((juce::int64)tid));
        writeLastOperationBreadcrumb("track delete undo restore end id=" + juce::String((juce::int64)tid));
        if (callbacks_.refreshInstrumentUi)
        {
            callbacks_.refreshInstrumentUi();
        }
        return;
    }

    if (!callbacks_.getOrCreateInstrumentRuntimeForTrack)
    {
        appendTrackDeleteDiagnosticLine("undo restore abort (no runtime factory callback)");
        return;
    }

    // Same silence + drain discipline as the delete transaction: the plugin reload below must not
    // race the realtime callback.
    const ScopedTrackDeleteRealtimeGate realtimeGate(playbackEngine_);

    const auto runtime = callbacks_.getOrCreateInstrumentRuntimeForTrack(tid);
    ExperimentalInstrumentHost* const host = runtime.first;
    InstrumentTrackController* const ctl = runtime.second;
    if (ctl == nullptr || host == nullptr)
    {
        appendTrackDeleteDiagnosticLine("undo restore abort (runtime create failed) trackId="
                                        + juce::String((juce::int64)tid));
        return;
    }

    if (callbacks_.getDeviceSampleRate)
    {
        const double sr = callbacks_.getDeviceSampleRate();
        if (sr > 0.0)
        {
            ctl->setTimelineSampleRate(sr);
        }
    }
    ctl->restoreExperimentalInstrumentSingleProjectRow(sides.row, nullptr);

    // Plugin reload mirrors the project-load restore path per instrument kind.
    juce::String warning;
    if (sides.row.instrumentKind == "GenericVst3")
    {
        ctl->runPendingGenericVst3ProjectAutoload(*host, warning);
        (void)ctl->bootstrapGenericCatalogInstrumentShellForSessionTrack(tid);
    }
    else if (sides.row.instrumentKind == "HALionSonic")
    {
        ctl->runPendingHalionSonicProjectAutoload(*host, warning);
    }
    else
    {
        ctl->runPendingGrooveAgentProjectAutoload(*host, warning);
    }

    const bool loaded = host->hasInstrument();
    appendTrackDeleteDiagnosticLine(
        juce::String("undo restore runtime result: ")
        + (loaded ? "plugin loaded" : "placeholder fallback (plugin not loaded; MIDI restored)")
        + (warning.isNotEmpty() ? " warning=\"" + warning.replaceCharacter('\n', ' ') + "\""
                                : juce::String()));
    appendTrackDeleteDiagnosticLine("undo restore end trackId=" + juce::String((juce::int64)tid));
    writeLastOperationBreadcrumb("track delete undo restore end id=" + juce::String((juce::int64)tid));

    if (callbacks_.refreshInstrumentUi)
    {
        callbacks_.refreshInstrumentUi();
    }
}

void TrackLanesEditCoordinator::redoTeardownDeletedInstrumentTrack(const TrackId tid)
{
    appendTrackDeleteDiagnosticLine("redo delete begin trackId=" + juce::String((juce::int64)tid));
    writeLastOperationBreadcrumb("track delete redo start id=" + juce::String((juce::int64)tid));

    // Same hardened transaction as the original delete, minus the session mutation and insert
    // eviction (the redo timeline snapshot and pluginSides->after already handled those).
    const ScopedTrackDeleteRealtimeGate realtimeGate(playbackEngine_);

    if (callbacks_.getInstrumentHostForTrack)
    {
        if (ExperimentalInstrumentHost* mh = callbacks_.getInstrumentHostForTrack(tid))
        {
            appendTrackDeleteDiagnosticLine("phase: close native plugin editor");
            mh->closeNativeEditor();
        }
    }
    appendTrackDeleteDiagnosticLine("phase: detach MIDI editor booking");
    if (callbacks_.resetMidiEditorBookingIfOpenOnTrack)
    {
        callbacks_.resetMidiEditorBookingIfOpenOnTrack(tid);
    }
    appendTrackDeleteDiagnosticLine("phase: tear down instrument timeline UI");
    if (callbacks_.tearDownInstrumentTimelineUiForTrack)
    {
        callbacks_.tearDownInstrumentTimelineUiForTrack(tid);
    }
    appendTrackDeleteDiagnosticLine("phase: retire instrument runtime (bridge republish + drain)");
    if (callbacks_.removeInstrumentRuntimeForTrack)
    {
        callbacks_.removeInstrumentRuntimeForTrack(tid);
    }
    if (callbacks_.refreshInstrumentUi)
    {
        callbacks_.refreshInstrumentUi();
    }
    appendTrackDeleteDiagnosticLine("redo delete end trackId=" + juce::String((juce::int64)tid));
    writeLastOperationBreadcrumb("track delete redo end id=" + juce::String((juce::int64)tid));
}

// =============================================================================
// Duplicate Track (single-row command; header context menu, explicit source TrackId)
// =============================================================================
std::optional<TrackId> TrackLanesEditCoordinator::duplicateTrack(const TrackId sourceTid)
{
    if (callbacks_.isRecording() || callbacks_.isCountInActive())
    {
        appendTrackDeleteDiagnosticLine("duplicate refused (recording/count-in) trackId="
                                        + juce::String((juce::int64)sourceTid));
        return std::nullopt;
    }
    if (sourceTid == kInvalidTrackId || !callbacks_.executeUndoableTrackDelete)
    {
        return std::nullopt;
    }
    std::optional<TrackId> created;
    // One undo step (the Delete Track step shape, inverted): `before` lacks the copy, `after` has
    // it; `pluginSides` carries the copied chain under the NEW id (before = empty); the instrument
    // payload carries the captured project row under the NEW id. Undo = snapshot without the row +
    // chain evicted + runtime retired; redo = snapshot with the row + chain re-imported + runtime
    // recreated — the same application rule the Delete Track step uses, decided by row presence.
    callbacks_.executeUndoableTrackDelete(
        "Duplicate track",
        [this, sourceTid, &created](std::optional<PluginUndoStepSides>& outPluginSides,
                                    std::optional<InstrumentTrackDeleteUndoSides>& outInstrumentRow) -> bool {
            const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
            const int ix = snap != nullptr ? snap->findTrackIndexById(sourceTid) : -1;
            if (ix < 0)
            {
                appendTrackDeleteDiagnosticLine("duplicate abort (track not found) trackId="
                                                + juce::String((juce::int64)sourceTid));
                return false;
            }
            const Track& src = snap->getTrack(ix);
            if (src.getKind() == TrackKind::Master)
            {
                appendTrackDeleteDiagnosticLine("duplicate abort (master track) trackId="
                                                + juce::String((juce::int64)sourceTid));
                return false;
            }
            appendTrackDeleteDiagnosticLine("duplicate begin trackId=" + juce::String((juce::int64)sourceTid)
                                            + " kind=" + trackKindDiagnosticName(src.getKind())
                                            + " name=\"" + src.getName() + "\"");
            writeLastOperationBreadcrumb("track duplicate start id=" + juce::String((juce::int64)sourceTid));

            // ---- 1. Capture under the realtime gate: insert chain + instrument row with the CURRENT
            // live state. The gate silences and drains the callback (and with it the render
            // workers), so no state capture overlaps processing of the same instance.
            PluginTrackChain chain;
            std::optional<ProjectFileExperimentalInstrumentTrackV1> row;
            {
                const ScopedTrackDeleteRealtimeGate captureGate(playbackEngine_);
                if (pluginHost_.hasAnyInsertOnTrack(sourceTid))
                {
                    chain = pluginHost_.exportChain(sourceTid);
                }
                if ((src.getKind() == TrackKind::Instrument || src.getKind() == TrackKind::Midi)
                    && callbacks_.getInstrumentControllerForTrack)
                {
                    if (InstrumentTrackController* const ctl = callbacks_.getInstrumentControllerForTrack(sourceTid);
                        ctl != nullptr && ctl->hasInstrumentTrack())
                    {
                        ProjectFileExperimentalInstrumentTrackV1 captured = ctl->buildExperimentalInstrumentProjectBlock();
                        if (captured.enabled)
                        {
                            row = std::move(captured);
                        }
                    }
                }
            }
            appendTrackDeleteDiagnosticLine(
                "duplicate capture: insertSlots=" + juce::String((int)chain.slots.size())
                + " instrumentRow=" + (row.has_value() ? ("yes kind=" + row->instrumentKind
                                                          + " clips=" + juce::String((int)row->clips.size())
                                                          + " stateBase64Len=" + juce::String(row->pluginStateBase64.length()))
                                                       : juce::String("no")));

            // ---- 2. Session: the copy directly below the source (new track + clip ids, same
            // destinations). Nothing points at the copy, so routing stays acyclic.
            const std::optional<TrackId> newTid = session_.duplicateTrack(sourceTid);
            if (!newTid.has_value())
            {
                appendTrackDeleteDiagnosticLine("duplicate abort (session refused) trackId="
                                                + juce::String((juce::int64)sourceTid));
                return false;
            }
            created = newTid;
            appendTrackDeleteDiagnosticLine("duplicate: session row added newTrackId=" + juce::String((juce::int64)*newTid));

            // ---- 3. Inserts: own instances with the captured state (unavailable plug-ins stay
            // placeholders with identity + retained state, exactly like a project load).
            if (!chain.slots.empty())
            {
                // Fresh track id: no instances exist yet, so this instantiates the whole chain
                // (no undo recording happens in the host; the step below carries the sides).
                pluginHost_.importChain(*newTid, chain);
                PluginUndoStepSides sides;
                sides.trackId = *newTid;
                sides.before = PluginTrackChain{};
                sides.after = chain;
                outPluginSides = std::move(sides);
            }

            // ---- 4. Instrument / MIDI-content runtime: the captured row under the new id, through
            // the same restore path project load and Delete-Track undo use (own plug-in instance,
            // state restored; placeholder shell when the plug-in cannot be loaded).
            if (row.has_value())
            {
                InstrumentTrackDeleteUndoSides sides;
                sides.trackId = *newTid;
                sides.row = *row;
                sides.row.trackId = *newTid;
                restoreDeletedInstrumentTrackForUndo(sides);
                outInstrumentRow = std::move(sides);
            }

            // ---- 5. UI: the copy is the active track; Monitor / Arm are runtime-only and start off.
            session_.setActiveTrack(*newTid);
            callbacks_.syncViewportFromSession();
            trackLanesView_.syncTracksFromSession();
            if (callbacks_.refreshInstrumentUi)
            {
                callbacks_.refreshInstrumentUi();
            }
            rulerView_.repaint();
            trackLanesView_.repaint();
            inspectorView_.refreshFromSession();
            appendTrackDeleteDiagnosticLine("duplicate end ok sourceTrackId=" + juce::String((juce::int64)sourceTid)
                                            + " newTrackId=" + juce::String((juce::int64)*newTid));
            writeLastOperationBreadcrumb("track duplicate end ok id=" + juce::String((juce::int64)*newTid));
            (void) stability_invariants::runRegisteredStabilityInvariantsCheck("track-duplicate-end");
            return true;
        });
    return created;
}
