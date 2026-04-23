#pragma once

// =============================================================================
// AudioFileLoader  —  file path + device sample rate  →  AudioClip (import only, no app context)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   The “import” step in ARCHITECTURE_PRINCIPLES: turn bytes on disk into an in-memory
//   juce::AudioBuffer inside AudioClip. It does not know about Session, Transport, playback,
//   or windows. That keeps file format quirks in one place.
//
// WHERE IT RUNS
//   [Message thread] only, Phase 1: synchronous, blocking decode. Callers must not invoke from
//   the audio callback.
//
// WHAT IT IS NOT
//   Not a long-lived service — static methods, no singleton. Not responsible for resampling
//   (Phase 1: reject mismatch), not responsible for multiple clips, not responsible for async I/O.
//
// See: AudioClip (output), Session::replaceClipFromFile (typical caller).
//
// AudioFileLoader.cpp walks each decision (missing file, format, rate mismatch, size limits) in
// plain language next to the JUCE API calls; that is the pedagogical map for *why* a load failed.
// =============================================================================

#include <juce_core/juce_core.h>

#include <memory>

class AudioClip;

class AudioFileLoader
{
public:
    // [Message thread] Full synchronous decode. On success outClip is replaced; on failure
    // outClip is reset and the Result holds the error string for UI.
    static juce::Result loadFromFile(const juce::File& file,
                                     double requiredDeviceSampleRate,
                                     std::unique_ptr<AudioClip>& outClip);

private:
    AudioFileLoader() = delete;
};
