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
             const bool trackMuted,
             const TrackKind kind,
             const float stereoPan,
             const TrackId routedOutputTrackId,
             std::vector<TrackSend> sends) noexcept
    : id_(id)
    , name_(std::move(name))
    , placedClips_(std::move(placedClips))
    , channelFaderGain_(juce::jlimit(0.0f, kTrackChannelFaderGainMax, channelFaderGain))
    , trackOff_(trackOff)
    , trackMuted_(trackMuted)
    , kind_(kind)
    , stereoPan_(sanitizeTrackStereoPan(stereoPan))
    , routedOutputTrackId_(routedOutputTrackId)
    , sends_(std::move(sends))
{
    jassert(id_ != kInvalidTrackId);
    for (TrackSend& s : sends_)
    {
        s.amountLinear = clampTrackSendAmountLinear(s.amountLinear);
    }
}

Track::Track(const TrackId id, juce::String name, std::vector<PlacedClip> placedClips) noexcept
    : Track(id, std::move(name), std::move(placedClips), kTrackChannelVolumeUnityGain)
{}

int Track::getNumSends() const noexcept
{
    return static_cast<int>(sends_.size());
}

const TrackSend& Track::getSend(const int index) const
{
    jassert(index >= 0 && index < getNumSends());
    return sends_.at((size_t)index);
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

Track Track::renamed(juce::String newName) const noexcept
{
    return Track(id_,
                 std::move(newName),
                 placedClips_,
                 channelFaderGain_,
                 trackOff_,
                 trackMuted_,
                 kind_,
                 stereoPan_,
                 routedOutputTrackId_,
                 sends_);
}
