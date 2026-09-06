#pragma once

// =============================================================================
// TimelineDomain — explicit conversion between persisted timeline-reference samples and
// engine/render samples (TLD-1, steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §10.1)
// =============================================================================
// The persisted timeline reference sample rate (ProjectFileV1::timelineSampleRate, held by
// Session) and the current engine/device sample rate are SEPARATE coordinate domains: one defines
// what the stored sample integers *mean*, the other what the running engine *needs*. A sample
// count created at rate A must never be interpreted unchanged as a sample count at rate B —
// every boundary that needs engine- or render-rate positions converts through these helpers
// (`engineSamples = round(storedSamples * engineRate / referenceRate)`).
//
// Current consumers of the persisted reference domain (P1B/P1C):
//   * InstrumentTrackController::setTimelineSampleRate — the tick→sample bake and derived clip
//     lengths interpret timeline content at the session reference rate;
//   * the P1C proxy render snapshot (renderConfig.timelineReferenceRate) — sample-domain clip
//     anchors/windows convert to the render rate with convertSampleCount below.
// Engine consumers that still read stored integers 1:1 as device samples (playhead, locators,
// audio-clip placement, mix scheduling, waveform/UI mapping) are enumerated in §10.1 and remain
// assigned to a later blocking slice before P1J cross-rate acceptance — deliberately NOT converted
// piecemeal here.
// =============================================================================

#include <cmath>
#include <cstdint>

namespace timeline_domain
{
    /// True when `rate` is usable as a coordinate-domain rate.
    [[nodiscard]] inline bool isValidRate(const double rate) noexcept
    {
        return rate > 0.0 && std::isfinite(rate);
    }

    /// Convert a sample position/count from one rate domain to another:
    /// `round(samples * toRate / fromRate)`. Identity (bit-exact, no rounding) when the rates are
    /// equal. Returns `samples` unchanged when either rate is invalid — callers validate rates at
    /// the boundary; this keeps the helper total without inventing data.
    [[nodiscard]] inline std::int64_t convertSampleCount(const std::int64_t samples,
                                                         const double fromRate,
                                                         const double toRate) noexcept
    {
        if (!isValidRate(fromRate) || !isValidRate(toRate))
        {
            return samples;
        }
        if (fromRate == toRate)
        {
            return samples;
        }
        return (std::int64_t)std::llround((double)samples * (toRate / fromRate));
    }

    /// Persisted timeline-reference domain → current engine domain.
    [[nodiscard]] inline std::int64_t referenceToEngineSamples(const std::int64_t referenceSamples,
                                                               const double timelineReferenceRate,
                                                               const double engineRate) noexcept
    {
        return convertSampleCount(referenceSamples, timelineReferenceRate, engineRate);
    }

    /// Current engine domain → persisted timeline-reference domain.
    [[nodiscard]] inline std::int64_t engineToReferenceSamples(const std::int64_t engineSamples,
                                                               const double engineRate,
                                                               const double timelineReferenceRate) noexcept
    {
        return convertSampleCount(engineSamples, engineRate, timelineReferenceRate);
    }
} // namespace timeline_domain
