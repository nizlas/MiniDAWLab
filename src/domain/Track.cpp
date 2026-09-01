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
             std::vector<TrackSend> sends,
             const int midiOutputChannel,
             const TrackId midiDestinationTrackId) noexcept
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
    , midiOutputChannel_(sanitizeTrackMidiOutputChannel(midiOutputChannel))
    , midiDestinationTrackId_(midiDestinationTrackId)
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
    return withName(std::move(newName));
}

// Each helper copies *this whole* (the compiler-generated copy constructor copies every field,
// including any added later) and overwrites exactly one field, re-applying the same sanitation the
// constructor applies. A field added to Track can therefore never be dropped by a mutation path.

Track Track::withName(juce::String name) const noexcept
{
    Track out(*this);
    out.name_ = std::move(name);
    return out;
}

Track Track::withPlacedClips(std::vector<PlacedClip> clips) const noexcept
{
    Track out(*this);
    out.placedClips_ = std::move(clips);
    return out;
}

Track Track::withChannelFaderGain(const float gain) const noexcept
{
    Track out(*this);
    out.channelFaderGain_ = juce::jlimit(0.0f, kTrackChannelFaderGainMax, gain);
    return out;
}

Track Track::withTrackOff(const bool off) const noexcept
{
    Track out(*this);
    out.trackOff_ = off;
    return out;
}

Track Track::withMuted(const bool muted) const noexcept
{
    Track out(*this);
    out.trackMuted_ = muted;
    return out;
}

Track Track::withKind(const TrackKind kind) const noexcept
{
    Track out(*this);
    out.kind_ = kind;
    return out;
}

Track Track::withStereoPan(const float pan) const noexcept
{
    Track out(*this);
    out.stereoPan_ = sanitizeTrackStereoPan(pan);
    return out;
}

Track Track::withRoutedOutputTrackId(const TrackId dest) const noexcept
{
    Track out(*this);
    out.routedOutputTrackId_ = dest;
    return out;
}

Track Track::withSends(std::vector<TrackSend> sends) const noexcept
{
    Track out(*this);
    out.sends_ = std::move(sends);
    for (TrackSend& s : out.sends_)
    {
        s.amountLinear = clampTrackSendAmountLinear(s.amountLinear);
    }
    return out;
}

Track Track::withMidiOutputChannel(const int channel) const noexcept
{
    Track out(*this);
    out.midiOutputChannel_ = sanitizeTrackMidiOutputChannel(channel);
    return out;
}

Track Track::withMidiDestinationTrackId(const TrackId dest) const noexcept
{
    Track out(*this);
    out.midiDestinationTrackId_ = dest;
    return out;
}
