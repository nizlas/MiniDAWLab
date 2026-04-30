#pragma once

// =============================================================================
// MonoWavFileWriter — offline mono 24-bit PCM WAV segments (same convention as RecorderService)
// =============================================================================
//
// MESSAGE THREAD ONLY. Used after cycle OD stops: slice float PCM from decoded buffer to
// independent take files — not called from realtime audio callbacks.
//
// =============================================================================

#include <juce_core/juce_core.h>

namespace MonoWavFileWriter
{

// Writes [samples, samples + numSamples) as mono **24-bit** PCM WAV at `sampleRate`.
[[nodiscard]] juce::Result writeMono24BitWavSegment(const juce::File& outputFile,
                                                  const float* samples,
                                                  int numSamples,
                                                  double sampleRate);

} // namespace MonoWavFileWriter
