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

/// [Message thread] Progress feedback during the blocking export. `statusText` names the current
/// phase ("Rendering... 42%", "Encoding MP3... 80%", "Finalizing..."); `fraction01` is 0..1 for
/// determinate progress and negative for an indeterminate phase. The exporter calls both methods
/// from inside its blocking loops (render blocks, encoder wait), so an implementation must paint
/// synchronously and may service only its own window's input there — the export owns the message
/// thread and no other DAL code may run until it returns (see `AudioMixdownProgressWindow`).
class MixdownProgressSink
{
public:
    virtual ~MixdownProgressSink() = default;
    virtual void setMixdownProgress(const juce::String& statusText, double fraction01) = 0;
    /// Cooperative cancel: polled between render blocks and during encoding. A true return makes
    /// the exporter stop, remove its own working files and report "Export cancelled." — the
    /// destination file is never touched on that path. Default: never cancels (headless callers).
    [[nodiscard]] virtual bool isMixdownCancelRequested() const noexcept { return false; }
};

/// True when `result` is the exporter's cooperative-cancel outcome (not an error).
[[nodiscard]] bool isMixdownCancelledResult(const juce::Result& result) noexcept;

struct MixdownExportRequest
{
    juce::File outputFile;
    double sampleRate = 0.0;
    MixdownWaveBits bits = MixdownWaveBits::Pcm24;
    /// Optional live progress feedback (non-owning; must outlive the blocking export call).
    MixdownProgressSink* progressSink = nullptr;
    /// True when the caller already asked the user about overwriting `outputFile` (skips the
    /// exporter's own overwrite prompt).
    bool overwriteConfirmed = false;
    /// True when this WAV is only the intermediate step of a larger export (MP3): the render phase
    /// still reports "Rendering... NN%", but the "Finalizing..." phase is left to the outer export.
    bool isIntermediateStep = false;
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

/// MP3 mixdown: renders the active loop to a **private working WAV in the system temp folder**
/// (32-bit float or 24-bit PCM), encodes it with the bundled LAME into a sibling working MP3 next
/// to the destination, and only then replaces the destination. Bitrate must be one of
/// 128, 160, 192, 224, 256, 320. Every exit path — success, error, timeout, cancel — removes the
/// working WAV and the working MP3; the user's export folder receives exactly the final MP3.
/// Only files this export created are ever deleted (unique tagged names), never a pre-existing
/// user file that happens to share the base name.
[[nodiscard]] juce::Result exportStereoMixdownMp3Blocking(
    Transport& transport,
    Session& session,
    PlaybackEngine& playbackEngine,
    juce::AudioDeviceManager& deviceManager,
    const std::function<void()>& syncTransportUiFromDomain,
    const juce::File& mp3OutputFile,
    int bitrateKbps,
    MixdownProgressSink* progressSink = nullptr,
    bool overwriteConfirmed = false);

} // namespace mini_daw_audio_mixdown
