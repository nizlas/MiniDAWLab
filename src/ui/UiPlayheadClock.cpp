#include "ui/UiPlayheadClock.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace
{
    /// Snap (allowing a backwards jump) when the published transport position differs this much
    /// from the smoothed value: seek, cycle wrap, or a device dropout. Matches the MIDI editor's
    /// own extrapolation threshold so both windows recover on the same events.
    constexpr double kHardResyncSamples = 8192.0;

    /// Proportional pull toward the published transport value per tick. The published value is
    /// block-quantized, so chasing it fully every tick reproduces the staircase as jitter; a small
    /// gain filters the quantization to sub-pixel drift while still correcting real clock skew
    /// within a few hundred milliseconds. 0.08/tick at 60 Hz ≈ 200 ms error half-life.
    constexpr double kDriftCorrectionGainPerTick = 0.08;

    /// Ignore an advance shorter than this (double tick in one frame) and reuse the last value, so
    /// any accidental second caller inside one frame sees the identical position.
    constexpr double kFrameCoherenceSec = 0.004;

    /// A requested seek is considered committed once the published playhead is this close to it.
    constexpr std::int64_t kSeekHoldReleaseSamples = 4096;

    [[nodiscard]] double nowSeconds() noexcept
    {
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }
} // namespace

double UiPlayheadClock::readDisplaySamples(const std::int64_t rawTransportSample,
                                           const bool playing,
                                           const double sampleRate) noexcept
{
    const double now = nowSeconds();
    const double raw = (double)rawTransportSample;
    const double sr = (sampleRate > 0.0 && std::isfinite(sampleRate)) ? sampleRate : 48000.0;

    if (seekHoldActive_)
    {
        if (std::llabs(rawTransportSample - seekHoldSample_) <= kSeekHoldReleaseSamples)
        {
            seekHoldActive_ = false;
        }
        else
        {
            const double dt = lastAdvanceWallSec_ >= 0.0 ? (now - lastAdvanceWallSec_) : 0.0;
            lastRawSample_ = rawTransportSample;
            wasPlaying_ = playing;
            if (playing)
            {
                lastValue_ += juce::jmax(0.0, dt) * sr;
            }
            lastAdvanceWallSec_ = now;
            return lastValue_;
        }
    }

    if (!initialised_ || !playing || playing != wasPlaying_)
    {
        lastValue_ = raw;
        lastAdvanceWallSec_ = now;
        lastRawSample_ = rawTransportSample;
        wasPlaying_ = playing;
        initialised_ = true;
        return lastValue_;
    }

    const double dt = juce::jmax(0.0, now - lastAdvanceWallSec_);
    if (dt < kFrameCoherenceSec && rawTransportSample == lastRawSample_)
    {
        return lastValue_;
    }

    const double predicted = lastValue_ + dt * sr;
    const double error = raw - predicted;

    double value;
    if (std::abs(error) > kHardResyncSamples)
    {
        // Real discontinuity (seek from elsewhere, cycle wrap, dropout): snap, backwards allowed.
        value = raw;
    }
    else
    {
        // Filtered tracking: advance with wall time, nudge toward the published value. Forward-only
        // so block quantization can never pull the line visibly backwards.
        value = predicted + kDriftCorrectionGainPerTick * error;
        value = juce::jmax(value, lastValue_);
    }

    lastValue_ = value;
    lastAdvanceWallSec_ = now;
    lastRawSample_ = rawTransportSample;
    wasPlaying_ = playing;
    return value;
}

void UiPlayheadClock::reanchorTo(const std::int64_t sample) noexcept
{
    lastValue_ = (double)sample;
    lastAdvanceWallSec_ = nowSeconds();
    initialised_ = true;
    seekHoldActive_ = true;
    seekHoldSample_ = sample;
}
