#pragma once

// =============================================================================
// ProxyRenderTypes — production P1D result/status/tail/cancellation vocabulary
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §13, §15.2, §15.5, §15.6)
// =============================================================================
// Shared by the deterministic render executor (ProxyRenderExecutor.h), the
// message-thread instance lifecycle (ProxyRenderInstanceLifecycle.h) and the
// selftests. Header-only, depends on juce_core + juce_audio_basics only, so the
// deterministic selftests exercise every policy decision without plugin hosting.
//
// P1D scope: the renderer RETURNS a structured result; it never publishes into
// the project (P1F owns publication) and never updates persisted proxy metadata.

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace proxy_render
{

//==============================================================================
// Tail policy v1 (Locked, steering revision 6, PID-005; fingerprinted via F12)
//==============================================================================
inline constexpr double kTailThresholdDb = -70.0; ///< absolute per-block peak threshold (X)
inline constexpr double kTailSilenceWindowSec = 1.0; ///< continuous silence window (Y)
inline constexpr double kTailMaxSec = 30.0; ///< maximum tail after the final relevant event (Z)
inline constexpr int kTailPolicyVersion = 1;

/// Locked render block/format policy (steering §15.4/§15.5, revision 6).
inline constexpr int kRenderBlockSize = 512;
inline constexpr int kRenderChannels = 2; ///< proxy v1 asset is stereo (32-bit-float WAV)

[[nodiscard]] inline double dbToLinear(const double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

//==============================================================================
// Cooperative cancellation (P1E seam; §13.3 / PI-013)
//==============================================================================
/// Shared flag checked by the render worker at every block boundary. Requesting
/// cancellation is safe from any thread; the worker stops promptly, the job
/// returns Cancelled (never Failed), temporary output is cleaned up, and the
/// live plugin instance is never touched by any part of the cancellation path.
class ProxyRenderCancellationToken final
{
public:
    ProxyRenderCancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void requestCancel() const noexcept { flag_->store(true, std::memory_order_release); }
    [[nodiscard]] bool isCancelled() const noexcept
    {
        return flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

//==============================================================================
// Structured result (§13.2 job vocabulary, P1D subset)
//==============================================================================
enum class ProxyRenderStatus
{
    Succeeded,          ///< complete render, validated temporary WAV available
    SucceededSilent,    ///< §15.7 explicit silent generation — no WAV by design
    Cancelled,          ///< cooperative cancellation; temp output cleaned up
    Failed              ///< see failureReason; never publishable
};

enum class ProxyRenderFailureReason
{
    None,
    SnapshotInvalid,        ///< unusable render request (no destination/config)
    PluginCreationFailed,   ///< isolated instance could not be created
    StateRestoreFailed,     ///< setStateInformation on the isolated instance threw
    PrepareFailed,          ///< bus layout / prepareToPlay failed for the render config
    TailLimitReached,       ///< §15.2: cap hit with materially non-silent output — diagnosed
                            ///< incomplete render; NEVER published, never "complete"
    NonFiniteAudio,         ///< the isolated instance produced NaN/Inf samples
    WavWriteFailed,         ///< temporary WAV could not be created/written
    WavValidationFailed     ///< §8 post-write validation rejected the artifact
};

[[nodiscard]] inline const char* toString(const ProxyRenderStatus s) noexcept
{
    switch (s)
    {
        case ProxyRenderStatus::Succeeded: return "Succeeded";
        case ProxyRenderStatus::SucceededSilent: return "SucceededSilent";
        case ProxyRenderStatus::Cancelled: return "Cancelled";
        case ProxyRenderStatus::Failed: return "Failed";
    }
    return "?";
}

[[nodiscard]] inline const char* toString(const ProxyRenderFailureReason r) noexcept
{
    switch (r)
    {
        case ProxyRenderFailureReason::None: return "None";
        case ProxyRenderFailureReason::SnapshotInvalid: return "SnapshotInvalid";
        case ProxyRenderFailureReason::PluginCreationFailed: return "PluginCreationFailed";
        case ProxyRenderFailureReason::StateRestoreFailed: return "StateRestoreFailed";
        case ProxyRenderFailureReason::PrepareFailed: return "PrepareFailed";
        case ProxyRenderFailureReason::TailLimitReached: return "TailLimitReached";
        case ProxyRenderFailureReason::NonFiniteAudio: return "NonFiniteAudio";
        case ProxyRenderFailureReason::WavWriteFailed: return "WavWriteFailed";
        case ProxyRenderFailureReason::WavValidationFailed: return "WavValidationFailed";
    }
    return "?";
}

/// MIDI delivery tallies of what the executor actually handed to processBlock —
/// the integration test's proof that routed channels and CC11 reached the clone.
struct ProxyRenderMidiTallies
{
    std::int64_t noteOnsByChannel[16] = {}; ///< index 0 = MIDI channel 1
    std::int64_t noteOffsByChannel[16] = {};
    std::int64_t ccByController[128] = {};
    std::int64_t totalEvents = 0;
};

struct ProxyRenderResult
{
    ProxyRenderStatus status = ProxyRenderStatus::Failed;
    ProxyRenderFailureReason failureReason = ProxyRenderFailureReason::None;
    juce::String message;

    // Identity echo of the captured request (§8 validation: result ↔ request pairing).
    juce::String expectedFingerprint;
    std::uint64_t primarySemanticRevision = 0;

    // Render configuration actually used.
    double renderSampleRate = 0.0;
    int blockSize = 0;
    int channels = kRenderChannels;

    // §7: reported plugin latency is preserved (never trimmed/shifted) and recorded.
    int pluginLatencySamplesAtStart = -1;
    int pluginLatencySamplesAtEnd = -1;

    // §6 span/tail outcome, all in render-rate samples.
    std::int64_t spanEndRenderSamples = 0;   ///< last relevant event (converted at the boundary)
    std::int64_t renderedLengthSamples = 0;  ///< asset length = spanEnd + accepted tail
    std::int64_t tailLengthSamples = 0;      ///< accepted tail (includes the silence window)
    bool tailCompleted = false;

    // Signal integrity.
    bool allFinite = true;
    double maxPeakLinear = 0.0;
    std::uint64_t blocksProcessed = 0;
    std::uint64_t blocksWithMidi = 0;
    ProxyRenderMidiTallies midi;

    // Temporary artifact (empty for SucceededSilent / cleaned-up failures).
    juce::File temporaryWavFile;
    std::int64_t wavBytes = 0;

    // Evidence for thread-affinity assertions in tests/integration logging.
    juce::String workerThreadId;
    double wallMs = 0.0;
};

//==============================================================================
// Tail detector (single locked policy; the SPIKE-02 candidate-grid evaluator is
// superseded by this production detector)
//==============================================================================
/// Feed per-block peaks AFTER the span end. Decision semantics (§15.2):
///   * a continuous run of blocks whose peak stays below the threshold, spanning
///     at least the silence window, completes the tail — the asset ends when the
///     accepted tail completes (the window itself is part of the asset, §15.6);
///   * reaching the maximum tail while output remains material ⇒ Failed
///     ("tail limit reached — render incomplete"); never published.
class ProxyTailDetector final
{
public:
    ProxyTailDetector(const double sampleRate,
                      const double thresholdDb = kTailThresholdDb,
                      const double windowSec = kTailSilenceWindowSec,
                      const double maxTailSec = kTailMaxSec) noexcept
        : thresholdLinear_(dbToLinear(thresholdDb)),
          windowSamples_((std::int64_t)std::llround(windowSec * sampleRate)),
          maxTailSamples_((std::int64_t)std::llround(maxTailSec * sampleRate))
    {
    }

    enum class Verdict
    {
        Continue,      ///< keep rendering tail blocks
        TailComplete,  ///< silence window satisfied — stop; asset ends here
        CapReached     ///< max tail consumed with material output ⇒ Failed
    };

    /// One post-span block of `numSamples` with absolute per-block `peakLinear`.
    [[nodiscard]] Verdict feedBlock(const double peakLinear, const int numSamples) noexcept
    {
        tailSamples_ += numSamples;
        if (peakLinear < thresholdLinear_)
        {
            silentRunSamples_ += numSamples;
            if (silentRunSamples_ >= windowSamples_)
            {
                return Verdict::TailComplete;
            }
        }
        else
        {
            silentRunSamples_ = 0;
        }
        if (tailSamples_ >= maxTailSamples_)
        {
            // Cap hit. Only a cap landing exactly inside an already-satisfied window could
            // complete; otherwise output was material within the last window ⇒ incomplete.
            return silentRunSamples_ >= windowSamples_ ? Verdict::TailComplete
                                                       : Verdict::CapReached;
        }
        return Verdict::Continue;
    }

    [[nodiscard]] std::int64_t tailSamplesConsumed() const noexcept { return tailSamples_; }

private:
    const double thresholdLinear_;
    const std::int64_t windowSamples_;
    const std::int64_t maxTailSamples_;
    std::int64_t tailSamples_ = 0;
    std::int64_t silentRunSamples_ = 0;
};

//==============================================================================
// RAII temporary-file guard (§8: cancellation/exception/failure never leaks temp files)
//==============================================================================
class ScopedTempFileGuard final
{
public:
    explicit ScopedTempFileGuard(juce::File f) noexcept : file_(std::move(f)) {}

    ~ScopedTempFileGuard()
    {
        if (!released_ && file_ != juce::File())
        {
            (void)file_.deleteFile();
        }
    }

    /// Success path: the caller takes ownership (the validated temp WAV in the result).
    juce::File release() noexcept
    {
        released_ = true;
        return file_;
    }

    [[nodiscard]] const juce::File& file() const noexcept { return file_; }

    JUCE_DECLARE_NON_COPYABLE(ScopedTempFileGuard)

private:
    juce::File file_;
    bool released_ = false;
};

} // namespace proxy_render
