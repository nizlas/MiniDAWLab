#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace juce
{
class AudioDeviceManager;
}

class PlaybackEngine;
class Session;
class Transport;

namespace mini_daw_audio_mixdown
{

enum class MixdownWaveBits : int
{
    Pcm16 = 16,
    Pcm24 = 24,
    IeeeFloat32 = 32,
};

struct MixdownExportRequest
{
    juce::File outputFile;
    double sampleRate = 0.0;
    MixdownWaveBits bits = MixdownWaveBits::Pcm24;
};

/// Half-open timeline span **[startSample, startSample + lengthSamples)** used for mixdown.
/// Source of truth matches realtime cycle playback: `Transport::readCycleEnabledForUi()` plus
/// `SessionSnapshot::getLeftLocatorSamples()` / `getRightLocatorSamples()` (same predicate as
/// `PlaybackEngine`: cycle armed and `R > L` and `R > 0`).
struct ActiveLoopMixdownSpan
{
    std::int64_t startSample = 0;
    std::int64_t lengthSamples = 0;
};

/// Validates active loop/cycle only — **no** fallback to arrangement extent.
[[nodiscard]] juce::Result resolveActiveLoopMixdownSpan(bool cycleEnabledFromTransport,
                                                        std::int64_t leftLocatorSamples,
                                                        std::int64_t rightLocatorSamples,
                                                        ActiveLoopMixdownSpan& out) noexcept;

/// [Message thread] Renders the **active loop range only** via `PlaybackEngine::renderOfflineMixdownBlock`
/// and writes a stereo WAV (blocked export; stops transport; gates realtime audio).
[[nodiscard]] juce::Result exportStereoMixdownWavBlocking(
    Transport& transport,
    Session& session,
    PlaybackEngine& playbackEngine,
    juce::AudioDeviceManager& deviceManager,
    const std::function<void()>& syncTransportUiFromDomain,
    const MixdownExportRequest& request);

/// Bundled encoder: `<executable_dir>/Tools/lame/lame.exe` (Windows). Non-Windows also tries `Tools/lame/lame`.
[[nodiscard]] juce::File findBundledLameExecutable() noexcept;

[[nodiscard]] bool isBundledLameEncoderAvailable() noexcept;

/// MP3 mixdown v1: render active loop to a **temporary** 32-bit float or 24-bit PCM WAV, then run bundled LAME.
/// Bitrate must be one of 128, 160, 192, 224, 256, 320.
/// On LAME failure or timeout, the temporary WAV is **kept** and its path is included in the error message for debugging.
[[nodiscard]] juce::Result exportStereoMixdownMp3Blocking(
    Transport& transport,
    Session& session,
    PlaybackEngine& playbackEngine,
    juce::AudioDeviceManager& deviceManager,
    const std::function<void()>& syncTransportUiFromDomain,
    const juce::File& mp3OutputFile,
    int bitrateKbps);

} // namespace mini_daw_audio_mixdown
