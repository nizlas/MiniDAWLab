// =============================================================================
// TrackLanesView.cpp  —  one lane view per `Track` (message thread)
// =============================================================================

#include "ui/TrackLanesView.h"

#include "audio/LatencySettingsStore.h"
#include "ui/ClipWaveformView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "domain/PlacedClip.h"
#include "transport/Transport.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr double kSppMin = 0.1;
    // Match `ClipWaveformView` playhead refresh so preview drain + repaints stay in the same ballpark.
    constexpr int kRecordingPreviewTimerHz = 20;

    // Shared arrange-lane chrome behind waveform / MIDI lanes (`TrackLanesView::paint`).
    constexpr unsigned int kArrangementLaneBackgroundArgb = 0xff252528u;

    /// Horizontal separator in the track-header column only (lane segment keeps subtle grey below).
    constexpr unsigned int kArrangementHeaderRowSeparatorArgb = 0xff181a1du;

    // Single separator family (vertical drawn in `paintOverChildren` so lane children do not cover it).
    constexpr unsigned int kArrangementSeparatorArgb = 0xff4a4a52u;
    constexpr float kArrangementSeparatorAlphaVertical = 0.62f;
    constexpr float kArrangementSeparatorAlphaHorizontal = 0.58f;

    // Move up to `maxSamples` front-most source samples from `from` into `out` (may split a block).
    void peelPeakBlocksBySampleCount(std::vector<RecordingPreviewPeakBlock>& from,
                                     std::vector<RecordingPreviewPeakBlock>& out,
                                     const std::int64_t maxSamples)
    {
        out.clear();
        if (maxSamples <= 0)
        {
            return;
        }
        std::int64_t taken = 0;
        while (taken < maxSamples && !from.empty())
        {
            RecordingPreviewPeakBlock blk = from.front();
            if (blk.numSourceSamples <= 0)
            {
                from.erase(from.begin());
                continue;
            }
            const std::int64_t need = maxSamples - taken;
            const std::int64_t n = static_cast<std::int64_t>(blk.numSourceSamples);
            if (n <= need)
            {
                out.push_back(blk);
                taken += n;
                from.erase(from.begin());
            }
            else
            {
                RecordingPreviewPeakBlock head = blk;
                head.numSourceSamples = static_cast<int>(need);
                out.push_back(head);
                from.front().numSourceSamples -= static_cast<int>(need);
                taken += need;
            }
        }
    }

    [[nodiscard]] std::int64_t totalPeakBlockSamples(const std::vector<RecordingPreviewPeakBlock>& v)
    {
        std::int64_t s = 0;
        for (const auto& b : v)
        {
            s += static_cast<std::int64_t>(juce::jmax(0, b.numSourceSamples));
        }
        return s;
    }

    void peakBlocksAppendPrefixCopy(const std::vector<RecordingPreviewPeakBlock>& src,
                                    std::vector<RecordingPreviewPeakBlock>& dst,
                                    const std::int64_t prefixSamples)
    {
        dst.clear();
        if (prefixSamples <= 0)
        {
            return;
        }
        std::int64_t taken = 0;
        for (const RecordingPreviewPeakBlock& blk : src)
        {
            if (blk.numSourceSamples <= 0)
            {
                continue;
            }
            const std::int64_t ns = static_cast<std::int64_t>(blk.numSourceSamples);
            const std::int64_t need = prefixSamples - taken;
            if (need <= 0)
            {
                break;
            }
            if (ns <= need)
            {
                dst.push_back(blk);
                taken += ns;
            }
            else
            {
                RecordingPreviewPeakBlock head = blk;
                head.numSourceSamples = static_cast<int>(need);
                dst.push_back(head);
                taken += need;
                break;
            }
        }
    }

    void discardPeakSamplesFromFront(std::vector<RecordingPreviewPeakBlock>& v,
                                     std::int64_t nSamples)
    {
        if (nSamples <= 0)
        {
            return;
        }
        while (nSamples > 0 && !v.empty())
        {
            RecordingPreviewPeakBlock& fb = v.front();
            if (fb.numSourceSamples <= 0)
            {
                v.erase(v.begin());
                continue;
            }
            const auto ns = static_cast<std::int64_t>(fb.numSourceSamples);
            if (nSamples >= ns)
            {
                nSamples -= ns;
                v.erase(v.begin());
                continue;
            }
            fb.numSourceSamples -= static_cast<int>(nSamples);
            nSamples = 0;
            break;
        }
    }

    void peakBlocksApplyPlacementCompensation(std::vector<RecordingPreviewPeakBlock> peaksWork,
                                              const std::int64_t rawSegmentTimelineStart,
                                              const std::int64_t placementOffsetSamples,
                                              std::int64_t& outVisibleStartSample,
                                              std::vector<RecordingPreviewPeakBlock>& outPeaks)
    {
        const std::int64_t wanted = rawSegmentTimelineStart + placementOffsetSamples;
        const std::int64_t trimAtProjectOrigin = wanted < std::int64_t{ 0 } ? -wanted : std::int64_t{ 0 };
        outVisibleStartSample = wanted < std::int64_t{ 0 } ? std::int64_t{ 0 } : wanted;
        discardPeakSamplesFromFront(peaksWork, trimAtProjectOrigin);
        outPeaks = std::move(peaksWork);
    }

} // namespace

TrackLanesView::TrackLanesView(
    Session& session,
    Transport& transport,
    TimelineViewportModel& timelineViewport,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorder,
    LatencySettingsStore& latencySettingsStore,
    AudioWaveformCache& waveformCache)
    : session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
    , deviceManager_(deviceManager)
    , recorder_(recorder)
    , latencyStore_(latencySettingsStore)
    , waveformCache_(waveformCache)
{
    setOpaque(true);
    syncTracksFromSession();
    startTimerHz(kRecordingPreviewTimerHz);
}

TrackLanesView::~TrackLanesView()
{
    stopTimer();
    clearCycleRecordingPreviewContext();
    // `TrackLanesView`'s JUCE `Component` has no `removeFromParent()`; keep non-owned shell children
    // alive for `TransportControlsContent`. `removeChildComponent` does not delete the child.
    auto detachFromParentIfAny = [](juce::Component* c) noexcept {
        if (c == nullptr)
        {
            return;
        }
        if (auto* const p = c->getParentComponent())
        {
            p->removeChildComponent(c);
        }
    };
    for (auto& kv : instrumentTimelineAttachments_)
    {
        detachFromParentIfAny(kv.second.header);
        detachFromParentIfAny(kv.second.midiLane);
    }
    instrumentTimelineAttachments_.clear();
}

void TrackLanesView::setTrackHeaderPluginHost(TrackHeaderPluginHost host) noexcept
{
    trackHeaderPluginHost_ = std::move(host);
    headers_.clear();
    lanes_.clear();
    aggregatedSelectedPlacedClip_.reset();
    syncTracksFromSession();
}

void TrackLanesView::timerCallback()
{
    updateRecordingPreviewOverlaysFromRecorder();
}

void TrackLanesView::updateRecordingPreviewOverlaysFromRecorder()
{
    if (!recorder_.isRecording())
    {
        recordingPreviewPeaksAccum_.clear();
        cycleRecordingCompletedPassPeaks_.clear();
        cyclePreviewActive_ = false;
        for (auto& u : lanes_)
        {
            if (u != nullptr)
            {
                u->clearRecordingPreviewOverlay();
            }
        }
        return;
    }

    RecordingPreviewPeakBlock blk;
    while (recorder_.drainNextPreviewBlock(blk))
    {
        recordingPreviewPeaksAccum_.push_back(blk);
    }

    const TrackId recTid = recorder_.getRecordingTrackId();
    const std::int64_t placementOff = latencyStore_.getCurrentRecordingOffsetSamples();

    std::int64_t recStart = recorder_.getRecordingStartSample();
    std::int64_t recLen = recorder_.getRecordedSampleCount();

    const std::uint32_t wrapNow = transport_.readCycleWrapCountForUi();

    // Cycle preview mapping (raw anchors S, L, R). Placement offset mirrors commit math in Main.cpp:
    // wantedPreviewStart = rawSegmentTimelineStart + placementOff; clamp visible start >= 0;
    // discard that many preview source samples before drawing (timeline underflow trim).
    const std::int64_t passLen = cyclePreviewLocR_ - cyclePreviewLocL_;
    const bool cycleRangeUsable = cyclePreviewActive_
                                  && passLen > 0
                                  && cyclePreviewActualStart_ < cyclePreviewLocR_;
    std::int64_t firstSegLen = 0;
    std::vector<std::vector<RecordingPreviewPeakBlock>> compensatedCompletedBehind;

    if (cycleRangeUsable)
    {
        firstSegLen = cyclePreviewLocR_ - cyclePreviewActualStart_;

        while (cyclePreviewLastSeenWrap_ < wrapNow)
        {
            const std::uint32_t alreadyConsumed = cyclePreviewLastSeenWrap_ - cyclePreviewWrapBaseline_;
            const std::int64_t peelLen = (alreadyConsumed == 0u) ? firstSegLen : passLen;
            std::vector<RecordingPreviewPeakBlock> onePass;
            peelPeakBlocksBySampleCount(recordingPreviewPeaksAccum_, onePass, peelLen);
            cycleRecordingCompletedPassPeaks_.push_back(std::move(onePass));
            ++cyclePreviewLastSeenWrap_;
        }

        compensatedCompletedBehind.reserve(cycleRecordingCompletedPassPeaks_.size());
        for (size_t pi = 0; pi < cycleRecordingCompletedPassPeaks_.size(); ++pi)
        {
            const std::int64_t rawAnch
                = (pi == 0) ? cyclePreviewActualStart_ : cyclePreviewLocL_;
            std::vector<RecordingPreviewPeakBlock> work = cycleRecordingCompletedPassPeaks_[pi];
            std::int64_t segVisStartUnused = 0;
            std::vector<RecordingPreviewPeakBlock> comp;
            peakBlocksApplyPlacementCompensation(
                std::move(work), rawAnch, placementOff, segVisStartUnused, comp);
            compensatedCompletedBehind.push_back(std::move(comp));
        }

        const std::uint32_t wraps = (wrapNow >= cyclePreviewWrapBaseline_)
                                    ? (wrapNow - cyclePreviewWrapBaseline_)
                                    : 0u;
        if (wraps == 0u)
        {
            recStart = cyclePreviewActualStart_;
            recLen = juce::jlimit<std::int64_t>(std::int64_t{ 0 }, firstSegLen, recLen);
        }
        else
        {
            const std::int64_t sourceOffset
                = firstSegLen + static_cast<std::int64_t>(wraps - 1u) * passLen;
            const std::int64_t offsetInPass = recLen - sourceOffset;
            recStart = cyclePreviewLocL_;
            recLen = juce::jlimit<std::int64_t>(std::int64_t{ 0 }, passLen, offsetInPass);
        }
    }

    for (auto& u : lanes_)
    {
        if (u == nullptr)
        {
            continue;
        }
        if (u->getTrackId() == recTid)
        {
            if (cycleRangeUsable)
            {
                const std::int64_t wrappedWanted = cyclePreviewLocL_ + placementOff;
                const std::int64_t compensatedLoopAnchorL = (wrappedWanted < 0) ? std::int64_t{ 0 }
                                                                               : wrappedWanted;
                const std::int64_t wrappedPassVisibleLen
                    = (wrappedWanted < 0)
                          ? juce::jmax<std::int64_t>(std::int64_t{ 0 }, passLen + wrappedWanted)
                          : passLen;

                std::int64_t firstSegmentVisLen = 0;
                std::int64_t firstSegmentTimelineStart = 0;
                if (!compensatedCompletedBehind.empty())
                {
                    const std::int64_t seg0wanted
                        = cyclePreviewActualStart_ + placementOff;
                    firstSegmentTimelineStart
                        = (seg0wanted < 0) ? std::int64_t{ 0 } : seg0wanted;
                    firstSegmentVisLen = totalPeakBlockSamples(compensatedCompletedBehind.front());
                }

                std::vector<RecordingPreviewPeakBlock> currentPrefix;
                peakBlocksAppendPrefixCopy(
                    recordingPreviewPeaksAccum_, currentPrefix, recLen);
                std::int64_t currentVisStart = 0;
                std::vector<RecordingPreviewPeakBlock> currentCompensatedPeaks;
                peakBlocksApplyPlacementCompensation(
                    std::move(currentPrefix),
                    recStart,
                    placementOff,
                    currentVisStart,
                    currentCompensatedPeaks);
                const std::int64_t currentVisLen
                    = totalPeakBlockSamples(currentCompensatedPeaks);

                u->setRecordingCyclePassPreviewLayers(
                    compensatedCompletedBehind,
                    firstSegmentTimelineStart,
                    firstSegmentVisLen,
                    compensatedLoopAnchorL,
                    wrappedPassVisibleLen,
                    currentVisStart,
                    currentVisLen,
                    currentCompensatedPeaks);
            }
            else
            {
                std::vector<RecordingPreviewPeakBlock> previewPrefix;
                peakBlocksAppendPrefixCopy(recordingPreviewPeaksAccum_, previewPrefix, recLen);
                std::int64_t visStartSample = 0;
                std::vector<RecordingPreviewPeakBlock> compPeaks;
                peakBlocksApplyPlacementCompensation(
                    std::move(previewPrefix),
                    recStart,
                    placementOff,
                    visStartSample,
                    compPeaks);
                const std::int64_t visLen = totalPeakBlockSamples(compPeaks);
                u->setRecordingPreviewOverlay(visStartSample, visLen, compPeaks);
            }
        }
        else
        {
            u->clearRecordingPreviewOverlay();
        }
    }
}

void TrackLanesView::setCycleRecordingPreviewContext(
    const bool active,
    const std::int64_t loopLeftSample,
    const std::int64_t loopRightSample,
    const std::int64_t actualRecordingStart,
    const std::uint32_t wrapPassCountBaselineAtRecordingStart) noexcept
{
    cyclePreviewActive_ = active;
    cyclePreviewLocL_ = loopLeftSample;
    cyclePreviewLocR_ = loopRightSample;
    cyclePreviewActualStart_ = actualRecordingStart;
    cyclePreviewWrapBaseline_ = wrapPassCountBaselineAtRecordingStart;
    cyclePreviewLastSeenWrap_ = wrapPassCountBaselineAtRecordingStart;
    recordingPreviewPeaksAccum_.clear();
    cycleRecordingCompletedPassPeaks_.clear();
}

void TrackLanesView::clearCycleRecordingPreviewContext() noexcept
{
    cyclePreviewActive_ = false;
    recordingPreviewPeaksAccum_.clear();
    cycleRecordingCompletedPassPeaks_.clear();
}

void TrackLanesView::syncTracksFromSession()
{
    rebuildChildLanesIfNeeded();
    resized();
}

void TrackLanesView::setOnDeleteTrackRequested(
    std::function<void(TrackId)> onDeleteTrackRequested) noexcept
{
    onDeleteTrackRequested_ = std::move(onDeleteTrackRequested);
}

void TrackLanesView::requestDeleteTrackForHeaderMenu(const TrackId tid) noexcept
{
    if (onDeleteTrackRequested_ != nullptr)
    {
        onDeleteTrackRequested_(tid);
    }
}

void TrackLanesView::setOnUndoableClipMoveRequested(
    std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> fn) noexcept
{
    onUndoableClipMoveRequested_ = std::move(fn);
}

void TrackLanesView::setOnUndoableClipTrimRequested(
    std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> fn) noexcept
{
    onUndoableClipTrimRequested_ = std::move(fn);
}

void TrackLanesView::setActiveEditToolProvider(std::function<EditTool()> fn) noexcept
{
    activeEditToolProvider_ = std::move(fn);
}

void TrackLanesView::setOnUndoableClipSplitRequested(
    std::function<void(PlacedClipId, std::int64_t, bool)> fn) noexcept
{
    onUndoableClipSplitRequested_ = std::move(fn);
}

void TrackLanesView::setHeaderActiveSuppressProvider(std::function<bool()> fn) noexcept
{
    headerActiveSuppressProvider_ = std::move(fn);
    for (auto& h : headers_)
    {
        if (h != nullptr)
        {
            h->repaint();
        }
    }
}

void TrackLanesView::setOnAudioHeaderActivated(std::function<void()> fn) noexcept
{
    onAudioHeaderActivated_ = std::move(fn);
}

void TrackLanesView::setOnAudioClipMouseDownClearForeignSelections(std::function<void()> fn) noexcept
{
    onAudioClipMouseDownClearForeignSelections_ = std::move(fn);
}

void TrackLanesView::setStructuralTimelineEditBlockedPredicate(std::function<bool()> fn) noexcept
{
    structuralTimelineEditBlockedPredicate_ = std::move(fn);
}

void TrackLanesView::setInstrumentMidiClipMoveBlockedPredicate(std::function<bool()> fn) noexcept
{
    instrumentMidiClipMoveBlockedPredicate_ = std::move(fn);
}

void TrackLanesView::setCommittedHeaderDragTrackReorder(
    std::function<void(TrackId, int)> fn) noexcept
{
    committedHeaderDragTrackReorder_ = std::move(fn);
}

void TrackLanesView::syncInstrumentTimelineAttachments(
    const std::vector<InstrumentTimelineAttachment>& rows) noexcept
{
    std::unordered_set<TrackId> keep;
    keep.reserve(rows.size());
    for (const InstrumentTimelineAttachment& r : rows)
    {
        if (r.sessionTrackId != kInvalidTrackId)
        {
            keep.insert(r.sessionTrackId);
        }
    }

    for (auto it = instrumentTimelineAttachments_.begin(); it != instrumentTimelineAttachments_.end();)
    {
        if (keep.count(it->first) == 0)
        {
            InstrumentTimelineAttachment& slot = it->second;
            if (slot.header != nullptr && slot.header->getParentComponent() == this)
            {
                removeChildComponent(slot.header);
            }
            if (slot.midiLane != nullptr && slot.midiLane->getParentComponent() == this)
            {
                removeChildComponent(slot.midiLane);
            }
            it = instrumentTimelineAttachments_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const InstrumentTimelineAttachment& row : rows)
    {
        if (row.sessionTrackId == kInvalidTrackId || row.controller == nullptr || row.header == nullptr
            || row.midiLane == nullptr)
        {
            continue;
        }
        InstrumentTimelineAttachment& dst = instrumentTimelineAttachments_[row.sessionTrackId];
        dst = row;
        if (dst.header->getParentComponent() != this)
        {
            addAndMakeVisible(*dst.header);
        }
        if (dst.midiLane->getParentComponent() != this)
        {
            addAndMakeVisible(*dst.midiLane);
        }
    }

    refreshInstrumentHeaderReorderAttachments();
    rebuildVisibleTrackEntries();
    resized();
}

void TrackLanesView::refreshInstrumentHeaderReorderAttachments() noexcept
{
    for (auto& kv : instrumentTimelineAttachments_)
    {
        InstrumentTimelineAttachment& a = kv.second;
        TrackHeaderView* const h = a.header;
        if (h == nullptr)
        {
            continue;
        }

        const bool trioReady = a.controller != nullptr && a.controller->hasInstrumentTrack() && a.midiLane != nullptr
                               && kv.first != kInvalidTrackId && a.sessionTrackId == kv.first;

        if (trioReady && a.controller->getExperimentalInstrumentDomainTrackId() != kv.first)
        {
            juce::Logger::writeToLog(
                "[TrackLanesView] Instrument header drag id "
                + juce::String((juce::int64)kv.first) + " does not match controller domain id "
                + juce::String((juce::int64)a.controller->getExperimentalInstrumentDomainTrackId())
                + " — header reorder disabled until they match.");
            h->setHeaderReorderDrag(std::nullopt, kInvalidTrackId);
            continue;
        }

        const TrackId dragId = trioReady ? kv.first : kInvalidTrackId;

        if (trioReady && dragId != kInvalidTrackId)
        {
            TrackHeaderDragHost dh;
            dh.onHeaderDragBegan
                = [this](const TrackId id, TrackHeaderView* src) { beginHeaderTrackDrag(id, *src); };
            dh.onHeaderDragMoved
                = [this](const TrackId id, const juce::Point<int> p) { updateHeaderTrackDrag(id, p); };
            dh.onHeaderDragEnded = [this](const TrackId id) { endHeaderTrackDrag(id); };
            h->setHeaderReorderDrag(std::move(dh), dragId);
        }
        else
        {
            h->setHeaderReorderDrag(std::nullopt, kInvalidTrackId);
        }
    }
}

void TrackLanesView::rebuildVisibleTrackEntries() noexcept
{
    visibleTrackEntries_.clear();
    const int n = session_.getNumTracks();
    visibleTrackEntries_.reserve((size_t)juce::jmax(0, n));

    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
        if (tid == kInvalidTrackId)
        {
            jassert(false);
            continue;
        }

        if (session_.getTrackKindAtIndex(i) == TrackKind::Audio)
        {
            visibleTrackEntries_.push_back(
                VisibleTrackEntry{ VisibleTrackKind::Audio, tid });
            continue;
        }

        auto itAttach = instrumentTimelineAttachments_.find(tid);
        if (itAttach == instrumentTimelineAttachments_.end())
        {
            juce::Logger::writeToLog(
                juce::String("[TrackLanesView] Snapshot Instrument row id=") + juce::String((juce::int64)tid)
                + " has no instrument UI attachment.");
            continue;
        }

        InstrumentTrackController* const ctl = itAttach->second.controller;
        if (ctl == nullptr || !ctl->hasInstrumentTrack() || itAttach->second.header == nullptr
            || itAttach->second.midiLane == nullptr)
        {
            juce::Logger::writeToLog(juce::String("[TrackLanesView] Instrument attachment incomplete for tid=")
                                     + juce::String((juce::int64)tid) + ".");
            continue;
        }

        if (ctl->getExperimentalInstrumentDomainTrackId() != tid)
        {
            juce::Logger::writeToLog("[TrackLanesView] Snapshot Instrument row id="
                                     + juce::String((juce::int64)tid)
                                     + " mismatches controller domain id="
                                     + juce::String((juce::int64)ctl->getExperimentalInstrumentDomainTrackId())
                                     + ".");
            continue;
        }

        visibleTrackEntries_.push_back(
            VisibleTrackEntry{ VisibleTrackKind::Instrument, tid });
    }
}

bool TrackLanesView::isInstrumentTimelineRowVisible() const noexcept
{
    for (const VisibleTrackEntry& entry : visibleTrackEntries_)
        if (entry.kind == VisibleTrackKind::Instrument)
            return true;
    return false;
}

bool TrackLanesView::isStructuralTimelineEditBlocked() const noexcept
{
    if (structuralTimelineEditBlockedPredicate_)
    {
        return structuralTimelineEditBlockedPredicate_();
    }
    return recorder_.isRecording();
}

bool TrackLanesView::isInstrumentMidiClipMoveBlocked() const noexcept
{
    if (instrumentMidiClipMoveBlockedPredicate_)
    {
        return instrumentMidiClipMoveBlockedPredicate_();
    }
    return recorder_.isRecording();
}

bool TrackLanesView::isClipEditGestureInProgress() const noexcept
{
    for (const auto& u : lanes_)
    {
        if (u == nullptr)
        {
            continue;
        }
        if (u->isClipMoveGestureInProgress() || u->isClipTrimGestureInProgress())
        {
            return true;
        }
    }
    return false;
}

void TrackLanesView::clearAllPlacedClipSelections() noexcept
{
    aggregatedSelectedPlacedClip_.reset();
    for (auto& u : lanes_)
    {
        if (u != nullptr)
        {
            u->clearSelectionOnly();
        }
    }
}

void TrackLanesView::cancelAllClipGesturesAndTransientUiState() noexcept
{
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] TrackLanesView::cancelAllClipGestures entered laneCount="
            + juce::String(static_cast<int>(lanes_.size())));
    }
    aggregatedSelectedPlacedClip_.reset();
    for (auto& u : lanes_)
    {
        if (u != nullptr)
        {
            u->cancelInteractionStateForSnapshotRestore();
        }
    }
    clearHeaderTrackDragState();
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] TrackLanesView::cancelAllClipGestures done");
    }
}

void TrackLanesView::rebuildChildLanesIfNeeded()
{
    prunePerTrackRowHeightsNotInSession();

    const int n = session_.getNumTracks();
    if (n <= 0)
    {
        headers_.clear();
        lanes_.clear();
        perTrackRowHeightPx_.clear();
        aggregatedSelectedPlacedClip_.reset();
        rebuildVisibleTrackEntries();
        return;
    }
    std::vector<TrackId> audioTrackIds;
    audioTrackIds.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        if (session_.getTrackKindAtIndex(i) == TrackKind::Audio)
        {
            const TrackId tid = session_.getTrackIdAtIndex(i);
            if (tid != kInvalidTrackId)
            {
                audioTrackIds.push_back(tid);
            }
        }
    }

    bool need = (((int)lanes_.size() != (int)audioTrackIds.size())
                 || ((int)headers_.size() != (int)audioTrackIds.size()));
    if (!need)
    {
        for (int i = 0; i < (int)audioTrackIds.size(); ++i)
        {
            if (lanes_[(size_t)i]->getTrackId() != audioTrackIds[(size_t)i])
            {
                need = true;
                break;
            }
        }
    }
    if (!need)
    {
        rebuildVisibleTrackEntries();
        refreshInstrumentHeaderReorderAttachments();
        return;
    }
    headers_.clear();
    lanes_.clear();
    aggregatedSelectedPlacedClip_.reset();

    for (const TrackId tid : audioTrackIds)
    {
        if (tid == kInvalidTrackId)
        {
            jassert(false);
            continue;
        }
        TrackHeaderDragHost dragHost;
        dragHost.onHeaderDragBegan
            = [this](const TrackId id, TrackHeaderView* const src) { beginHeaderTrackDrag(id, *src); };
        dragHost.onHeaderDragMoved
            = [this](const TrackId id, const juce::Point<int> p) { updateHeaderTrackDrag(id, p); };
        dragHost.onHeaderDragEnded
            = [this](const TrackId id) { endHeaderTrackDrag(id); };
        const auto onActive = [this] { repaint(); };
        const auto onArm = [this] {
            for (auto& h : headers_)
            {
                h->repaint();
            }
        };
        auto onDelete = [this](const TrackId id) {
            if (onDeleteTrackRequested_ != nullptr)
            {
                onDeleteTrackRequested_(id);
            }
        };

        TrackHeaderModelProvider modelProvider = [this, tid]() -> TrackHeaderModel {
            TrackHeaderModel m;
            m.subtitle = {};
            const bool sessionSaysActive = (session_.getActiveTrackId() == tid);
            const bool suppressed
                = headerActiveSuppressProvider_ != nullptr && headerActiveSuppressProvider_();
            m.active = sessionSaysActive && !suppressed;
            m.armed = (recorder_.getArmedTrackId() == tid);
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    const Track& tr = snap->getTrack(idx);
                    m.name = tr.getName();
                    m.off = tr.isTrackOff();
                    m.muted = tr.isMuted();
                }
            }
            m.powerInteractable = !isStructuralTimelineEditBlocked();
            m.muteInteractable = true;
            m.armInteractable = true;
            return m;
        };

        TrackHeaderCallbacks callbacks;
        callbacks.onActivateName = [this, tid, onActive] {
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onToggleArm = [this, tid, onActive, onArm] {
            if (recorder_.getArmedTrackId() == tid)
            {
                recorder_.disarm();
            }
            else
            {
                recorder_.armForRecording(tid);
            }
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onToggleMute = [this, tid, onActive, onArm] {
            bool nowMuted = true;
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    nowMuted = !snap->getTrack(idx).isMuted();
                }
            }
            session_.setTrackMuted(tid, nowMuted);
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onTogglePower = [this, tid, onActive, onArm]() -> bool {
            if (isStructuralTimelineEditBlocked())
            {
                return false;
            }
            bool nowOff = true;
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    nowOff = !snap->getTrack(idx).isTrackOff();
                }
            }
            session_.setTrackOff(tid, nowOff);
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
            return true;
        };
        callbacks.onShowContextMenu = [this, tid, onActive, onDelete, pluginHost = trackHeaderPluginHost_](
            TrackHeaderView& self, const juce::MouseEvent&) {
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();

            juce::PopupMenu menu;
            constexpr int kDeleteTrackMenuId = 1;
            constexpr int kLoadVst3MenuId = 10;
            constexpr int kPluginEditorMenuId = 11;
            constexpr int kPluginParamsMenuId = 12;
            constexpr int kRemovePluginMenuId = 13;

            const bool editLocked = isStructuralTimelineEditBlocked();
            juce::PopupMenu::Item deleteItem;
            deleteItem.itemID = kDeleteTrackMenuId;
            deleteItem.text = "Delete Track";
            deleteItem.isEnabled = !editLocked;
            menu.addItem(deleteItem);

            if (pluginHost.loadVst3 != nullptr)
            {
                juce::PopupMenu::Item loadItem;
                loadItem.itemID = kLoadVst3MenuId;
                loadItem.text = "Load VST3…";
                loadItem.isEnabled = !editLocked;
                menu.addItem(loadItem);
            }
            if (pluginHost.openPluginEditor != nullptr)
            {
                juce::PopupMenu::Item edItem;
                edItem.itemID = kPluginEditorMenuId;
                edItem.text = "Plugin editor…";
                edItem.isEnabled = !editLocked;
                menu.addItem(edItem);
            }
            if (pluginHost.openPluginParams != nullptr)
            {
                juce::PopupMenu::Item parItem;
                parItem.itemID = kPluginParamsMenuId;
                parItem.text = "Plugin parameters…";
                parItem.isEnabled = !editLocked;
                menu.addItem(parItem);
            }
            if (pluginHost.removePlugin != nullptr)
            {
                juce::PopupMenu::Item rmItem;
                rmItem.itemID = kRemovePluginMenuId;
                rmItem.text = "Remove VST3";
                rmItem.isEnabled = !editLocked;
                menu.addItem(rmItem);
            }

            juce::Component::SafePointer<TrackHeaderView> safeThis(&self);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&self),
                [safeThis,
                 this,
                 pluginHost,
                 tid,
                 onDelete,
                 kDeleteTrackMenuId,
                 kLoadVst3MenuId,
                 kPluginEditorMenuId,
                 kPluginParamsMenuId,
                 kRemovePluginMenuId](const int result) {
                    if (safeThis == nullptr)
                    {
                        return;
                    }
                    if (result == kDeleteTrackMenuId)
                    {
                        if (isStructuralTimelineEditBlocked())
                        {
                            return;
                        }
                        onDelete(tid);
                        return;
                    }
                    if (isStructuralTimelineEditBlocked())
                    {
                        return;
                    }
                    if (result == kLoadVst3MenuId && pluginHost.loadVst3 != nullptr)
                    {
                        pluginHost.loadVst3(tid);
                    }
                    else if (result == kPluginEditorMenuId && pluginHost.openPluginEditor != nullptr)
                    {
                        pluginHost.openPluginEditor(tid);
                    }
                    else if (result == kPluginParamsMenuId && pluginHost.openPluginParams != nullptr)
                    {
                        pluginHost.openPluginParams(tid);
                    }
                    else if (result == kRemovePluginMenuId && pluginHost.removePlugin != nullptr)
                    {
                        pluginHost.removePlugin(tid);
                    }
                });
        };

        callbacks.onRowHeightDrag = [this, tid](const int startH, const int delta) {
            applyTrackRowHeightDelta(tid, startH, delta);
        };
        callbacks.onRowHeightDragEnd = [] {};

        auto head = std::make_unique<TrackHeaderView>(
            std::move(modelProvider),
            std::move(callbacks),
            tid,
            std::optional<TrackHeaderDragHost>(std::move(dragHost)));
        addAndMakeVisible(*head);
        headers_.push_back(std::move(head));
        ClipWaveformLaneHost host;
        host.onBeginMouseDown = [this](ClipWaveformView& sender) {
            for (auto& u : lanes_)
            {
                if (u.get() != &sender)
                {
                    u->clearSelectionOnly();
                }
            }
            if (onAudioClipMouseDownClearForeignSelections_ != nullptr)
            {
                onAudioClipMouseDownClearForeignSelections_();
            }
        };
        host.findLaneAtScreen = [this](const juce::Point<int> screenPos) -> ClipWaveformView* {
            return findLaneAtScreenPosition(screenPos);
        };
        host.setGhostOnLane
            = [this](ClipWaveformView* target, const std::int64_t start, const std::int64_t len) {
                  setGhostOnLaneImpl(target, start, len);
              };
        host.clearAllGhosts = [this] { clearAllGhostsImpl(); };
        host.onPlacedClipSelectionChanged =
            [this](const TrackId laneId, const std::optional<PlacedClipId> id) {
                onLanePlacedClipSelectionChanged(laneId, id);
            };
        host.commitClipMoveAsUndoable =
            [this](const PlacedClipId id, const std::int64_t start, const std::optional<TrackId> dest) -> bool {
                if (onUndoableClipMoveRequested_ != nullptr)
                {
                    return onUndoableClipMoveRequested_(id, start, dest);
                }
                if (dest.has_value())
                {
                    session_.moveClipToTrack(id, start, *dest);
                }
                else
                {
                    session_.moveClip(id, start);
                }
                return true;
            };
        host.commitClipTrimAsUndoable =
            [this](const PlacedClipId id, const ClipTrimEdge edge, const std::int64_t value) -> bool {
                if (onUndoableClipTrimRequested_ != nullptr)
                {
                    return onUndoableClipTrimRequested_(id, edge, value);
                }
                if (edge == ClipTrimEdge::Left)
                {
                    session_.setClipLeftEdgeTrim(id, value);
                }
                else
                {
                    session_.setClipRightEdgeVisibleLength(id, value);
                }
                return true;
            };
        host.getActiveEditTool = [this]() -> EditTool {
            return activeEditToolProvider_ != nullptr ? activeEditToolProvider_() : EditTool::Pointer;
        };
        host.commitClipSplitAsUndoable =
            [this](const PlacedClipId id, const std::int64_t splitT, const bool wasSel) {
                if (onUndoableClipSplitRequested_ != nullptr)
                {
                    onUndoableClipSplitRequested_(id, splitT, wasSel);
                }
                else
                {
                    (void)session_.splitClip(id, splitT);
                }
            };
        auto ptr = std::make_unique<ClipWaveformView>(
            session_, transport_, tid, timelineViewport_, waveformCache_, std::move(host));
        addAndMakeVisible(*ptr);
        lanes_.push_back(std::move(ptr));
    }
    rebuildVisibleTrackEntries();
    refreshInstrumentHeaderReorderAttachments();
}

ClipWaveformView* TrackLanesView::findLaneAtScreenPosition(const juce::Point<int> screenPos)
{
    const juce::Point<int> local = getLocalPoint(nullptr, screenPos);
    if (!getLocalBounds().contains(local))
    {
        return nullptr;
    }
    for (auto& u : lanes_)
    {
        if (u->getBounds().contains(local))
        {
            return u.get();
        }
    }
    return nullptr;
}

void TrackLanesView::setGhostOnLaneImpl(
    ClipWaveformView* const target,
    const std::int64_t startSample,
    const std::int64_t lengthSamples)
{
    for (auto& u : lanes_)
    {
        if (u.get() == target)
        {
            u->setDragGhost(startSample, lengthSamples);
        }
        else
        {
            u->clearDragGhost();
        }
    }
}

void TrackLanesView::clearAllGhostsImpl()
{
    for (auto& u : lanes_)
    {
        u->clearDragGhost();
    }
}

void TrackLanesView::resized()
{
    rebuildChildLanesIfNeeded();
    rebuildVisibleTrackEntries();

    auto area = getLocalBounds();
    const int vr = static_cast<int>(visibleTrackEntries_.size());

    std::unordered_set<TrackId> instrumentVisibleTids;
    for (const VisibleTrackEntry& ve : visibleTrackEntries_)
        if (ve.kind == VisibleTrackKind::Instrument)
            instrumentVisibleTids.insert(ve.sessionTrackId);

    for (auto& kv : instrumentTimelineAttachments_)
    {
        const bool visible = instrumentVisibleTids.count(kv.first) > 0;
        InstrumentTimelineAttachment& a = kv.second;
        if (a.header != nullptr)
        {
            a.header->setVisible(visible && a.controller != nullptr && a.controller->hasInstrumentTrack());
        }
        if (a.midiLane != nullptr)
        {
            a.midiLane->setVisible(visible && a.controller != nullptr && a.controller->hasInstrumentTrack());
        }
    }

    if (area.getHeight() <= 0 || vr <= 0)
    {
        if (vr <= 0)
        {
            verticalScrollOffsetPx_ = 0;
        }
        return;
    }

    const int viewportH = area.getHeight();
    int contentH = 0;
    for (int vi = 0; vi < vr; ++vi)
    {
        contentH += rowHeightForVisibleEntry(vi);
    }
    verticalScrollOffsetPx_
        = juce::jlimit(0, juce::jmax(0, contentH - viewportH), verticalScrollOffsetPx_);

    const int w = area.getWidth();
    const int leftW = juce::jmin(kTrackHeaderWidth, w);

    int y = area.getY() - verticalScrollOffsetPx_;
    for (int vi = 0; vi < vr; ++vi)
    {
        const int rowH = juce::jmax(1, rowHeightForVisibleEntry(vi));
        juce::Rectangle row(area.getX(), y, w, rowH);
        const VisibleTrackEntry& e = visibleTrackEntries_[(size_t)vi];
        if (e.kind == VisibleTrackKind::Instrument)
        {
            auto itA = instrumentTimelineAttachments_.find(e.sessionTrackId);
            if (itA != instrumentTimelineAttachments_.end() && itA->second.header != nullptr
                && itA->second.midiLane != nullptr)
            {
                itA->second.header->setBounds(row.removeFromLeft(leftW));
                itA->second.midiLane->setBounds(row);
                itA->second.header->toFront(false);
                itA->second.midiLane->toFront(false);
            }
        }
        else
        {
            const int si = audioLaneIndexFromTrackId(e.sessionTrackId);
            if (si >= 0 && si < (int)headers_.size() && si < (int)lanes_.size()
                && headers_[(size_t)si] != nullptr && lanes_[(size_t)si] != nullptr)
            {
                headers_[(size_t)si]->setBounds(row.removeFromLeft(leftW));
                lanes_[(size_t)si]->setBounds(row);
            }
        }
        y += rowH;
    }

    const int tw = juce::jmax(0, getWidth() - kTrackHeaderWidth);
    if (tw > 0)
    {
        timelineViewport_.clampToExtent((double)tw, session_.getArrangementExtentSamples());
    }
}

void TrackLanesView::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    if (bounds.isEmpty())
    {
        return;
    }

    const auto laneBg = juce::Colour(kArrangementLaneBackgroundArgb);
    g.fillAll(laneBg);

    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (vr <= 0)
    {
        return;
    }

    const int ay = bounds.getY();
    const int headerW = juce::jmin(kTrackHeaderWidth, bounds.getWidth());
    const int hx = bounds.getX() + headerW;

    const auto laneSepColour
        = juce::Colour(kArrangementSeparatorArgb).withAlpha(kArrangementSeparatorAlphaHorizontal);

    int yLine = ay - verticalScrollOffsetPx_;
    for (int i = 0; i < vr; ++i)
    {
        yLine += rowHeightForVisibleEntry(i);
        if (yLine <= bounds.getY() || yLine >= bounds.getBottom())
        {
            continue;
        }

        if (hx < bounds.getRight())
        {
            g.setColour(laneSepColour);
            g.drawHorizontalLine(yLine, (float)hx, (float)bounds.getRight());
        }
    }
}

int TrackLanesView::rowHeightForTrack(const TrackId tid) const noexcept
{
    if (tid == kInvalidTrackId)
    {
        return juce::jlimit(minRowHeightPx_, maxRowHeightPx_, defaultRowHeightPx_);
    }
    auto it = perTrackRowHeightPx_.find(tid);
    const int h = (it != perTrackRowHeightPx_.end()) ? it->second : defaultRowHeightPx_;
    return juce::jlimit(minRowHeightPx_, maxRowHeightPx_, h);
}

int TrackLanesView::rowHeightForVisibleEntry(const int visibleIndex) const noexcept
{
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(visibleTrackEntries_.size()))
    {
        return 0;
    }
    return rowHeightForTrack(visibleTrackEntries_[(size_t)visibleIndex].sessionTrackId);
}

int TrackLanesView::visibleRowPixelHeight(const int visibleIndex) const noexcept
{
    return rowHeightForVisibleEntry(visibleIndex);
}

int TrackLanesView::totalContentHeightPx() const noexcept
{
    int sum = 0;
    const int vr = static_cast<int>(visibleTrackEntries_.size());
    for (int i = 0; i < vr; ++i)
    {
        sum += rowHeightForVisibleEntry(i);
    }
    return sum;
}

void TrackLanesView::applyTrackRowHeightDelta(const TrackId tid,
                                              const int startHeightPx,
                                              const int deltaPx) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    const int nh = juce::jlimit(minRowHeightPx_, maxRowHeightPx_, startHeightPx + deltaPx);
    if (nh == defaultRowHeightPx_)
    {
        perTrackRowHeightPx_.erase(tid);
    }
    else
    {
        perTrackRowHeightPx_[tid] = nh;
    }
    resized();
    repaint();
}

void TrackLanesView::prunePerTrackRowHeightsNotInSession() noexcept
{
    std::unordered_set<TrackId> alive;
    const int n = session_.getNumTracks();
    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
        if (tid != kInvalidTrackId)
        {
            alive.insert(tid);
        }
    }

    for (auto it = perTrackRowHeightPx_.begin(); it != perTrackRowHeightPx_.end();)
    {
        if (alive.count(it->first) == 0)
        {
            it = perTrackRowHeightPx_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

int TrackLanesView::audioLaneIndexFromTrackId(const TrackId tid) const noexcept
{
    if (tid == kInvalidTrackId)
    {
        return -1;
    }
    int audioIx = -1;
    const int n = session_.getNumTracks();
    for (int i = 0; i < n; ++i)
    {
        if (session_.getTrackKindAtIndex(i) != TrackKind::Audio)
        {
            continue;
        }
        ++audioIx;
        if (session_.getTrackIdAtIndex(i) == tid)
        {
            return audioIx;
        }
    }
    return -1;
}

int TrackLanesView::maxVerticalScrollOffsetPx() const noexcept
{
    const int vh = getLocalBounds().getHeight();
    return juce::jmax(0, totalContentHeightPx() - vh);
}

void TrackLanesView::setVerticalScrollOffsetPx(const int newOffset) noexcept
{
    verticalScrollOffsetPx_ = newOffset;
    resized();
}

int TrackLanesView::findVisibleRowIndexForDragSource(const TrackId movedId) const noexcept
{
    for (int i = 0; i < static_cast<int>(visibleTrackEntries_.size()); ++i)
    {
        if (visibleTrackEntries_[(size_t)i].sessionTrackId == movedId)
        {
            return i;
        }
    }
    return -1;
}



void TrackLanesView::beginHeaderTrackDrag(const TrackId movedId, TrackHeaderView& sourceView)
{
    headerTrackDragActive_ = true;
    headerTrackDragId_ = movedId;
    headerTrackDragSourceView_ = &sourceView;
    headerTrackDragInsertGapK_ = -1;
    headerTrackDragNoopLineY_ = -1;
    headerTrackDragInvalidArea_ = true;
    headerTrackDragNoop_ = true;
}

void TrackLanesView::updateHeaderTrackDrag(const TrackId movedId, const juce::Point<int> screenPos)
{
    if (!headerTrackDragActive_ || movedId != headerTrackDragId_)
    {
        return;
    }
    const juce::Point<int> local = getLocalPoint(nullptr, screenPos);
    if (!getLocalBounds().contains(local) || local.x >= kTrackHeaderWidth)
    {
        headerTrackDragInvalidArea_ = true;
        headerTrackDragInsertGapK_ = -1;
        headerTrackDragNoopLineY_ = -1;
        headerTrackDragNoop_ = true;
        if (headerTrackDragSourceView_ != nullptr)
        {
            headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
        }
        repaint();
        return;
    }

    headerTrackDragInvalidArea_ = false;
    if (headerTrackDragSourceView_ != nullptr)
    {
        headerTrackDragSourceView_->restoreSourceCursorAfterHeaderDrag();
    }

    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (vr <= 0)
    {
        return;
    }

    {
        const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
        if (snap == nullptr || snap->findTrackIndexById(movedId) < 0)
        {
            headerTrackDragInvalidArea_ = true;
            headerTrackDragInsertGapK_ = -1;
            headerTrackDragNoopLineY_ = -1;
            headerTrackDragNoop_ = true;
            if (headerTrackDragSourceView_ != nullptr)
            {
                headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
            }
            repaint();
            return;
        }
    }

    const int sv = findVisibleRowIndexForDragSource(movedId);
    if (sv < 0)
    {
        headerTrackDragInvalidArea_ = true;
        headerTrackDragInsertGapK_ = -1;
        headerTrackDragNoopLineY_ = -1;
        headerTrackDragNoop_ = true;
        if (headerTrackDragSourceView_ != nullptr)
        {
            headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
        }
        repaint();
        return;
    }

    const int ay = getLocalBounds().getY();
    std::vector<int> gapY((size_t)vr + 1u);
    int accY = ay - verticalScrollOffsetPx_;
    for (int k = 0; k <= vr; ++k)
    {
        gapY[(size_t)k] = accY;
        if (k < vr)
        {
            accY += rowHeightForVisibleEntry(k);
        }
    }

    int bestK = 0;
    int bestAbs = 0x7fffffff;
    for (int k = 0; k <= vr; ++k)
    {
        const int d = local.y - gapY[(size_t)k];
        const int a = d < 0 ? -d : d;
        if (a < bestAbs)
        {
            bestAbs = a;
            bestK = k;
        }
    }

    const int destVis = bestK <= sv ? bestK : (bestK - 1);
    const bool noop = (destVis == sv);
    headerTrackDragNoop_ = noop;
    if (noop)
    {
        // Red: line tracks pointer (valid header column only). Green uses snapped gaps above.
        headerTrackDragInsertGapK_ = -1;
        const int h = getHeight();
        headerTrackDragNoopLineY_ = (h > 0) ? juce::jlimit(0, h - 1, local.y) : 0;
    }
    else
    {
        headerTrackDragNoopLineY_ = -1;
        headerTrackDragInsertGapK_ = bestK;
    }
    repaint();
}

void TrackLanesView::endHeaderTrackDrag(const TrackId movedId)
{
    if (movedId != headerTrackDragId_ || !headerTrackDragActive_)
    {
        return;
    }
    TrackHeaderView* const src = headerTrackDragSourceView_;
    if (src != nullptr)
    {
        src->restoreSourceCursorAfterHeaderDrag();
    }

    const int sv = findVisibleRowIndexForDragSource(movedId);
    const bool commit = (!headerTrackDragInvalidArea_) && !headerTrackDragNoop_
        && (headerTrackDragInsertGapK_ >= 0) && (sv >= 0);
    if (commit)
    {
        const int k = headerTrackDragInsertGapK_;
        const int destSessionIndex = (k <= sv) ? k : (k - 1);
        if (committedHeaderDragTrackReorder_ != nullptr)
        {
            committedHeaderDragTrackReorder_(movedId, destSessionIndex);
        }
        const int tw = juce::jmax(0, getWidth() - kTrackHeaderWidth);
        if (tw > 0)
        {
            timelineViewport_.clampToExtent((double)tw, session_.getArrangementExtentSamples());
        }
    }

    clearHeaderTrackDragState();
    repaint();
}

void TrackLanesView::clearHeaderTrackDragState() noexcept
{
    headerTrackDragActive_ = false;
    headerTrackDragId_ = kInvalidTrackId;
    headerTrackDragSourceView_ = nullptr;
    headerTrackDragInsertGapK_ = -1;
    headerTrackDragNoopLineY_ = -1;
    headerTrackDragInvalidArea_ = true;
    headerTrackDragNoop_ = true;
}

void TrackLanesView::paintHeaderColumnHorizontalRowSeparators(juce::Graphics& g) const noexcept
{
    const auto bounds = getLocalBounds();
    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (bounds.isEmpty() || vr <= 0)
    {
        return;
    }

    const int headerW = juce::jmin(kTrackHeaderWidth, bounds.getWidth());
    const int hx = bounds.getX() + headerW;
    if (hx <= bounds.getX())
    {
        return;
    }

    const int ay = bounds.getY();
    g.setColour(juce::Colour(kArrangementHeaderRowSeparatorArgb));

    int yLine = ay - verticalScrollOffsetPx_;
    for (int i = 0; i < vr; ++i)
    {
        yLine += rowHeightForVisibleEntry(i);
        if (yLine <= bounds.getY() || yLine >= bounds.getBottom())
        {
            continue;
        }

        g.drawHorizontalLine(yLine, (float)bounds.getX(), (float)hx);
    }
}

int TrackLanesView::yForVisibleInsertGapK(const int k) const noexcept
{
    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (k < 0 || k > vr)
    {
        return 0;
    }
    const int ay = getLocalBounds().getY();
    int y = ay - verticalScrollOffsetPx_;
    for (int i = 0; i < k; ++i)
    {
        y += rowHeightForVisibleEntry(i);
    }
    return y;
}

void TrackLanesView::paintOverChildren(juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    if (!bounds.isEmpty())
    {
        const int headerW = juce::jmin(kTrackHeaderWidth, bounds.getWidth());
        if (headerW > 0 && headerW < bounds.getWidth())
        {
            const float vx = (float)(bounds.getX() + headerW) - 0.5f;
            g.setColour(
                juce::Colour(kArrangementSeparatorArgb).withAlpha(kArrangementSeparatorAlphaVertical));
            g.drawLine(vx, (float)bounds.getY(), vx, (float)bounds.getBottom(), 1.0f);
        }

        paintHeaderColumnHorizontalRowSeparators(g);
    }

    if (!headerTrackDragActive_ || headerTrackDragInvalidArea_)
    {
        return;
    }
    const int h = getHeight();
    if (h <= 0)
    {
        return;
    }
    int yy = 0;
    if (headerTrackDragNoop_)
    {
        if (headerTrackDragNoopLineY_ < 0)
        {
            return;
        }
        g.setColour(juce::Colour(0xffc04040));
        yy = juce::jlimit(0, juce::jmax(0, h - 2), headerTrackDragNoopLineY_ - 1);
    }
    else
    {
        if (headerTrackDragInsertGapK_ < 0)
        {
            return;
        }
        g.setColour(juce::Colour(0xff40c040));
        const int y = yForVisibleInsertGapK(headerTrackDragInsertGapK_);
        yy = juce::jlimit(0, juce::jmax(0, h - 2), y - 1);
    }
    const int lineW = juce::jmin(kTrackHeaderWidth, getWidth());
    g.fillRect(0, yy, lineW, 2);
}

void TrackLanesView::mouseWheelMove(
    const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    for (const auto& lane : lanes_)
    {
        if (lane != nullptr && lane->isTimelineEditGestureInProgress())
        {
            return;
        }
    }
    const double d = (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);
    if (d == 0.0)
    {
        return;
    }
    if (e.mods.isCtrlDown())
    {
        const std::int64_t arr = session_.getArrangementExtentSamples();
        if (arr <= 0)
        {
            return;
        }
        const double spp = timelineViewport_.getSamplesPerPixel();
        if (spp <= 0.0)
        {
            return;
        }
        if (e.position.x < (float)kTrackHeaderWidth)
        {
            return;
        }
        const int tw = juce::jmax(0, getWidth() - kTrackHeaderWidth);
        if (tw <= 0)
        {
            return;
        }
        const double w = (double)tw;
        const double x = (double)e.position.x - (double)kTrackHeaderWidth;
        const double factor = std::pow(0.85, d);
        const double sppMax
            = juce::jmax(1.0, (double)juce::jmax(std::int64_t{1}, arr) / w);
        timelineViewport_.zoomAroundSample(factor, x, w, arr, kSppMin, sppMax);
        repaint();
        return;
    }
    if (e.mods.isShiftDown())
    {
        const std::int64_t arr = session_.getArrangementExtentSamples();
        if (arr <= 0)
        {
            return;
        }
        const double spp = timelineViewport_.getSamplesPerPixel();
        if (spp <= 0.0)
        {
            return;
        }
        const int twPan = juce::jmax(0, getWidth() - kTrackHeaderWidth);
        if (twPan <= 0)
        {
            return;
        }
        const double wPan = (double)twPan;
        const double panNotchPx = juce::jmax(1.0, wPan / 8.0);
        const std::int64_t step = (d > 0.0) ? (std::int64_t)std::llround(panNotchPx * spp)
                                           : -((std::int64_t)std::llround(panNotchPx * spp));
        if (step == 0)
        {
            return;
        }
        timelineViewport_.panBySamples(step, wPan, arr);
        repaint();
        return;
    }

    if (visibleTrackEntries_.empty())
    {
        return;
    }
    const int deltaPx = (int)std::llround(d * (double)defaultRowHeightPx_ * 0.5);
    if (deltaPx == 0)
    {
        return;
    }
    setVerticalScrollOffsetPx(verticalScrollOffsetPx_ + deltaPx);
}

void TrackLanesView::notifyPlacedClipRemoved(const TrackId trackId, const PlacedClipId clipId) noexcept
{
    if (aggregatedSelectedPlacedClip_.has_value()
        && aggregatedSelectedPlacedClip_->first == trackId
        && aggregatedSelectedPlacedClip_->second == clipId)
    {
        aggregatedSelectedPlacedClip_.reset();
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() == trackId)
        {
            u->clearSelectionOnly();
            break;
        }
    }
}

void TrackLanesView::onLanePlacedClipSelectionChanged(const TrackId laneTrackId,
                                                      const std::optional<PlacedClipId> id) noexcept
{
    if (id.has_value())
    {
        aggregatedSelectedPlacedClip_ = std::pair<TrackId, PlacedClipId>(laneTrackId, *id);
    }
    else if (aggregatedSelectedPlacedClip_.has_value()
             && aggregatedSelectedPlacedClip_->first == laneTrackId)
    {
        aggregatedSelectedPlacedClip_.reset();
    }
}

std::optional<std::pair<TrackId, PlacedClipId>> TrackLanesView::getAggregatedSelectedClip()
    const noexcept
{
    return aggregatedSelectedPlacedClip_;
}

std::optional<std::pair<TrackId, std::vector<InstrumentMidiClipId>>>
TrackLanesView::getAggregatedSelectedInstrumentMidiClipSelection() const noexcept
{
    const auto pickForTrack = [this](const TrackId tid)
        -> std::optional<std::pair<TrackId, std::vector<InstrumentMidiClipId>>> {
        if (tid == kInvalidTrackId)
        {
            return std::nullopt;
        }
        const auto it = instrumentTimelineAttachments_.find(tid);
        if (it == instrumentTimelineAttachments_.end() || it->second.controller == nullptr)
        {
            return std::nullopt;
        }
        const auto& sel = it->second.controller->getSelectedClipIds();
        if (sel.empty())
        {
            return std::nullopt;
        }
        return std::pair<TrackId, std::vector<InstrumentMidiClipId>>(tid,
                                                                     std::vector<InstrumentMidiClipId>(
                                                                         sel.begin(),
                                                                         sel.end()));
    };

    const TrackId active = session_.getActiveTrackId();
    if (auto fromActive = pickForTrack(active))
    {
        return fromActive;
    }
    for (const auto& kv : instrumentTimelineAttachments_)
    {
        if (auto r = pickForTrack(kv.first))
        {
            return r;
        }
    }
    return std::nullopt;
}

void TrackLanesView::selectFrontPlacedClipOnTrack(const TrackId tid) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int tIdx = snap->findTrackIndexById(tid);
    if (tIdx < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(tIdx);
    if (tr.getNumPlacedClips() <= 0)
    {
        return;
    }
    const PlacedClipId pid = tr.getPlacedClip(0).getId();
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() != tid)
        {
            u->clearSelectionOnly();
        }
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() == tid)
        {
            u->applyExternalPlacedClipSelection(pid);
            break;
        }
    }
}

void TrackLanesView::selectPlacedClipOnTrack(const TrackId tid, const PlacedClipId clipId) noexcept
{
    if (tid == kInvalidTrackId || clipId == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int tIdx = snap->findTrackIndexById(tid);
    if (tIdx < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(tIdx);
    bool found = false;
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        if (tr.getPlacedClip(i).getId() == clipId)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        return;
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() != tid)
        {
            u->clearSelectionOnly();
        }
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() == tid)
        {
            u->applyExternalPlacedClipSelection(clipId);
            break;
        }
    }
}