#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

[[nodiscard]] inline bool probeStereoFloatWavSupportedMixdown(const double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        return false;
    }
    juce::WavAudioFormat wav;
    auto mos = std::make_unique<juce::MemoryOutputStream>();
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(
        mos.release(), sampleRate, 2u, 32, juce::StringPairArray(), 0));
    return writer != nullptr && writer->isFloatingPoint();
}
