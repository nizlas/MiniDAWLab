#include "engine/PlaybackEngine.h"

#include "domain/Session.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

PlaybackEngine::PlaybackEngine(Transport& transport, Session& session)
    : transport_(transport)
    , session_(session)
{
}

PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    juce::ignoreUnused(device);
}

void PlaybackEngine::audioDeviceStopped() {}

void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                      int numInputChannels,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

    transport_.audioThread_beginBlock();

    const auto clip = session_.loadClipForAudioThread();
    const auto intent = transport_.audioThread_loadIntent();
    const std::int64_t playhead = transport_.audioThread_loadPlayhead();

    const int clipLen = clip != nullptr ? clip->getNumSamples() : 0;

    int framesToAdvance = 0;
    if (clip != nullptr && intent == PlaybackIntent::Playing && playhead < clipLen && numSamples > 0)
    {
        const std::int64_t remaining =
            static_cast<std::int64_t>(clipLen) - playhead;
        framesToAdvance =
            static_cast<int>(juce::jmin(static_cast<std::int64_t>(numSamples), remaining));
    }

    for (int out = 0; out < numOutputChannels; ++out)
    {
        float* const dest = outputChannelData[out];
        if (dest == nullptr)
            continue;

        if (clip == nullptr || framesToAdvance <= 0)
        {
            juce::FloatVectorOperations::clear(dest, numSamples);
            continue;
        }

        const int clipChannels = clip->getNumChannels();
        const int offset = static_cast<int>(playhead);
        const bool monoToFirstTwoOuts = (clipChannels == 1 && numOutputChannels >= 2
                                         && (out == 0 || out == 1));

        if (monoToFirstTwoOuts)
        {
            const float* const src = clip->getAudio().getReadPointer(0);
            juce::FloatVectorOperations::copy(dest, src + offset, framesToAdvance);
            if (framesToAdvance < numSamples)
                juce::FloatVectorOperations::clear(dest + framesToAdvance,
                                                 numSamples - framesToAdvance);
        }
        else if (out < clipChannels)
        {
            const float* const src = clip->getAudio().getReadPointer(out);
            juce::FloatVectorOperations::copy(dest, src + offset, framesToAdvance);
            if (framesToAdvance < numSamples)
                juce::FloatVectorOperations::clear(dest + framesToAdvance,
                                                 numSamples - framesToAdvance);
        }
        else
        {
            juce::FloatVectorOperations::clear(dest, numSamples);
        }
    }

    transport_.audioThread_advancePlayheadIfPlaying(
        static_cast<std::int64_t>(framesToAdvance));
}
