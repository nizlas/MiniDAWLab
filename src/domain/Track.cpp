// =============================================================================
// Track.cpp  —  one lane of placed clips (immutable value inside `SessionSnapshot`)
// =============================================================================

#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <utility>

Track::Track(const TrackId id, std::vector<PlacedClip> placedClips) noexcept
    : id_(id)
    , placedClips_(std::move(placedClips))
{
    jassert(id_ != kInvalidTrackId);
}

int Track::getNumPlacedClips() const noexcept
{
    return static_cast<int>(placedClips_.size());
}

const PlacedClip& Track::getPlacedClip(const int index) const
{
    jassert(index >= 0 && index < getNumPlacedClips());
    return placedClips_.at((size_t)index);
}
