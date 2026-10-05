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
// Tail policy v2 (steering §15.2; fingerprinted via F12 — v1 = revision 6 values on the
// absolute peak, v2 adds DC tracking; a v1 generation stays comparable, see §16.1 / currency)
//==============================================================================
inline constexpr double kTailThresholdDb = -70.0; ///< per-block peak threshold (X), on the DC-tracked signal
inline constexpr double kTailSilenceWindowSec = 1.0; ///< continuous silence window (Y)
inline constexpr double kTailMaxSec = 30.0; ///< maximum tail after the final relevant event (Z)
/// v2: the detector judges x − dc(x), where dc is a one-pole running estimate with this time
/// constant (corner 1/(2πτ) ≈ 0.32 Hz). A constant offset parked on the output (measured: VB3-II
/// holds −0.605 after any note) therefore cannot hold the tail open, while musical content down
/// to a few Hz passes essentially unattenuated (5 Hz: −0.02 dB; 1 Hz: −0.4 dB). A step in the
/// offset shows up as a transient that decays with τ (−70 dBFS after ≈ 7.9 τ for a full-scale
/// step), so the tail after the last event is bounded by max(real decay, ≈ 4 s) + the window.
inline constexpr double kTailDcTimeConstantSec = 0.5;
inline constexpr int kTailPolicyVersion = 2;

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
    double flushResidualDb = -200.0; ///< DC-tracked residual peak of the last flush block (the judged value)
    double flushRawPeakDb = -200.0;  ///< raw |x| peak of the same block (a parked offset shows here)
    double parkedDcAtRenderStart[2] = {}; ///< offset estimate per channel handed to the render
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
    double maxPeakLinear = 0.0;          ///< raw |x| peak over the recorded boundary (diagnostics)
    /// Tail policy v2 measurement over the whole render: peak of the DC-tracked residual. This is
    /// the value the plausibility rule judges (a constant offset contributes nothing to it).
    double maxResidualPeakLinear = 0.0;
    /// Offset estimate per channel when the asset ends, and the boundary samples (diagnostics /
    /// boundary-handling input: a parked offset at the asset's edges is what a player steps from).
    double dcEstimateAtEnd[2] = {};
    float firstSample[2] = {};
    float lastSample[2] = {};
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
// DC-tracking peak meter (tail policy v2 measurement; shared by the tail detector, the
// readiness verification and the plausibility check)
//==============================================================================
/// Per channel: a one-pole running offset estimate `dc += a · (x − dc)` with time constant τ
/// (`a = 1 − e^(−1/(τ·fs))`) and the peak of the residual `x − dc`. State is continuous across
/// blocks and independent of the block size; the only block-size effect anywhere in the tail
/// policy is the decision granularity (one block). Non-finite samples are skipped here (the
/// executor fails the render on them separately). Two channels are measured independently and
/// the block verdict takes the LOUDER one — never a mid-sum, so anti-phase content between L
/// and R, or an offset on one channel only, is judged as it sounds.
class DcTrackingPeakMeter final
{
public:
    static constexpr int kMaxChannels = 2;

    DcTrackingPeakMeter(const double sampleRate, const double dcTimeConstantSec = kTailDcTimeConstantSec) noexcept
        : coefficient_(sampleRate > 0.0 && dcTimeConstantSec > 0.0
                           ? 1.0 - std::exp(-1.0 / (dcTimeConstantSec * sampleRate))
                           : 1.0)
    {
    }

    struct BlockReading
    {
        double residualPeak = 0.0;      ///< max |x − dc| over channels and samples (the judged value)
        double rawPeak = 0.0;           ///< max |x| (diagnostics)
        double dcAtEnd[kMaxChannels] = {}; ///< offset estimate per channel after the block
    };

    /// Feed one block of up to two channels; the per-channel filter state advances.
    BlockReading feedBlock(const float* const* channels, const int numChannels, const int numSamples) noexcept
    {
        BlockReading r;
        const int n = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        for (int c = 0; c < n; ++c)
        {
            const float* d = channels[c];
            double dc = dc_[c];
            if (d != nullptr)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    const double x = (double)d[i];
                    if (!std::isfinite(x))
                    {
                        continue;
                    }
                    dc += coefficient_ * (x - dc);
                    const double residual = std::abs(x - dc);
                    const double raw = std::abs(x);
                    r.residualPeak = residual > r.residualPeak ? residual : r.residualPeak;
                    r.rawPeak = raw > r.rawPeak ? raw : r.rawPeak;
                }
            }
            dc_[c] = dc;
            r.dcAtEnd[c] = dc;
        }
        return r;
    }

    /// Seed the offset estimates with a KNOWN parked offset (the readiness verification hands the
    /// instrument's current offset to the render, so the asset's first block does not look like
    /// a step the detector then has to let decay).
    void seed(const double left, const double right) noexcept
    {
        dc_[0] = std::isfinite(left) ? left : 0.0;
        dc_[1] = std::isfinite(right) ? right : 0.0;
    }

    [[nodiscard]] double dcEstimate(const int channel) const noexcept
    {
        return channel >= 0 && channel < kMaxChannels ? dc_[channel] : 0.0;
    }

private:
    const double coefficient_;
    double dc_[kMaxChannels] = {};
};

//==============================================================================
// Tail detector (tail policy v2: the DC-tracked residual, per channel; the SPIKE-02
// candidate-grid evaluator is superseded by this production detector)
//==============================================================================
/// Observe EVERY rendered block (the offset estimate must track the music before the tail
/// phase begins); the tail decision is evaluated only for blocks fed with `inTailPhase`.
/// Decision semantics (§15.2):
///   * a continuous run of tail blocks whose residual peak stays below the threshold, spanning
///     at least the silence window, completes the tail — the asset ends when the accepted
///     tail completes (the window itself is part of the asset, §15.6);
///   * reaching the maximum tail while the residual remains material ⇒ Failed
///     ("tail limit reached — render incomplete"); never published.
/// A constant offset alone can never hold the tail open; a step in the offset is a transient
/// that decays with the meter's time constant and is judged like any other decaying content.
class ProxyTailDetector final
{
public:
    ProxyTailDetector(const double sampleRate,
                      const double thresholdDb = kTailThresholdDb,
                      const double windowSec = kTailSilenceWindowSec,
                      const double maxTailSec = kTailMaxSec,
                      const double dcTimeConstantSec = kTailDcTimeConstantSec) noexcept
        : meter_(sampleRate, dcTimeConstantSec),
          thresholdLinear_(dbToLinear(thresholdDb)),
          windowSamples_((std::int64_t)std::llround(windowSec * sampleRate)),
          maxTailSamples_((std::int64_t)std::llround(maxTailSec * sampleRate))
    {
    }

    enum class Verdict
    {
        Continue,      ///< keep rendering (music phase, or tail blocks still material)
        TailComplete,  ///< silence window satisfied — stop; asset ends here
        CapReached     ///< max tail consumed with material residual ⇒ Failed
    };

    /// One block of the recorded stereo boundary.
    [[nodiscard]] Verdict feedBlock(const float* const* channels, const int numChannels,
                                    const int numSamples, const bool inTailPhase) noexcept
    {
        last_ = meter_.feedBlock(channels, numChannels, numSamples);
        if (!inTailPhase)
        {
            return Verdict::Continue;
        }
        tailSamples_ += numSamples;
        if (judgedPeak() < thresholdLinear_)
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
            // complete; otherwise the residual was material within the last window ⇒ incomplete.
            return silentRunSamples_ >= windowSamples_ ? Verdict::TailComplete
                                                       : Verdict::CapReached;
        }
        return Verdict::Continue;
    }

    [[nodiscard]] std::int64_t tailSamplesConsumed() const noexcept { return tailSamples_; }
    [[nodiscard]] const DcTrackingPeakMeter::BlockReading& lastReading() const noexcept { return last_; }
    [[nodiscard]] double dcEstimate(const int channel) const noexcept { return meter_.dcEstimate(channel); }
    /// Known parked offset at the render's first sample (see DcTrackingPeakMeter::seed).
    void seedDcEstimate(const double left, const double right) noexcept { meter_.seed(left, right); }
    /// DIAGNOSTICS ONLY: judge the raw |x| peak instead of the DC-tracked residual (tail policy v1
    /// behaviour, to reproduce a pre-v2 failure on the same material). Never used in production.
    void setJudgeRawPeakForDiagnostics(const bool judgeRaw) noexcept { judgeRaw_ = judgeRaw; }

private:
    [[nodiscard]] double judgedPeak() const noexcept { return judgeRaw_ ? last_.rawPeak : last_.residualPeak; }

    DcTrackingPeakMeter meter_;
    DcTrackingPeakMeter::BlockReading last_;
    bool judgeRaw_ = false;
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
