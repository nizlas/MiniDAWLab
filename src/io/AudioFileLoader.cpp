#include "io/AudioFileLoader.h"

#include "domain/AudioClip.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <limits>

juce::Result AudioFileLoader::loadFromFile(const juce::File& file,
                                           double requiredDeviceSampleRate,
                                           std::unique_ptr<AudioClip>& outClip)
{
    outClip.reset();

    if (!file.existsAsFile())
        return juce::Result::fail("File does not exist.");

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return juce::Result::fail("Could not read audio file (unsupported format or I/O error).");

    if (!juce::approximatelyEqual(reader->sampleRate, requiredDeviceSampleRate))
    {
        return juce::Result::fail(
            "Audio file sample rate ("
            + juce::String(reader->sampleRate, 2)
            + " Hz) does not match the audio device ("
            + juce::String(requiredDeviceSampleRate, 2)
            + " Hz). Phase 1 does not resample; use a matching file or change the device rate.");
    }

    const int numChannels = static_cast<int>(reader->numChannels);
    if (numChannels <= 0)
        return juce::Result::fail("Audio file has no channels.");

    const juce::int64 lengthInSamples = reader->lengthInSamples;
    if (lengthInSamples <= 0)
        return juce::Result::fail("Audio file contains no samples.");

    if (lengthInSamples > static_cast<juce::int64>(std::numeric_limits<int>::max()))
        return juce::Result::fail("Audio file is too large for Phase 1 in-memory decode.");

    const int numSamples = static_cast<int>(lengthInSamples);
    juce::AudioBuffer<float> buffer(numChannels, numSamples);

    if (!reader->read(buffer.getArrayOfWritePointers(), numChannels, 0, numSamples))
        return juce::Result::fail("Failed to read sample data from file.");

    outClip = std::make_unique<AudioClip>(
        std::move(buffer),
        reader->sampleRate,
        file.getFullPathName());

    return juce::Result::ok();
}
