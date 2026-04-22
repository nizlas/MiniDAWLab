#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Single decoded clip: full in-memory buffer, fixed channel layout (no upmix/downmix).
class AudioClip
{
public:
    AudioClip(juce::AudioBuffer<float> samples,
              double sourceSampleRate,
              juce::String sourceDescription);

    const juce::AudioBuffer<float>& getAudio() const noexcept { return samples_; }

    double getSourceSampleRate() const noexcept { return sourceSampleRate_; }

    int getNumChannels() const noexcept { return samples_.getNumChannels(); }

    int getNumSamples() const noexcept { return samples_.getNumSamples(); }

    const juce::String& getSourceDescription() const noexcept { return sourceDescription_; }

private:
    juce::AudioBuffer<float> samples_;
    double sourceSampleRate_;
    juce::String sourceDescription_;
};
