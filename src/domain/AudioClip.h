#pragma once

// =============================================================================
// AudioClip  —  decoded PCM + metadata; immutable after construction
// =============================================================================
//
// ROLE
//   A value object: one contiguous float buffer per channel, sample count, and a text label
//   (e.g. file path) for error messages. Session publishes a const `SessionSnapshot`; PlaybackEngine
//   reads clip material through Session. Phases that add “clip on a timeline with an offset” will add placement
//   *outside* this type; AudioClip is only the audio *material*.
//
// THREADING
//   Not synchronized inside — const after construction. Safe to read from any thread if you
//   hold a const reference or shared_ptr to const AudioClip that outlives the read. Session’s
//   published snapshot carries that shared_ptr on the right threads.
//
// NOT RESPONSIBLE FOR
//   Device sample rate policy (loader enforces match), playhead, or routing.
//
// AudioClip.cpp is intentionally a one-line constructor: there is no branch logic; the in-body
// story for “what this contains” lives in the file header and in the loader that constructs it.
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

class AudioClip
{
public:
    // [Message thread, during load] Builds the in-memory representation; moves samples in.
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
