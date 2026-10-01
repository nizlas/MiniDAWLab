#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
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

/// Level statistics of the rendered export, measured on the exporter's float stereo block **after
/// the complete master strip and before any file conversion** (the same point the live Stereo Out
/// meter measures). Sample peak — not true / inter-sample peak. `overs` = samples with |x| > 1.0:
/// the float mix exceeded 0 dBFS there; a 16/24-bit WAV is hard-clipped at those samples by the
/// writer, a float WAV keeps them, and an MP3 overshoots on decode. Returned to the caller so a
/// transient overload stays visible in the export result after any animated meter has fallen.
struct MixdownExportLevelReport
{
    float peak[2] = { 0.0f, 0.0f };
    std::uint32_t overs[2] = { 0, 0 };
    double rms[2] = { 0.0, 0.0 };
    /// Mean sample value per channel: a constant offset wastes headroom and clicks at file edges.
    double dcOffset[2] = { 0.0, 0.0 };
    std::uint32_t nonFinite = 0;
    std::int64_t frames = 0;
    float firstSample[2] = { 0.0f, 0.0f };
    float lastSample[2] = { 0.0f, 0.0f };
    bool valid = false;

    [[nodiscard]] bool hasOverload() const noexcept { return overs[0] + overs[1] > 0 || nonFinite > 0; }
    /// |DC| above −40 dBFS (0.01): large enough to eat headroom and click at the file edges.
    static constexpr double kSignificantDcOffset = 0.01;
    [[nodiscard]] bool hasSignificantDcOffset() const noexcept
    {
        return std::abs(dcOffset[0]) > kSignificantDcOffset || std::abs(dcOffset[1]) > kSignificantDcOffset;
    }
    /// "Peak L -1.2 dBFS, R -0.8 dBFS" plus explicit overload / DC-offset lines when they apply.
    [[nodiscard]] juce::String summaryText() const;
};

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
    /// Optional: receives the rendered float output's level statistics (filled on success).
    MixdownExportLevelReport* levelReportOut = nullptr;
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
    bool overwriteConfirmed = false,
    MixdownExportLevelReport* levelReportOut = nullptr);

} // namespace mini_daw_audio_mixdown
