// =============================================================================
// MonoWavFileWriter.cpp — chunked write via JUCE AudioFormatWriter (mono 24-bit)
// =============================================================================

#include "io/MonoWavFileWriter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr int kBitsPerSample = 24;
constexpr int kChunk = 65536;

juce::WavAudioFormat&
getWav()
{
    static juce::WavAudioFormat w;
    return w;
}

} // namespace

juce::Result MonoWavFileWriter::writeMono24BitWavSegment(const juce::File& outputFile,
                                                        const float* samples,
                                                        int numSamples,
                                                        const double sampleRate)
{
    if (outputFile == juce::File())
        return juce::Result::fail("Invalid WAV path");
    if (samples == nullptr || numSamples <= 0)
        return juce::Result::fail("No samples to write");
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return juce::Result::fail("Invalid sample rate");

    auto out = std::make_unique<juce::FileOutputStream>(outputFile);
    if (out->failedToOpen())
        return juce::Result::fail("Could not open file for writing: " + outputFile.getFullPathName());

    std::unique_ptr<juce::AudioFormatWriter> wr(getWav().createWriterFor(out.release(),
                                                                           sampleRate,
                                                                           1u,
                                                                           kBitsPerSample,
                                                                           juce::StringPairArray(),
                                                                           0));
    if (wr == nullptr || wr->getBitsPerSample() != kBitsPerSample)
        return juce::Result::fail("Could not create 24-bit WAV writer");

    juce::AudioBuffer<float> scratch(1, kChunk);
    scratch.clear();
    const float* ptr = samples;
    int remaining = numSamples;

    while (remaining > 0)
    {
        const int chunk = juce::jmin(kChunk, remaining);
        scratch.copyFrom(0, 0, ptr, chunk);
        if (!wr->writeFromAudioSampleBuffer(scratch, 0, chunk))
            return juce::Result::fail("Disk write failed while writing WAV slice");
        ptr += chunk;
        remaining -= chunk;
    }

    if (!wr->flush())
        return juce::Result::fail("WAV flush failed");

    return juce::Result::ok();
}
