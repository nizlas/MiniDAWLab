#include "app/ClipPasteboardController.h"

#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "transport/Transport.h"
#include "ui/ArrangementGroupMove.h"
#include "ui/TrackLanesView.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace
{
    [[nodiscard]] bool sessionTrackOwnsTimelineMidiClips(const Session& session, const TrackId tid) noexcept
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
        return trackKindOwnsTimelineMidiClips(snap->getTrack(ix).getKind());
    }

    [[nodiscard]] ArrangementEventSelectionRef toSelectionRef(const ArrangementEventId& id) noexcept
    {
        ArrangementEventSelectionRef ref;
        ref.kind = id.kind == ArrangementEventKind::MidiClip ? ArrangementEventSelectionKind::MidiClip
                                                             : ArrangementEventSelectionKind::AudioClip;
        ref.trackId = id.trackId;
        ref.eventId = id.eventId;
        return ref;
    }

    [[nodiscard]] juce::String editLabel(const char* oneAudio,
                                         const char* oneMidi,
                                         const char* many,
                                         const std::vector<ArrangementEventId>& ids)
    {
        if (ids.size() == 1 && ids.front().kind == ArrangementEventKind::MidiClip)
        {
            return oneMidi;
        }
        if (ids.size() == 1)
        {
            return oneAudio;
        }
        return many;
    }
} // namespace

ClipPasteboardController::ClipPasteboardController(Session& session,
                                                   Transport& transport,
                                                   TrackLanesView& trackLanesView,
                                                   Callbacks callbacks)
    : session_(session)
    , transport_(transport)
    , trackLanesView_(trackLanesView)
    , callbacks_(std::move(callbacks))
{
}

TrackId ClipPasteboardController::resolveInstrumentMidiPasteTargetTrack(
    const TrackId sourceTrackFromPasteboard) const noexcept
{
    const auto midiSel = trackLanesView_.getAggregatedSelectedInstrumentMidiClipSelection();
    if (midiSel.has_value() && sessionTrackOwnsTimelineMidiClips(session_, midiSel->first))
    {
        return midiSel->first;
    }
    if (sourceTrackFromPasteboard != kInvalidTrackId
        && sessionTrackOwnsTimelineMidiClips(session_, sourceTrackFromPasteboard))
    {
        return sourceTrackFromPasteboard;
    }
    const TrackId active = session_.getActiveTrackId();
    if (sessionTrackOwnsTimelineMidiClips(session_, active))
    {
        return active;
    }
    return kInvalidTrackId;
}

bool ClipPasteboardController::selectionIsMidiOnOneTrack(const ClipboardPayload& payload) const noexcept
{
    if (payload.items.empty())
    {
        return false;
    }
    for (const ClipboardItem& item : payload.items)
    {
        if (item.offset.kind != ArrangementEventKind::MidiClip || item.offset.trackOffset != 0)
        {
            return false;
        }
    }
    return true;
}

std::optional<ClipPasteboardController::ClipboardPayload> ClipPasteboardController::captureSelection()
{
    std::vector<ArrangementEventId> selected;
    trackLanesView_.copyArrangementEventSelection(selected);
    if (selected.empty())
    {
        return std::nullopt;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return std::nullopt;
    }

    struct Captured
    {
        ArrangementClipboardSourceSample source;
        ClipboardItem item;
        TrackId trackId = kInvalidTrackId;
    };
    std::vector<Captured> captured;
    captured.reserve(selected.size());
    for (const ArrangementEventId& id : selected)
    {
        const int trackIndex = snap->findTrackIndexById(id.trackId);
        if (trackIndex < 0)
        {
            return std::nullopt;
        }
        Captured row;
        row.trackId = id.trackId;
        row.source.kind = id.kind;
        row.source.trackIndex = trackIndex;
        row.item.offset.kind = id.kind;
        if (id.kind == ArrangementEventKind::AudioClip)
        {
            const Track& track = snap->getTrack(trackIndex);
            const PlacedClip* placed = nullptr;
            for (int i = 0; i < track.getNumPlacedClips(); ++i)
            {
                if (track.getPlacedClip(i).getId() == id.eventId)
                {
                    placed = &track.getPlacedClip(i);
                    break;
                }
            }
            if (placed == nullptr || placed->getMaterial() == nullptr)
            {
                return std::nullopt;
            }
            row.source.startSamples = placed->getStartSample();
            row.item.material = placed->getMaterial();
            row.item.leftTrimSamples = placed->getLeftTrimSamples();
            row.item.visibleLengthSamples = placed->getEffectiveLengthSamples();
            row.item.materialWindowStartSamples = placed->getMaterialWindowStartSamples();
            row.item.materialWindowEndExclusiveSamples = placed->getMaterialWindowEndExclusiveSamples();
            row.item.eventGainDb = placed->getEventGainDb();
            row.item.fadeInSamples = placed->getFadeInSamples();
            row.item.fadeOutSamples = placed->getFadeOutSamples();
            row.item.displayName = placed->getDisplayName();
        }
        else
        {
            if (!callbacks_.getInstrumentControllerForTrack)
            {
                return std::nullopt;
            }
            InstrumentTrackController* const controller = callbacks_.getInstrumentControllerForTrack(id.trackId);
            const InstrumentMidiClip* const clip = controller != nullptr ? controller->getClipById(id.eventId) : nullptr;
            if (clip == nullptr)
            {
                return std::nullopt;
            }
            row.source.startSamples = clip->startSamples;
            row.source.anchorOffsetFromStartSamples = clip->timelineAnchorSamples - clip->startSamples;
            row.item.midiClip = *clip;
        }
        captured.push_back(std::move(row));
    }

    std::vector<ArrangementClipboardSourceSample> sources;
    sources.reserve(captured.size());
    for (const Captured& row : captured)
    {
        sources.push_back(row.source);
    }
    std::vector<ArrangementClipboardOffset> offsets;
    if (!buildArrangementClipboardOffsets(sources, offsets) || offsets.size() != captured.size())
    {
        return std::nullopt;
    }

    ClipboardPayload payload;
    int topIndex = std::numeric_limits<int>::max();
    TrackId topTrack = kInvalidTrackId;
    for (size_t i = 0; i < captured.size(); ++i)
    {
        captured[i].item.offset = offsets[i];
        payload.items.push_back(std::move(captured[i].item));
        if (sources[i].trackIndex < topIndex)
        {
            topIndex = sources[i].trackIndex;
            topTrack = captured[i].trackId;
        }
    }
    if (selectionIsMidiOnOneTrack(payload))
    {
        payload.midiSameTrackSourceId = topTrack;
    }
    return payload;
}

void ClipPasteboardController::refreshAfterEdit() const
{
    if (callbacks_.syncViewportFromSession)
    {
        callbacks_.syncViewportFromSession();
    }
    trackLanesView_.syncTracksFromSession();
    if (callbacks_.refreshInstrumentArrangementUi)
    {
        callbacks_.refreshInstrumentArrangementUi();
    }
    if (callbacks_.repaintRuler)
    {
        callbacks_.repaintRuler();
    }
    trackLanesView_.repaint();
    if (callbacks_.refreshInspector)
    {
        callbacks_.refreshInspector();
    }
}

void ClipPasteboardController::deleteCurrentSelection(const juce::String& labelOverride)
{
    if ((callbacks_.isRecording && callbacks_.isRecording())
        || (callbacks_.isCountInActive && callbacks_.isCountInActive())
        || !callbacks_.executeUndoableArrangementEdit)
    {
        return;
    }
    const std::optional<ClipboardPayload> captured = captureSelection();
    if (!captured.has_value())
    {
        return;
    }
    std::vector<ArrangementEventId> selected;
    trackLanesView_.copyArrangementEventSelection(selected);
    if (selected.size() != captured->items.size())
    {
        return;
    }
    const juce::String label = labelOverride.isNotEmpty()
                                   ? labelOverride
                                   : editLabel("Delete event", "Delete MIDI clip", "Delete events", selected);
    callbacks_.executeUndoableArrangementEdit(label, [this, selected]() -> bool {
        std::vector<std::pair<TrackId, PlacedClipId>> audio;
        std::unordered_map<TrackId, std::vector<InstrumentMidiClipId>> midi;
        for (const ArrangementEventId& id : selected)
        {
            if (id.kind == ArrangementEventKind::AudioClip)
            {
                audio.emplace_back(id.trackId, id.eventId);
            }
            else
            {
                midi[id.trackId].push_back(id.eventId);
            }
        }
        for (const auto& group : midi)
        {
            if (!callbacks_.getInstrumentControllerForTrack)
            {
                return false;
            }
            InstrumentTrackController* const controller = callbacks_.getInstrumentControllerForTrack(group.first);
            if (controller == nullptr)
            {
                return false;
            }
            for (const InstrumentMidiClipId id : group.second)
            {
                if (controller->getClipById(id) == nullptr)
                {
                    return false;
                }
            }
        }
        for (const auto& group : midi)
        {
            InstrumentTrackController* const controller = callbacks_.getInstrumentControllerForTrack(group.first);
            if (controller == nullptr || !controller->removeInstrumentMidiClipsByIds(group.second))
            {
                return false;
            }
        }
        if (!session_.tryRemovePlacedClipsTogether(audio))
        {
            return false;
        }
        if (callbacks_.noteArrangementSelectionUndo)
        {
            ArrangementSelectionUndoSides sides;
            sides.before.reserve(selected.size());
            for (const ArrangementEventId& id : selected)
            {
                sides.before.push_back(toSelectionRef(id));
            }
            callbacks_.noteArrangementSelectionUndo(std::move(sides));
        }
        trackLanesView_.setArrangementEventSelection({});
        refreshAfterEdit();
        return true;
    });
}

void ClipPasteboardController::invokeDeleteSelectedPlacedClipFromWindowShortcut()
{
    deleteCurrentSelection();
}

void ClipPasteboardController::invokeCopySelectedClipFromWindowShortcut()
{
    std::optional<ClipboardPayload> captured = captureSelection();
    if (!captured.has_value())
    {
        return;
    }
    clipboard_ = std::move(captured);
}

void ClipPasteboardController::invokeCutSelectedClipsFromWindowShortcut()
{
    if ((callbacks_.isRecording && callbacks_.isRecording())
        || (callbacks_.isCountInActive && callbacks_.isCountInActive()))
    {
        return;
    }
    std::optional<ClipboardPayload> captured = captureSelection();
    if (!captured.has_value())
    {
        return;
    }
    clipboard_ = std::move(captured);
    deleteCurrentSelection(clipboard_->items.size() == 1 ? juce::String("Cut event") : juce::String("Cut events"));
}

void ClipPasteboardController::invokePasteClipFromWindowShortcut()
{
    if ((callbacks_.isRecording && callbacks_.isRecording())
        || (callbacks_.isCountInActive && callbacks_.isCountInActive())
        || !clipboard_.has_value() || clipboard_->items.empty()
        || !callbacks_.executeUndoableArrangementEdit)
    {
        return;
    }
    const ClipboardPayload payload = *clipboard_;
    const bool midiSameTrack = selectionIsMidiOnOneTrack(payload);
    TrackId anchorTrack = session_.getActiveTrackId();
    if (midiSameTrack)
    {
        anchorTrack = resolveInstrumentMidiPasteTargetTrack(payload.midiSameTrackSourceId);
        if (anchorTrack == kInvalidTrackId)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Paste MIDI",
                "No MIDI or instrument track is available to paste MIDI clips onto.",
                "OK");
            return;
        }
    }

    std::vector<ArrangementEventId> beforeSelection;
    trackLanesView_.copyArrangementEventSelection(beforeSelection);
    const juce::String label = payload.items.size() > 1
                                   ? juce::String("Paste events")
                                   : (midiSameTrack ? juce::String("Paste MIDI clip") : juce::String("Paste clip"));
    callbacks_.executeUndoableArrangementEdit(
        label,
        [this, payload, anchorTrack, beforeSelection]() -> bool {
            const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return false;
            }
            const int anchorIndex = snap->findTrackIndexById(anchorTrack);
            if (anchorIndex < 0)
            {
                return false;
            }
            std::vector<TrackKind> kinds;
            std::vector<TrackId> ids;
            std::vector<int> collapsed;
            kinds.reserve((size_t)snap->getNumTracks());
            ids.reserve((size_t)snap->getNumTracks());
            collapsed.reserve((size_t)snap->getNumTracks());
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& track = snap->getTrack(i);
                kinds.push_back(track.getKind());
                ids.push_back(track.getId());
                collapsed.push_back(trackLanesView_.isTrackInCollapsedVisualGroup(track.getId()) ? 1 : 0);
            }
            std::vector<ArrangementClipboardOffset> offsets;
            offsets.reserve(payload.items.size());
            for (const ClipboardItem& item : payload.items)
            {
                offsets.push_back(item.offset);
            }
            std::int64_t playhead = transport_.readPlayheadSamplesForUi();
            if (callbacks_.snapArrangementTimelineSample)
            {
                playhead = callbacks_.snapArrangementTimelineSample(playhead);
            }
            const ArrangementClipboardPastePlan plan = planArrangementClipboardPaste(
                offsets, anchorIndex, playhead, kinds, ids, collapsed);
            if (!plan.valid || plan.dests.size() != payload.items.size())
            {
                return false;
            }

            std::unordered_map<TrackId, std::vector<size_t>> midiByTrack;
            std::vector<Session::AudioClipPastePlacement> audio;
            for (size_t i = 0; i < payload.items.size(); ++i)
            {
                if (payload.items[i].offset.kind == ArrangementEventKind::MidiClip)
                {
                    midiByTrack[plan.dests[i].destTrackId].push_back(i);
                }
                else
                {
                    Session::AudioClipPastePlacement placement;
                    placement.material = payload.items[i].material;
                    placement.startSampleOnTimeline = plan.dests[i].startSamples;
                    placement.leftTrimSamples = payload.items[i].leftTrimSamples;
                    placement.visibleLengthSamples = payload.items[i].visibleLengthSamples;
                    placement.materialWindowStartSamples = payload.items[i].materialWindowStartSamples;
                    placement.materialWindowEndExclusiveSamples = payload.items[i].materialWindowEndExclusiveSamples;
                    placement.eventGainDb = payload.items[i].eventGainDb;
                    placement.fadeInSamples = payload.items[i].fadeInSamples;
                    placement.fadeOutSamples = payload.items[i].fadeOutSamples;
                    placement.displayName = payload.items[i].displayName;
                    placement.destTrackId = plan.dests[i].destTrackId;
                    audio.push_back(std::move(placement));
                }
            }
            for (const auto& group : midiByTrack)
            {
                if (!callbacks_.getInstrumentControllerForTrack
                    || callbacks_.getInstrumentControllerForTrack(group.first) == nullptr)
                {
                    return false;
                }
            }

            std::vector<ArrangementEventId> pasted;
            for (const auto& group : midiByTrack)
            {
                InstrumentTrackController* const controller = callbacks_.getInstrumentControllerForTrack(group.first);
                if (controller == nullptr)
                {
                    return false;
                }
                std::vector<InstrumentMidiClip> clips;
                std::vector<std::pair<std::int64_t, std::int64_t>> placements;
                clips.reserve(group.second.size());
                placements.reserve(group.second.size());
                for (const size_t index : group.second)
                {
                    clips.push_back(payload.items[index].midiClip);
                    placements.emplace_back(plan.dests[index].startSamples, plan.dests[index].anchorSamples);
                }
                const std::vector<InstrumentMidiClipId> newIds
                    = controller->appendDeepCopiedInstrumentMidiClips(clips, placements);
                if (newIds.size() != clips.size())
                {
                    return false;
                }
                for (const InstrumentMidiClipId id : newIds)
                {
                    pasted.push_back(ArrangementEventId { ArrangementEventKind::MidiClip, group.first, id });
                }
            }
            const Session::AudioClipPasteResult audioResult = session_.tryPasteAudioClipsTogether(audio);
            if (!audioResult.ok || audioResult.newIds.size() != audio.size())
            {
                return false;
            }
            size_t audioIndex = 0;
            for (size_t i = 0; i < payload.items.size(); ++i)
            {
                if (payload.items[i].offset.kind != ArrangementEventKind::AudioClip)
                {
                    continue;
                }
                pasted.push_back(ArrangementEventId {
                    ArrangementEventKind::AudioClip, plan.dests[i].destTrackId, audioResult.newIds[audioIndex++] });
            }
            if (callbacks_.noteArrangementSelectionUndo)
            {
                ArrangementSelectionUndoSides sides;
                for (const ArrangementEventId& id : beforeSelection)
                {
                    sides.before.push_back(toSelectionRef(id));
                }
                for (const ArrangementEventId& id : pasted)
                {
                    sides.after.push_back(toSelectionRef(id));
                }
                callbacks_.noteArrangementSelectionUndo(std::move(sides));
            }
            refreshAfterEdit();
            trackLanesView_.setArrangementEventSelection(pasted);
            return true;
        });
}
