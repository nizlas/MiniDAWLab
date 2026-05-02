// =============================================================================
// Track.cpp  —  one lane of placed clips (immutable value inside `SessionSnapshot`)
// =============================================================================

#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <utility>

Track::Track(const TrackId id,
             juce::String name,
             std::vector<PlacedClip> placedClips,
             const float channelFaderGain,
             const bool trackOff,
             const bool trackMuted) noexcept
    : id_(id)
    , name_(std::move(name))
    , placedClips_(std::move(placedClips))
    , channelFaderGain_(juce::jlimit(0.0f, kTrackChannelFaderGainMax, channelFaderGain))
    , trackOff_(trackOff)
    , trackMuted_(trackMuted)
{
    jassert(id_ != kInvalidTrackId);
}

Track::Track(const TrackId id, juce::String name, std::vector<PlacedClip> placedClips) noexcept
    : Track(id, std::move(name), std::move(placedClips), kTrackChannelVolumeUnityGain)
{}

int Track::getNumPlacedClips() const noexcept
{
    return static_cast<int>(placedClips_.size());
}

const PlacedClip& Track::getPlacedClip(const int index) const
{
    jassert(index >= 0 && index < getNumPlacedClips());
    return placedClips_.at((size_t)index);
}
