#include "app/ClipPasteboardController.h"

#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "transport/Transport.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

ClipPasteboardController::ClipPasteboardController(Session& session,
                                                   Transport& transport,
                                                   TrackLanesView& trackLanesView,
                                                   TimelineRulerView& rulerView,
                                                   InspectorView& inspectorView,
                                                   Callbacks callbacks)
    : session_(session)
    , transport_(transport)
    , trackLanesView_(trackLanesView)
    , rulerView_(rulerView)
    , inspectorView_(inspectorView)
    , callbacks_(std::move(callbacks))
{
}

void ClipPasteboardController::invokeDeleteSelectedPlacedClipFromWindowShortcut()
{
    if (callbacks_.isRecording() || callbacks_.isCountInActive())
    {
        return;
    }
    const std::optional<std::pair<TrackId, PlacedClipId>> sel = trackLanesView_.getAggregatedSelectedClip();
    if (!sel.has_value())
    {
        return;
    }
    const TrackId tid = sel->first;
    const PlacedClipId pid = sel->second;
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    const int ti = (snap != nullptr) ? snap->findTrackIndexById(tid) : -1;
    if (ti < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(ti);
    bool found = false;
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        if (tr.getPlacedClip(i).getId() == pid)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        return;
    }
    callbacks_.executeUndoableSessionEdit(
        "Delete event",
        [this, tid, pid]() -> bool {
            session_.removePlacedClip(tid, pid);
            trackLanesView_.notifyPlacedClipRemoved(tid, pid);
            callbacks_.syncViewportFromSession();
            trackLanesView_.syncTracksFromSession();
            rulerView_.repaint();
            trackLanesView_.repaint();
            inspectorView_.refreshFromSession();
            return true;
        });
}

void ClipPasteboardController::invokeCopySelectedClipFromWindowShortcut()
{
    const std::optional<std::pair<TrackId, PlacedClipId>> sel = trackLanesView_.getAggregatedSelectedClip();
    if (!sel.has_value())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int tIdx = snap->findTrackIndexById(sel->first);
    if (tIdx < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(tIdx);
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        const PlacedClip& p = tr.getPlacedClip(i);
        if (p.getId() != sel->second)
        {
            continue;
        }
        InternalClipPasteboard pb;
        pb.material = p.getMaterial();
        pb.leftTrimSamples = p.getLeftTrimSamples();
        pb.visibleLengthSamples = p.getEffectiveLengthSamples();
        pb.materialWindowStartSamples = p.getMaterialWindowStartSamples();
        pb.materialWindowEndExclusiveSamples = p.getMaterialWindowEndExclusiveSamples();
        clipPasteboard_ = std::move(pb);
        return;
    }
}

void ClipPasteboardController::invokePasteClipFromWindowShortcut()
{
    if (callbacks_.isRecording() || callbacks_.isCountInActive())
    {
        return;
    }
    if (!clipPasteboard_.has_value())
    {
        return;
    }
    const TrackId target = session_.getActiveTrackId();
    if (target == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->findTrackIndexById(target) < 0)
    {
        return;
    }
    const InternalClipPasteboard pb = *clipPasteboard_;
    if (pb.material == nullptr || pb.visibleLengthSamples <= 0)
    {
        return;
    }
    callbacks_.executeUndoableSessionEdit(
        "Paste clip",
        [this, target, pb]() -> bool {
            const juce::Result r = session_.addPlacedClipFromExistingMaterial(
                pb.material,
                transport_.readPlayheadSamplesForUi(),
                pb.leftTrimSamples,
                pb.visibleLengthSamples,
                target,
                pb.materialWindowStartSamples,
                pb.materialWindowEndExclusiveSamples);
            if (!r.wasOk())
            {
                return false;
            }
            callbacks_.syncViewportFromSession();
            trackLanesView_.syncTracksFromSession();
            trackLanesView_.selectFrontPlacedClipOnTrack(target);
            rulerView_.repaint();
            trackLanesView_.repaint();
            inspectorView_.refreshFromSession();
            return true;
        });
}
