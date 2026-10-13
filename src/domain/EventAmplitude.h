#pragma once

// =============================================================================
// EventAmplitude — per-placed-audio-clip gain and linear fade-in / fade-out
// =============================================================================
// Pure functions shared by the session model, the audio renderers and the lane
// overlay. No allocation. Fade lengths are samples measured from the current
// event edges. A fade-in is always 0 → 100 % and a fade-out is always 100 → 0 %.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace event_amplitude
{

inline constexpr float kGainDbMin = -60.0f;
inline constexpr float kGainDbMax = 24.0f;
inline constexpr float kGainDbDefault = 0.0f;

struct Params
{
    float gainDb = kGainDbDefault;
    std::int64_t fadeInSamples = 0;
    std::int64_t fadeOutSamples = 0;
};

[[nodiscard]] inline float sanitizeGainDb(const float db) noexcept
{
    if (!std::isfinite(db))
    {
        return kGainDbDefault;
    }
    return std::clamp(db, kGainDbMin, kGainDbMax);
}

[[nodiscard]] inline std::int64_t sanitizeFadeSamples(const std::int64_t samples) noexcept
{
    return samples > 0 ? samples : 0;
}

/// Proportional floor when the two fades no longer fit. The sum is never greater
/// than `length`. Each fade stays a full 0 → 100 % or 100 → 0 % ramp.
[[nodiscard]] inline Params fitToLength(const float gainDb,
                                        const std::int64_t fadeInSamples,
                                        const std::int64_t fadeOutSamples,
                                        const std::int64_t lengthSamples) noexcept
{
    Params out;
    out.gainDb = sanitizeGainDb(gainDb);
    out.fadeInSamples = sanitizeFadeSamples(fadeInSamples);
    out.fadeOutSamples = sanitizeFadeSamples(fadeOutSamples);
    if (lengthSamples <= 0)
    {
        out.fadeInSamples = 0;
        out.fadeOutSamples = 0;
        return out;
    }
    if (out.fadeInSamples > lengthSamples)
    {
        out.fadeInSamples = lengthSamples;
    }
    if (out.fadeOutSamples > lengthSamples)
    {
        out.fadeOutSamples = lengthSamples;
    }
    const std::int64_t sum = out.fadeInSamples + out.fadeOutSamples;
    if (sum > lengthSamples && sum > 0)
    {
        const double scale = static_cast<double>(lengthSamples) / static_cast<double>(sum);
        out.fadeInSamples = static_cast<std::int64_t>(std::floor(static_cast<double>(out.fadeInSamples) * scale));
        out.fadeOutSamples = static_cast<std::int64_t>(std::floor(static_cast<double>(out.fadeOutSamples) * scale));
        if (out.fadeInSamples + out.fadeOutSamples > lengthSamples)
        {
            const std::int64_t over = out.fadeInSamples + out.fadeOutSamples - lengthSamples;
            if (out.fadeOutSamples >= over)
            {
                out.fadeOutSamples -= over;
            }
            else
            {
                out.fadeInSamples = std::max<std::int64_t>(0, out.fadeInSamples - (over - out.fadeOutSamples));
                out.fadeOutSamples = 0;
            }
        }
    }
    return out;
}

/// Left half of a split: the original fade-in, limited to this part. No fade-out
/// at the new cut. Gain is copied.
[[nodiscard]] inline Params splitLeft(const Params& source, const std::int64_t leftLength) noexcept
{
    const std::int64_t length = std::max<std::int64_t>(0, leftLength);
    return fitToLength(source.gainDb, std::min(source.fadeInSamples, length), 0, length);
}

/// Right half of a split: the original fade-out, limited to this part. No fade-in
/// at the new cut. Gain is copied.
[[nodiscard]] inline Params splitRight(const Params& source, const std::int64_t rightLength) noexcept
{
    const std::int64_t length = std::max<std::int64_t>(0, rightLength);
    return fitToLength(source.gainDb, 0, std::min(source.fadeOutSamples, length), length);
}

/// 0 dB is exactly 1. −60 dB is a real gain, not silence-by-convention.
[[nodiscard]] inline float linearGain(const float gainDb) noexcept
{
    const float db = sanitizeGainDb(gainDb);
    if (db == 0.0f)
    {
        return 1.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

/// Linear amplitude envelope at event-local sample `rel` in [0, length).
/// Fade-in: `rel / fadeIn` while `rel < fadeIn` (the sample at `fadeIn` is 100 %).
/// Fade-out: `(length - rel) / fadeOut` while `rel >= length - fadeOut`
/// (the sample at the fade-out start is 100 %; the ramp reaches 0 at the right edge).
[[nodiscard]] inline float envelopeAt(const std::int64_t rel,
                                      const std::int64_t length,
                                      const std::int64_t fadeInSamples,
                                      const std::int64_t fadeOutSamples) noexcept
{
    if (length <= 0 || rel < 0 || rel >= length)
    {
        return 0.0f;
    }
    const Params fitted = fitToLength(kGainDbDefault, fadeInSamples, fadeOutSamples, length);
    if (fitted.fadeInSamples > 0 && rel < fitted.fadeInSamples)
    {
        return static_cast<float>(rel) / static_cast<float>(fitted.fadeInSamples);
    }
    if (fitted.fadeOutSamples > 0 && rel >= length - fitted.fadeOutSamples)
    {
        return static_cast<float>(length - rel) / static_cast<float>(fitted.fadeOutSamples);
    }
    return 1.0f;
}

[[nodiscard]] inline float amplitudeAt(const std::int64_t rel,
                                       const std::int64_t length,
                                       const float gainDb,
                                       const std::int64_t fadeInSamples,
                                       const std::int64_t fadeOutSamples) noexcept
{
    return linearGain(gainDb) * envelopeAt(rel, length, fadeInSamples, fadeOutSamples);
}

/// True when every sample of [rel, rel + run) sits in the full-level region
/// (or there is no fade). A constant event gain may then be folded once.
[[nodiscard]] inline bool isConstantOverRun(const std::int64_t rel,
                                            const int run,
                                            const std::int64_t length,
                                            const std::int64_t fadeInSamples,
                                            const std::int64_t fadeOutSamples) noexcept
{
    if (run <= 0)
    {
        return true;
    }
    const Params fitted = fitToLength(kGainDbDefault, fadeInSamples, fadeOutSamples, length);
    if (fitted.fadeInSamples <= 0 && fitted.fadeOutSamples <= 0)
    {
        return true;
    }
    const std::int64_t sustainBegin = fitted.fadeInSamples;
    const std::int64_t sustainEnd = length - fitted.fadeOutSamples;
    return rel >= sustainBegin && (rel + static_cast<std::int64_t>(run)) <= sustainEnd;
}

} // namespace event_amplitude
