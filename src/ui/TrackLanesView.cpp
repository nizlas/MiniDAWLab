// =============================================================================
// TrackLanesView.cpp  —  one lane view per `Track` (message thread)
// =============================================================================

#include "ui/TrackLanesView.h"

#include "ui/ClipWaveformView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
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
} // namespace

TrackLanesView::TrackLanesView(
    Session& session,
    Transport& transport,
    TimelineViewportModel& timelineViewport,
    juce::AudioDeviceManager& deviceManager)
    : session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
    , deviceManager_(deviceManager)
{
    syncTracksFromSession();
}

TrackLanesView::~TrackLanesView() = default;

void TrackLanesView::syncTracksFromSession()
{
    rebuildChildLanesIfNeeded();
    resized();
}

void TrackLanesView::rebuildChildLanesIfNeeded()
{
    const int n = session_.getNumTracks();
    if (n <= 0)
    {
        headers_.clear();
        lanes_.clear();
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
        auto head
            = std::make_unique<TrackHeaderView>(session_, tid, [this] { repaint(); }, std::move(dragHost));
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