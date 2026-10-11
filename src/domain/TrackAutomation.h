#pragma once

// =============================================================================
// TrackAutomation — Channel Volume and Pan curves (stage 1)
// =============================================================================
//
// ROLE
//   The saved shape of one fader or pan, in project seconds. Playback, the lane, and a
//   punch all call the same functions in this header. Nothing here runs on a timer and
//   nothing here knows about the device block size.
//
// SHAPE
//   A segment is a quadratic in normalized time. The stored control `C` is the Bezier
//   value, not a point in the plane. A user drag clamps `C` into the endpoint range.
//   A punch split (de Casteljau) may store a control outside the new piece's endpoints;
//   evaluation must not clamp that stored control, or the curve outside the punch moves.
//
// TIME
//   Seconds from the start of the timeline. One conversion per point:
//     engineSample = llround(seconds * engineRate)
//     seconds      = engineSample / engineRate
//   Positions are never a running sum of rounded deltas. Opening a project does not
//   rewrite the stored seconds. BPM does not move these points.
// =============================================================================

#include "domain/TrackStereoPan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline constexpr int kTrackAutomationVolumeId = 1;
inline constexpr int kTrackAutomationPanId = 2;

inline constexpr double kTrackAutomationGrabUMin = 0.08;
inline constexpr double kTrackAutomationGrabUMax = 0.92;
inline constexpr double kTrackAutomationShortSegmentSeconds = 0.0001;
inline constexpr double kTrackAutomationTimeMergeSeconds = 1.0e-6;
inline constexpr double kTrackAutomationReturnRampSeconds = 0.010;
inline constexpr float kTrackAutomationValueTolerance = 1.0e-5f;

struct TrackAutomationPoint
{
    double timeSeconds = 0.0;
    float value = 0.0f;
};

struct TrackAutomationCurve
{
    float anchor = 0.0f;
    bool anchorSet = false;
    std::vector<TrackAutomationPoint> points;
    /// `control[i]` is `C` for the segment `points[i] → points[i + 1]`.
    std::vector<float> control;
};

struct TrackAutomationState
{
    bool read = false;
    bool volumeLaneVisible = false;
    bool panLaneVisible = false;
    TrackAutomationCurve volume;
    TrackAutomationCurve pan;
};

[[nodiscard]] inline float sanitizeTrackAutomationValue(const int parameterId, const float value) noexcept
{
    if (!std::isfinite(value))
    {
        return parameterId == kTrackAutomationPanId ? 0.0f : 1.0f;
    }
    if (parameterId == kTrackAutomationPanId)
    {
        return sanitizeTrackStereoPan(value);
    }
    return std::clamp(value, 0.0f, 8.0f);
}

[[nodiscard]] inline float trackAutomationLinearControl(const float y0, const float y1) noexcept
{
    return 0.5f * (y0 + y1);
}

[[nodiscard]] inline double trackAutomationClampGrabU(const double u) noexcept
{
    if (!std::isfinite(u))
    {
        return 0.5;
    }
    return std::clamp(u, kTrackAutomationGrabUMin, kTrackAutomationGrabUMax);
}

/// Value of the quadratic at normalized time `u` in [0, 1]. `C` is not re-clamped.
[[nodiscard]] inline float trackAutomationQuadratic(const float y0,
                                                    const float control,
                                                    const float y1,
                                                    const double u) noexcept
{
    const double om = 1.0 - u;
    return static_cast<float>(om * om * static_cast<double>(y0)
                              + 2.0 * om * u * static_cast<double>(control)
                              + u * u * static_cast<double>(y1));
}

/// Solve `C` so the curve passes through the pointer, after clamping the pointer into
/// the values a legal user control can reach at this `u`. The returned handle value is
/// `trackAutomationQuadratic` at the clamped `u`, so the handle lies on the curve.
[[nodiscard]] inline float trackAutomationControlFromGrab(const float y0,
                                                          const float y1,
                                                          double u,
                                                          const float yDesired) noexcept
{
    u = trackAutomationClampGrabU(u);
    const double chord = (1.0 - u) * static_cast<double>(y0) + u * static_cast<double>(y1);
    const double half = std::fabs(static_cast<double>(y1) - static_cast<double>(y0)) * u * (1.0 - u);
    const double y = std::clamp(static_cast<double>(yDesired), chord - half, chord + half);
    const double denom = u * (1.0 - u);
    if (denom < 1.0e-12 || y0 == y1)
    {
        return y0;
    }
    const double deviation = (y - chord) / denom;
    const double solved = (deviation + static_cast<double>(y0) + static_cast<double>(y1)) * 0.5;
    const float lo = std::min(y0, y1);
    const float hi = std::max(y0, y1);
    return std::clamp(static_cast<float>(solved), lo, hi);
}

[[nodiscard]] inline int64_t trackAutomationSecondsToEngineSample(const double seconds,
                                                                  const double engineRate) noexcept
{
    if (!(engineRate > 0.0) || !std::isfinite(seconds))
    {
        return 0;
    }
    return std::llround(seconds * engineRate);
}

[[nodiscard]] inline double trackAutomationEngineSampleToSeconds(const int64_t engineSample,
                                                                 const double engineRate) noexcept
{
    if (!(engineRate > 0.0))
    {
        return 0.0;
    }
    return static_cast<double>(engineSample) / engineRate;
}

inline void trackAutomationNormalize(TrackAutomationCurve& curve, const int parameterId) noexcept
{
    for (TrackAutomationPoint& p : curve.points)
    {
        if (!std::isfinite(p.timeSeconds) || p.timeSeconds < 0.0)
        {
            p.timeSeconds = 0.0;
        }
        p.value = sanitizeTrackAutomationValue(parameterId, p.value);
    }
    bool sorted = true;
    for (size_t i = 1; i < curve.points.size(); ++i)
    {
        if (curve.points[i].timeSeconds < curve.points[i - 1].timeSeconds)
        {
            sorted = false;
            break;
        }
    }
    if (!sorted)
    {
        std::stable_sort(curve.points.begin(), curve.points.end(),
                         [](const TrackAutomationPoint& a, const TrackAutomationPoint& b) {
                             return a.timeSeconds < b.timeSeconds;
                         });
        curve.control.clear();
    }
    if (curve.points.size() < 2)
    {
        curve.control.clear();
    }
    else
    {
        curve.control.resize(curve.points.size() - 1);
        for (size_t i = 0; i + 1 < curve.points.size(); ++i)
        {
            if (!std::isfinite(curve.control[i]))
            {
                curve.control[i] = trackAutomationLinearControl(curve.points[i].value,
                                                                curve.points[i + 1].value);
            }
        }
    }
    if (!curve.anchorSet || !std::isfinite(curve.anchor))
    {
        curve.anchor = curve.points.empty()
                           ? sanitizeTrackAutomationValue(parameterId, 0.0f)
                           : curve.points.front().value;
        if (!curve.anchorSet)
        {
            curve.anchor = sanitizeTrackAutomationValue(parameterId, curve.anchor);
        }
    }
    else
    {
        curve.anchor = sanitizeTrackAutomationValue(parameterId, curve.anchor);
    }
}

/// Last stored point whose time is `<= t`, or -1 when every point is later.
[[nodiscard]] inline int trackAutomationPointIndexAtOrBefore(const TrackAutomationCurve& curve,
                                                             const double t) noexcept
{
    const auto& p = curve.points;
    if (p.empty() || !(t >= p.front().timeSeconds))
    {
        return -1;
    }
    int lo = 0;
    int hi = static_cast<int>(p.size()) - 1;
    while (lo < hi)
    {
        const int mid = lo + (hi - lo + 1) / 2;
        if (p[static_cast<size_t>(mid)].timeSeconds <= t)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return lo;
}

[[nodiscard]] inline float trackAutomationEvaluate(const TrackAutomationCurve& curve,
                                                   const double t,
                                                   const float manual) noexcept
{
    if (curve.points.empty() || !std::isfinite(t))
    {
        return manual;
    }
    if (t < curve.points.front().timeSeconds)
    {
        return curve.anchorSet ? curve.anchor : manual;
    }
    const int i = trackAutomationPointIndexAtOrBefore(curve, t);
    if (i < 0)
    {
        return manual;
    }
    if (i + 1 >= static_cast<int>(curve.points.size()))
    {
        return curve.points[static_cast<size_t>(i)].value;
    }
    const double t0 = curve.points[static_cast<size_t>(i)].timeSeconds;
    const double t1 = curve.points[static_cast<size_t>(i + 1)].timeSeconds;
    const double dt = t1 - t0;
    const float y0 = curve.points[static_cast<size_t>(i)].value;
    const float y1 = curve.points[static_cast<size_t>(i + 1)].value;
    if (dt <= kTrackAutomationTimeMergeSeconds)
    {
        return y1;
    }
    const double u = (t - t0) / dt;
    if (dt < kTrackAutomationShortSegmentSeconds)
    {
        return static_cast<float>((1.0 - u) * static_cast<double>(y0) + u * static_cast<double>(y1));
    }
    const float control = i < static_cast<int>(curve.control.size())
                              ? curve.control[static_cast<size_t>(i)]
                              : trackAutomationLinearControl(y0, y1);
    return trackAutomationQuadratic(y0, control, y1, u);
}

/// What a fader or pan widget shows when no gesture is active. Read off is the manual
/// value. Read on is the curve at `seconds`.
[[nodiscard]] inline float trackAutomationDisplayedValue(const TrackAutomationState& state,
                                                         const int parameterId,
                                                         const float manual,
                                                         const double seconds) noexcept
{
    const float safe = sanitizeTrackAutomationValue(parameterId, manual);
    if (!state.read)
    {
        return safe;
    }
    const TrackAutomationCurve& curve = parameterId == kTrackAutomationPanId ? state.pan : state.volume;
    return trackAutomationEvaluate(curve, seconds, safe);
}

struct TrackAutomationSplit
{
    float leftControl = 0.0f;
    float midValue = 0.0f;
    float rightControl = 0.0f;
};

/// de Casteljau at normalized `u`. The two new controls reproduce the same polynomial.
[[nodiscard]] inline TrackAutomationSplit trackAutomationSplitQuadratic(const float y0,
                                                                        const float control,
                                                                        const float y1,
                                                                        const double u) noexcept
{
    const double uu = std::clamp(u, 0.0, 1.0);
    TrackAutomationSplit s;
    s.leftControl = static_cast<float>((1.0 - uu) * static_cast<double>(y0) + uu * static_cast<double>(control));
    s.rightControl = static_cast<float>((1.0 - uu) * static_cast<double>(control) + uu * static_cast<double>(y1));
    s.midValue = trackAutomationQuadratic(y0, control, y1, uu);
    return s;
}

/// Insert a point at `timeSeconds` when it falls strictly inside a segment.
/// A time within `kTrackAutomationTimeMergeSeconds` of an existing point is left alone.
inline void trackAutomationSplitAt(TrackAutomationCurve& curve, const double timeSeconds) noexcept
{
    if (curve.points.size() < 2 || !std::isfinite(timeSeconds))
    {
        return;
    }
    for (const TrackAutomationPoint& p : curve.points)
    {
        if (std::fabs(p.timeSeconds - timeSeconds) <= kTrackAutomationTimeMergeSeconds)
        {
            return;
        }
    }
    const int i = trackAutomationPointIndexAtOrBefore(curve, timeSeconds);
    if (i < 0 || i + 1 >= static_cast<int>(curve.points.size()))
    {
        return;
    }
    const double t0 = curve.points[static_cast<size_t>(i)].timeSeconds;
    const double t1 = curve.points[static_cast<size_t>(i + 1)].timeSeconds;
    const double dt = t1 - t0;
    if (dt <= kTrackAutomationTimeMergeSeconds)
    {
        return;
    }
    const float y0 = curve.points[static_cast<size_t>(i)].value;
    const float y1 = curve.points[static_cast<size_t>(i + 1)].value;
    const float control = i < static_cast<int>(curve.control.size())
                              ? curve.control[static_cast<size_t>(i)]
                              : trackAutomationLinearControl(y0, y1);
    const TrackAutomationSplit split = trackAutomationSplitQuadratic(y0, control, y1, (timeSeconds - t0) / dt);
    TrackAutomationPoint mid;
    mid.timeSeconds = timeSeconds;
    mid.value = split.midValue;
    curve.points.insert(curve.points.begin() + (i + 1), mid);
    if (curve.control.size() < curve.points.size() - 2)
    {
        curve.control.resize(curve.points.size() - 2, trackAutomationLinearControl(y0, y1));
    }
    curve.control[static_cast<size_t>(i)] = split.leftControl;
    curve.control.insert(curve.control.begin() + (i + 1), split.rightControl);
}

/// Replace the open interval `(touchStart, rampEnd)` with `recorded`, keeping the
/// polynomial outside it. The split point at `touchStart` stays, so the left piece's
/// end value does not change. A recorded point within `1e-6` s of that boundary is
/// stored at the same time after it; evaluation uses the later point at that instant.
/// A recorded point that matches the split point at `rampEnd` is merged.
inline void trackAutomationPunch(TrackAutomationCurve& curve,
                                 const double touchStart,
                                 const double rampEnd,
                                 const std::vector<TrackAutomationPoint>& recorded,
                                 const int parameterId) noexcept
{
    if (!(rampEnd > touchStart))
    {
        return;
    }
    trackAutomationSplitAt(curve, touchStart);
    trackAutomationSplitAt(curve, rampEnd);

    struct OutPoint
    {
        TrackAutomationPoint point;
        bool explicitControl = false;
        float controlFromPrevious = 0.0f;
    };
    std::vector<OutPoint> out;
    out.reserve(curve.points.size() + recorded.size());

    auto append = [&out](const TrackAutomationPoint& point, const bool explicitControl, const float control) {
        if (!out.empty()
            && std::fabs(out.back().point.timeSeconds - point.timeSeconds) <= kTrackAutomationTimeMergeSeconds
            && std::fabs(out.back().point.value - point.value) <= kTrackAutomationValueTolerance)
        {
            out.back().point.value = point.value;
            return;
        }
        OutPoint row;
        row.point = point;
        row.explicitControl = explicitControl && !out.empty();
        row.controlFromPrevious = control;
        out.push_back(row);
    };

    int lastOriginal = -1;
    bool touchBoundaryKept = false;
    for (int i = 0; i < static_cast<int>(curve.points.size()); ++i)
    {
        const double t = curve.points[static_cast<size_t>(i)].timeSeconds;
        const bool left = t < touchStart - kTrackAutomationTimeMergeSeconds;
        const bool touchBoundary = !touchBoundaryKept
                                   && std::fabs(t - touchStart) <= kTrackAutomationTimeMergeSeconds;
        if (!left && !touchBoundary)
        {
            continue;
        }
        const bool consecutive = lastOriginal >= 0 && lastOriginal + 1 == i;
        const float control = consecutive && lastOriginal < static_cast<int>(curve.control.size())
                                  ? curve.control[static_cast<size_t>(lastOriginal)]
                                  : 0.0f;
        append(curve.points[static_cast<size_t>(i)], consecutive, control);
        lastOriginal = i;
        if (touchBoundary)
        {
            touchBoundaryKept = true;
        }
    }

    std::vector<TrackAutomationPoint> stroke = recorded;
    std::stable_sort(stroke.begin(), stroke.end(),
                     [](const TrackAutomationPoint& a, const TrackAutomationPoint& b) {
                         return a.timeSeconds < b.timeSeconds;
                     });
    for (TrackAutomationPoint point : stroke)
    {
        point.value = sanitizeTrackAutomationValue(parameterId, point.value);
        if (point.timeSeconds < touchStart - kTrackAutomationTimeMergeSeconds
            || point.timeSeconds > rampEnd + kTrackAutomationTimeMergeSeconds)
        {
            continue;
        }
        append(point, false, 0.0f);
    }

    lastOriginal = -1;
    for (int i = 0; i < static_cast<int>(curve.points.size()); ++i)
    {
        const double t = curve.points[static_cast<size_t>(i)].timeSeconds;
        if (t < rampEnd - kTrackAutomationTimeMergeSeconds)
        {
            continue;
        }
        const bool consecutive = lastOriginal >= 0 && lastOriginal + 1 == i
                                 && !out.empty()
                                 && std::fabs(out.back().point.timeSeconds
                                              - curve.points[static_cast<size_t>(lastOriginal)].timeSeconds)
                                       <= kTrackAutomationTimeMergeSeconds;
        const float control = consecutive && lastOriginal < static_cast<int>(curve.control.size())
                                  ? curve.control[static_cast<size_t>(lastOriginal)]
                                  : 0.0f;
        append(curve.points[static_cast<size_t>(i)], consecutive, control);
        lastOriginal = i;
    }

    curve.points.clear();
    curve.control.clear();
    curve.points.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (i > 0)
        {
            const float y0 = out[i - 1].point.value;
            const float y1 = out[i].point.value;
            curve.control.push_back(out[i].explicitControl
                                        ? out[i].controlFromPrevious
                                        : trackAutomationLinearControl(y0, y1));
        }
        curve.points.push_back(out[i].point);
    }
    trackAutomationNormalize(curve, parameterId);
}
