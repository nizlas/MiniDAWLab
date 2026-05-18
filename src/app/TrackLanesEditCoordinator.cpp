#include "app/TrackLanesEditCoordinator.h"

#include <memory>
#include <optional>
#include <utility>

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

TrackLanesEditCoordinator::TrackLanesEditCoordinator(Session& session,
                                                     PluginInsertHost& pluginHost,
                                                     TrackLanesView& trackLanesView,
                                                     TimelineRulerView& rulerView,
                                                     InspectorView& inspectorView,
                                                     Callbacks callbacks)
    : session_(session)
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
            return;
        }
        if (tid == kInvalidTrackId)
        {
            return;
        }
        callbacks_.executeUndoableSessionEdit(
            "Delete track",
            [this, tid]() -> bool {
                const std::shared_ptr<const SessionSnapshot> snap
                    = session_.loadSessionSnapshotForAudioThread();
                if (snap == nullptr || snap->findTrackIndexById(tid) < 0)
                {
                    return false;
                }
                const int ix = snap->findTrackIndexById(tid);
                if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Master)
                {
                    return false;
                }
                if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Instrument)
                {
                    if (ExperimentalInstrumentHost* mh = callbacks_.getInstrumentHostForTrack(tid))
                    {
                        mh->closeNativeEditor();
                    }
                    callbacks_.resetMidiEditorBookingIfOpenOnTrack(tid);
                    callbacks_.tearDownInstrumentTimelineUiForTrack(tid);
                    pluginHost_.evictPluginForTrackNoUndo(tid);
                    session_.removeTrack(tid);
                    callbacks_.removeInstrumentRuntimeForTrack(tid);
                    callbacks_.refreshInstrumentUi();
                }
                else
                {
                    pluginHost_.evictPluginForTrackNoUndo(tid);
                    session_.removeTrack(tid);
                }
                callbacks_.syncViewportFromSession();
                trackLanesView_.syncTracksFromSession();
                rulerView_.repaint();
                trackLanesView_.repaint();
                inspectorView_.refreshFromSession();
                return true;
            });
    });

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
    inspectorView_.setRenameTrackHandler(renameTrack);

    // UI-only mutex with the instrument timeline header row. Audio headers paint inactive
    // when the instrument row is the UI-active row; clicking any audio header clears it.
    // No `Session` change — `Session::activeTrackId_` semantics for Add Clip etc. unchanged.
    trackLanesView_.setHeaderActiveSuppressProvider(
        [this] { return callbacks_.hasAnyKeyedInstrumentControllerActive(); });
    trackLanesView_.setOnAudioHeaderActivated([this] {
        callbacks_.deactivateKeyedInstrumentControllersOnly();
        inspectorView_.refreshFromSession();
    });
}
