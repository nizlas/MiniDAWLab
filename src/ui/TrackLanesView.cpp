// =============================================================================
// TrackLanesView.cpp  —  one lane view per `Track` (message thread)
// =============================================================================

#include "ui/TrackLanesView.h"

#include "ui/ClipWaveformView.h"
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
        lanes_.clear();
        return;
    }
    bool need = ((int)lanes_.size() != n);
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
    lanes_.clear();
    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
        if (tid == kInvalidTrackId)
        {
            jassert(false);
            continue;
        }
        auto ptr = std::make_unique<ClipWaveformView>(session_, transport_, tid, [this](ClipWaveformView& sender) {
            for (auto& u : lanes_)
            {
                if (u.get() != &sender)
                {
                    u->clearSelectionOnly();
                }
            }
        });
        addAndMakeVisible(*ptr);
        lanes_.push_back(std::move(ptr));
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
    int y = 0;
    for (int i = 0; i < n; ++i)
    {
        const int hh = (i == n - 1) ? (totalH - y) : juce::jmax(1, totalH / n);
        lanes_[(size_t)i]->setBounds(area.getX(), y, area.getWidth(), hh);
        y += hh;
    }
}