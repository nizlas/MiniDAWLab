// =============================================================================
// AudioClip.cpp  —  constructor only; all behavior is in getters in the header
// =============================================================================
//
// After this constructor returns, the clip is immutable: Session and PlaybackEngine treat it
// as read-only shared data; edits always mean “replace with a new AudioClip from the loader”.
// =============================================================================

#include "domain/AudioClip.h"

AudioClip::AudioClip(juce::AudioBuffer<float> samples,
                     double sourceSampleRate,
                     juce::String sourceDescription)
    : samples_(std::move(samples))
    , sourceSampleRate_(sourceSampleRate)
    , sourceDescription_(std::move(sourceDescription))
{
}
