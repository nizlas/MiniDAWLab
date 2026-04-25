// =============================================================================
// TrackLanesView.cpp  —  one lane view per `Track` (message thread)
// =============================================================================

#include "ui/TrackLanesView.h"

#include "ui/ClipWaveformView.h"
#include "ui/TrackHeaderView.h"
#include "domain/Session.h"
#include "domain/Track.h"
#include "transport/Transport.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <utility>

TrackLanesView::TrackLanesView(Session& session, Transport& transport)
    : session_(session)
    , transport_(transport)
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
        auto head = std::make_unique<TrackHeaderView>(session_, tid, [this] { repaint(); });
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
        auto ptr = std::make_unique<ClipWaveformView>(session_, transport_, tid, std::move(host));
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
}