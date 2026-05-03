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
#include "domain/PlacedClip.h"
#include "transport/Transport.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    constexpr double kSppMin = 0.1;
    // Match `ClipWaveformView` playhead refresh so preview drain + repaints stay in the same ballpark.
    constexpr int kRecordingPreviewTimerHz = 20;

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
    LatencySettingsStore& latencySettingsStore)
    : session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
    , deviceManager_(deviceManager)
    , recorder_(recorder)
    , latencyStore_(latencySettingsStore)
{
    syncTracksFromSession();
    startTimerHz(kRecordingPreviewTimerHz);
}

TrackLanesView::~TrackLanesView()
{
    stopTimer();
    clearCycleRecordingPreviewContext();
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

void TrackLanesView::rebuildChildLanesIfNeeded()
{
    const int n = session_.getNumTracks();
    if (n <= 0)
    {
        headers_.clear();
        lanes_.clear();
        aggregatedSelectedPlacedClip_.reset();
        return;
    }
    bool need = ((int)lanes_.size() != n || (int)headers_.size() != n);
    if (!need)
    {
        for (int i = 0; i < n; ++i)
        {
            if (lanes_[(size_t)i]->getTrackId() != session_.getTrackIdAtIndex(i))
            {
                need = true;
                break;
            }
        }
    }
    if (!need)
    {
        return;
    }
    headers_.clear();
    lanes_.clear();
    aggregatedSelectedPlacedClip_.reset();
    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
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
        auto head = std::make_unique<TrackHeaderView>(
            session_,
            recorder_,
            transport_,
            tid,
            [this] { repaint(); },
            onArm,
            std::move(onDelete),
            trackHeaderPluginHost_,
            std::move(dragHost));
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
        auto ptr = std::make_unique<ClipWaveformView>(session_, transport_, tid, timelineViewport_, std::move(host));
        addAndMakeVisible(*ptr);
        lanes_.push_back(std::move(ptr));
    }
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
    auto area = getLocalBounds();
    const int n = (int)lanes_.size();
    if (n <= 0 || area.getHeight() <= 0)
    {
        return;
    }
    const int totalH = area.getHeight();
    const int w = area.getWidth();
    const int leftW = juce::jmin(kTrackHeaderWidth, w);
    int y = 0;
    for (int i = 0; i < n; ++i)
    {
        const int hh = (i == n - 1) ? (totalH - y) : juce::jmax(1, totalH / n);
        juce::Rectangle row(area.getX(), y, w, hh);
        headers_[(size_t)i]->setBounds(row.removeFromLeft(leftW));
        lanes_[(size_t)i]->setBounds(row);
        y += hh;
    }
    const int tw = juce::jmax(0, getWidth() - kTrackHeaderWidth);
    if (tw > 0)
    {
        timelineViewport_.clampToExtent((double)tw, session_.getArrangementExtentSamples());
    }
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
    headerTrackDragDestIndex_ = -1;
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
        headerTrackDragDestIndex_ = -1;
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

    const int n = (int)lanes_.size();
    if (n <= 0)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int s = snap->findTrackIndexById(movedId);
    if (s < 0)
    {
        headerTrackDragInvalidArea_ = true;
        headerTrackDragInsertGapK_ = -1;
        headerTrackDragNoopLineY_ = -1;
        if (headerTrackDragSourceView_ != nullptr)
        {
            headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
        }
        repaint();
        return;
    }

    std::vector<int> gapY;
    gapY.resize((size_t)(n + 1));
    gapY[0] = 0;
    for (int i = 0; i < n; ++i)
    {
        const int h = lanes_[(size_t)i]->getHeight();
        gapY[(size_t)(i + 1)] = gapY[(size_t)i] + h;
    }

    int bestK = 0;
    int bestAbs = 0x7ffffff;
    for (int k = 0; k <= n; ++k)
    {
        const int d = local.y - gapY[(size_t)k];
        const int a = d < 0 ? -d : d;
        if (a < bestAbs)
        {
            bestAbs = a;
            bestK = k;
        }
    }

    int dest;
    if (bestK <= s)
    {
        dest = bestK;
    }
    else
    {
        dest = bestK - 1;
    }
    const bool noop = (dest == s);
    headerTrackDragNoop_ = noop;
    headerTrackDragDestIndex_ = noop ? -1 : dest;
    if (noop)
    {
        // Red: line tracks pointer (valid header column only). Green path uses snapped `bestK` below.
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

    const bool commit
        = (!headerTrackDragInvalidArea_) && !headerTrackDragNoop_ && (headerTrackDragDestIndex_ >= 0);
    if (commit)
    {
        session_.moveTrack(movedId, headerTrackDragDestIndex_);
        {
            const int tw = juce::jmax(0, getWidth() - kTrackHeaderWidth);
            if (tw > 0)
            {
                timelineViewport_.clampToExtent((double)tw, session_.getArrangementExtentSamples());
            }
        }
        syncTracksFromSession();
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
    headerTrackDragDestIndex_ = -1;
}

int TrackLanesView::yForInsertGapK(const int k) const noexcept
{
    const int n = (int)lanes_.size();
    if (k < 0 || k > n)
    {
        return 0;
    }
    int y = 0;
    for (int i = 0; i < k; ++i)
    {
        y += lanes_[(size_t)i]->getHeight();
    }
    return y;
}

void TrackLanesView::paintOverChildren(juce::Graphics& g)
{
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
        const int y = yForInsertGapK(headerTrackDragInsertGapK_);
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
    const double d = (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);
    if (d == 0.0)
    {
        return;
    }
    if (e.mods.isCtrlDown())
    {
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