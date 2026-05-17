#include "app/ClipPasteboardController.h"

#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "transport/Transport.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

#include <algorithm>
#include <limits>

namespace
{
    [[nodiscard]] bool sessionTrackIsInstrument(const Session& session, const TrackId tid) noexcept
    {
        if (tid == kInvalidTrackId)
        {
            return false;
        }
        const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return false;
        }
        const int ix = snap->findTrackIndexById(tid);
        if (ix < 0)
        {
            return false;
        }
        return snap->getTrack(ix).getKind() == TrackKind::Instrument;
    }
} // namespace

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

TrackId ClipPasteboardController::resolveInstrumentMidiPasteTargetTrack(
    const TrackId sourceTrackFromPasteboard) const noexcept
{
    const auto midiSel = trackLanesView_.getAggregatedSelectedInstrumentMidiClipSelection();
    if (midiSel.has_value())
    {
        const TrackId tid = midiSel->first;
        if (sessionTrackIsInstrument(session_, tid))
        {
            return tid;
        }
    }
    if (sourceTrackFromPasteboard != kInvalidTrackId
        && sessionTrackIsInstrument(session_, sourceTrackFromPasteboard))
    {
        return sourceTrackFromPasteboard;
    }
    const TrackId active = session_.getActiveTrackId();
    if (sessionTrackIsInstrument(session_, active))
    {
        return active;
    }
    return kInvalidTrackId;
}

void ClipPasteboardController::invokeDeleteSelectedPlacedClipFromWindowShortcut()
{
    if (callbacks_.isRecording() || callbacks_.isCountInActive())
    {
        return;
    }
    const auto midiSel = trackLanesView_.getAggregatedSelectedInstrumentMidiClipSelection();
    if (midiSel.has_value())
    {
        const TrackId tid = midiSel->first;
        if (!sessionTrackIsInstrument(session_, tid))
        {
            return;
        }
        std::vector<InstrumentMidiClipId> ids = midiSel->second;
        callbacks_.executeUndoableInstrumentEdit(
            "Delete MIDI clip",
            [this, tid, ids = std::move(ids)]() mutable -> bool {
                InstrumentTrackController* const c = callbacks_.getInstrumentControllerForTrack(tid);
                if (c == nullptr)
                {
                    return false;
                }
                if (!c->removeInstrumentMidiClipsByIds(ids))
                {
                    return false;
                }
                callbacks_.refreshInstrumentArrangementUi();
                rulerView_.repaint();
                trackLanesView_.repaint();
                inspectorView_.refreshFromSession();
                return true;
            });
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
    const auto midiSel = trackLanesView_.getAggregatedSelectedInstrumentMidiClipSelection();
    if (midiSel.has_value())
    {
        InstrumentTrackController* const c = callbacks_.getInstrumentControllerForTrack(midiSel->first);
        if (c == nullptr)
        {
            return;
        }
        std::vector<const InstrumentMidiClip*> ptrs;
        ptrs.reserve(midiSel->second.size());
        for (const InstrumentMidiClipId id : midiSel->second)
        {
            if (const InstrumentMidiClip* cl = c->getClipById(id))
            {
                ptrs.push_back(cl);
            }
        }
        if (ptrs.empty())
        {
            return;
        }
        std::sort(ptrs.begin(), ptrs.end(), [](const InstrumentMidiClip* a, const InstrumentMidiClip* b) noexcept {
            return a->startSamples < b->startSamples;
        });

        InternalInstrumentMidiPasteboard pb;
        pb.sourceTrackId = midiSel->first;
        pb.groupEarliestStartSamples = ptrs.front()->startSamples;
        pb.clipsSortedByOriginalStart.reserve(ptrs.size());
        for (const InstrumentMidiClip* p : ptrs)
        {
            pb.clipsSortedByOriginalStart.push_back(*p);
        }

        instrumentMidiPasteboard_ = std::move(pb);
        audioPasteboard_.reset();
        payloadKind_ = PayloadKind::InstrumentMidi;
        return;
    }

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
        audioPasteboard_ = std::move(pb);
        instrumentMidiPasteboard_.reset();
        payloadKind_ = PayloadKind::Audio;
        return;
    }
}

void ClipPasteboardController::invokePasteClipFromWindowShortcut()
{
    if (callbacks_.isRecording() || callbacks_.isCountInActive())
    {
        return;
    }
    if (payloadKind_ == PayloadKind::InstrumentMidi && instrumentMidiPasteboard_.has_value())
    {
        const InternalInstrumentMidiPasteboard pb = *instrumentMidiPasteboard_;
        if (pb.clipsSortedByOriginalStart.empty())
        {
            return;
        }
        const TrackId target = resolveInstrumentMidiPasteTargetTrack(pb.sourceTrackId);
        if (target == kInvalidTrackId || !sessionTrackIsInstrument(session_, target))
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Paste MIDI",
                "No instrument track is available to paste MIDI clips onto.",
                "OK");
            return;
        }

        const std::int64_t playheadRaw = transport_.readPlayheadSamplesForUi();
        std::int64_t playhead = playheadRaw;
        if (callbacks_.snapArrangementTimelineSample != nullptr)
        {
            playhead = callbacks_.snapArrangementTimelineSample(playheadRaw);
        }
        const std::int64_t earliest = pb.groupEarliestStartSamples;
        std::vector<std::pair<std::int64_t, std::int64_t>> placements;
        placements.reserve(pb.clipsSortedByOriginalStart.size());
        std::int64_t minStart = std::numeric_limits<std::int64_t>::max();
        for (const InstrumentMidiClip& cl : pb.clipsSortedByOriginalStart)
        {
            const std::int64_t ns = playhead + (cl.startSamples - earliest);
            const std::int64_t na = playhead + (cl.timelineAnchorSamples - earliest);
            placements.push_back({ ns, na });
            minStart = juce::jmin(minStart, ns);
        }
        const std::int64_t shift = (minStart < 0) ? -minStart : 0;
        if (shift != 0)
        {
            for (auto& pr : placements)
            {
                pr.first += shift;
                pr.second += shift;
            }
        }

        callbacks_.executeUndoableInstrumentEdit(
            "Paste MIDI clip",
            [this,
             target,
             snapshots = pb.clipsSortedByOriginalStart,
             placements = std::move(placements)]() mutable -> bool {
                InstrumentTrackController* const c = callbacks_.getInstrumentControllerForTrack(target);
                if (c == nullptr)
                {
                    return false;
                }
                std::vector<InstrumentMidiClipId> newIds
                    = c->appendDeepCopiedInstrumentMidiClips(snapshots, placements);
                if (newIds.empty())
                {
                    return false;
                }
                c->replaceInstrumentMidiClipSelectionOrdered(std::move(newIds));
                callbacks_.refreshInstrumentArrangementUi();
                rulerView_.repaint();
                trackLanesView_.repaint();
                inspectorView_.refreshFromSession();
                const InstrumentMidiClipId active = c->getSelectedClipId();
                if (active != 0)
                {
                    callbacks_.openMidiEditorForInstrumentClip(target, active);
                }
                return true;
            });
        return;
    }

    if (payloadKind_ != PayloadKind::Audio || !audioPasteboard_.has_value())
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
    const InternalClipPasteboard pb = *audioPasteboard_;
    if (pb.material == nullptr || pb.visibleLengthSamples <= 0)
    {
        return;
    }
    callbacks_.executeUndoableSessionEdit(
        "Paste clip",
        [this, target, pb]() -> bool {
            std::int64_t pasteAt = transport_.readPlayheadSamplesForUi();
            if (callbacks_.snapArrangementTimelineSample != nullptr)
            {
                pasteAt = callbacks_.snapArrangementTimelineSample(pasteAt);
            }
            const juce::Result r = session_.addPlacedClipFromExistingMaterial(
                pb.material,
                pasteAt,
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
