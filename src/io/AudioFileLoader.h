#pragma once

#include <juce_core/juce_core.h>

#include <memory>

class AudioClip;

// Turns a file into a fully decoded in-memory clip. No transport, session, engine, or UI knowledge.
// Call only from the message thread; load is synchronous (Phase 1).
class AudioFileLoader
{
public:
    // On success, assigns a new AudioClip to outClip. On failure, outClip is cleared.
    static juce::Result loadFromFile(const juce::File& file,
                                     double requiredDeviceSampleRate,
                                     std::unique_ptr<AudioClip>& outClip);

private:
    AudioFileLoader() = delete;
};
