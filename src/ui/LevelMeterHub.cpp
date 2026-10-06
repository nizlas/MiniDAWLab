#include "ui/LevelMeterHub.h"

#include <algorithm>

LevelMeterHub::LevelMeterHub()
{
    startTimerHz(kTickHz);
}

LevelMeterHub::~LevelMeterHub()
{
    stopTimer();
    // Nothing is metered once the hub is gone (the engine keeps its Stereo Out window regardless).
    if (hooks_.setMeteredTracks != nullptr && !publishedInterest_.empty())
    {
        (void)hooks_.setMeteredTracks({});
    }
}

void LevelMeterHub::setEngineHooks(EngineHooks hooks)
{
    hooks_ = std::move(hooks);
    publishedInterest_.clear();
    publishInterestIfChanged();
}

void LevelMeterHub::addListener(Listener* const l)
{
    if (l != nullptr && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end())
    {
        listeners_.push_back(l);
        publishInterestIfChanged();
    }
}

void LevelMeterHub::removeListener(Listener* const l)
{
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
    if (!inTick_)
    {
        publishInterestIfChanged();
    }
}

void LevelMeterHub::acknowledgeOverload(const TrackId trackId)
{
    // Copy: a listener may remove itself while handling the acknowledgement.
    const std::vector<Listener*> copy = listeners_;
    for (Listener* l : copy)
    {
        if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end())
        {
            l->meterOverloadAcknowledged(trackId);
        }
    }
}

void LevelMeterHub::refreshInterestNow()
{
    publishInterestIfChanged();
}

void LevelMeterHub::publishInterestIfChanged()
{
    scratchInterest_.clear();
    for (Listener* l : listeners_)
    {
        l->collectMeterInterest(scratchInterest_);
    }
    // Normalize: drop invalid ids and duplicates; the Master row is drained separately and must
    // never claim a bank slot (its level is the Stereo Out window).
    const TrackId master = hooks_.masterTrackId != nullptr ? hooks_.masterTrackId() : kInvalidTrackId;
    scratchInterest_.erase(std::remove_if(scratchInterest_.begin(), scratchInterest_.end(),
                                          [master](const TrackId id) { return id == kInvalidTrackId || id == master; }),
                           scratchInterest_.end());
    std::sort(scratchInterest_.begin(), scratchInterest_.end());
    scratchInterest_.erase(std::unique(scratchInterest_.begin(), scratchInterest_.end()), scratchInterest_.end());
    if (scratchInterest_ == publishedInterest_)
    {
        return;
    }
    publishedInterest_ = scratchInterest_;
    if (hooks_.setMeteredTracks != nullptr)
    {
        lastOverflow_ = hooks_.setMeteredTracks(publishedInterest_);
    }
}

void LevelMeterHub::timerCallback()
{
    inTick_ = true;
    publishInterestIfChanged();
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // One drain per row, then the same window to every listener — no listener drains anything.
    if (hooks_.drainTrack != nullptr)
    {
        for (const TrackId id : publishedInterest_)
        {
            const level_meter::Reading r = hooks_.drainTrack(id);
            if (!r.hasSignalData())
            {
                continue;
            }
            for (Listener* l : listeners_)
            {
                l->meterWindowArrived(id, r, now);
            }
        }
    }
    // The Stereo Out window is always drained (export diagnostics and the Master row read it),
    // and delivered under the Master row's id when the session has one.
    if (hooks_.drainMaster != nullptr)
    {
        const level_meter::Reading m = hooks_.drainMaster();
        const TrackId master = hooks_.masterTrackId != nullptr ? hooks_.masterTrackId() : kInvalidTrackId;
        if (m.hasSignalData() && master != kInvalidTrackId)
        {
            for (Listener* l : listeners_)
            {
                l->meterWindowArrived(master, m, now);
            }
        }
    }
    for (Listener* l : listeners_)
    {
        l->meterTick(now);
    }
    inTick_ = false;
}
