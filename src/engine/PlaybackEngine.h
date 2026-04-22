#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

class Session;
class Transport;

// Sole AudioIODeviceCallback: reads Session clip + Transport playhead; fills device output.
class PlaybackEngine : public juce::AudioIODeviceCallback
{
public:
    PlaybackEngine(Transport& transport, Session& session);
    ~PlaybackEngine() override;

    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    PlaybackEngine(PlaybackEngine&&) = delete;
    PlaybackEngine& operator=(PlaybackEngine&&) = delete;

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    Transport& transport_;
    Session& session_;
};
