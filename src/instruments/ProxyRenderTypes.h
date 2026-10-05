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

//==============================================================================
// Instrument readiness verification (1.1.14; steering §14.2 step 4b)
//==============================================================================
// Why: a restored render instance may still be loading its content asynchronously when
// `prepareToPlay` returns (measured: Groove Agent SE restores its state synchronously but
// streams its kit samples in the background for ~1–3 s; a render started immediately after
// prepare produced digital silence for the first notes — the whole asset in a Release build,
// the first 6–12 s of material in a Debug build — and that silence was published as Current).
// Neither the plug-in's state bytes (volatile, growing every call) nor a fixed sleep is a
// readiness signal, so readiness is established from the instrument's RESPONSE: the content's
// own distinct notes are played into the prepared instance, one at a time, and the set of notes
// that produce output is compared across passes separated by a wall-clock interval. The render
// starts when that set has stopped growing (the instrument answers consistently), the stimulus
// is flushed with All Sound Off / All Notes Off first. Every bound below is a cap on waiting,
// never a correctness input; an instrument that answers at once is verified in two passes.
struct ProxyReadinessPolicy
{
    bool enabled = true;
    /// Material processed per stimulus note before deciding whether it sounds.
    double stimulusSeconds = 0.25;
    /// Material (at most) processed before each stimulus note until the previous one has
    /// decayed under the tail threshold, so the rise criterion judges the new note alone.
    double settleSeconds = 2.0;
    /// Output above this (and ≥ 12 dB above the residual before the note) counts as sounding.
    double floorDb = -90.0;
    /// Wall-clock wait between passes (the worker sleeps; the message thread keeps running).
    int passIntervalMs = 250;
    /// Consecutive passes in which no NEW sounding note appeared ⇒ the response has settled
    /// (3 × 250 ms = 750 ms of unchanged response; a progressively loading instrument keeps
    /// adding notes until its content is in).
    int settledPasses = 3;
    /// No note has sounded for this long ⇒ stop verifying (the render proceeds; the no-output
    /// rule below then decides on the actual artifact).
    int maxSilentWaitMs = 20000;
    /// Absolute bound of the verification phase (response still changing ⇒ proceed, unverified).
    int maxTotalWaitMs = 60000;
    /// Material processed after All Sound Off until the output is back under the tail threshold.
    double flushMaxSeconds = 12.0;
    int maxStimulusNotes = 48;
};

/// Readiness verification outcome recorded in the result (diagnostics; never fingerprinted).
struct ProxyReadinessOutcome
{
    bool attempted = false;
    bool verified = false;          ///< sounding response settled before the render
    int passes = 0;
    int stimulusNotes = 0;
    int soundingNotes = 0;          ///< in the final pass
    double waitMs = 0.0;            ///< wall-clock spent (processing + sleeps)
    std::uint64_t blocksProcessed = 0;
    double flushResidualDb = -200.0; ///< peak of the last flush block before the render
    bool flushReachedSilence = true;
    juce::String note;              ///< human-readable summary
    /// Per pass, per stimulus note: "residual->peak" in dBFS (diagnostics; bounded length).
    juce::String trace;
};

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
    WavValidationFailed,    ///< §8 post-write validation rejected the artifact
    NoAudibleOutput         ///< notes were scheduled but nothing above the tail threshold was
                            ///< ever produced (instrument not ready, muted or unsupported
                            ///< content) — an incomplete render, never published as Current
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
        case ProxyRenderFailureReason::NoAudibleOutput: return "NoAudibleOutput";
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

    /// Instrument readiness verification before the block loop (1.1.14, §14.2 step 4b).
    ProxyReadinessOutcome readiness;

    /// PI-011 diagnostic evidence (set by the production engine at prepare time): the isolated
    /// render instance pointer differed from the live audio-thread instance pointer. Value-only —
    /// no plugin instance is ever exposed through scheduler/status APIs.
    bool renderInstanceDistinctFromLive = false;
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
