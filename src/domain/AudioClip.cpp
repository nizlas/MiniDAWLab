#include "domain/AudioClip.h"

AudioClip::AudioClip(juce::AudioBuffer<float> samples,
                     double sourceSampleRate,
                     juce::String sourceDescription)
    : samples_(std::move(samples))
    , sourceSampleRate_(sourceSampleRate)
    , sourceDescription_(std::move(sourceDescription))
{
}
