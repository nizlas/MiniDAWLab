#pragma once

// =============================================================================
// TrackAutomation — Channel Volume and Pan curves (stage 1)
// =============================================================================
//
// ROLE
//   The saved shape of one fader or pan, in project seconds. Playback, the lane, a
//   punch and a live gesture all call the same evaluation. Nothing here runs on a
//   timer and nothing here knows about the device block size.
//
// SHAPE
//   Each segment stores an on-curve knee (normalized time and value) and three
//   tangents dy/du. Evaluation is two cubic Hermite pieces. A user drag sets the
//   knee and refits monotone tangents; a punch subdivides those cubics and stores
//   the pieces, so the curve outside the punch does not change. One tangent is not
//   enough: with fixed ends (0, 0) and (1, 1) the knee can sit near (0.8, 0.2).
//
//   Equal endpoint values are flat (knee on that value, tangents 0). A segment
//   shorter than kTrackAutomationShortSegmentSeconds evaluates as the chord; the
//   stored knee is kept. User grabs clamp the knee to u in [0.08, 0.92] and the
//   value into the endpoint range. Stored tangents from a split are not re-clamped.
//
// TIME
//   Seconds from the start of the timeline. The sample index is a timeline sample
//   at the project's reference rate, not the device rate:
//     timelineSample = llround(seconds * timelineRate)
//     seconds        = timelineSample / timelineRate
//   Opening a project does not rewrite the stored seconds.
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

/// One segment's two-piece shape. Tangents are dy/du in the segment's own
/// normalized time. `kneeU` / `kneeY` is a point on the curve.
struct TrackAutomationShape
{
    float kneeU = 0.5f;
    float kneeY = 0.0f;
    float tan0 = 0.0f;
    float tanKnee = 0.0f;
    float tan1 = 0.0f;
};

struct TrackAutomationCurve
{
    float anchor = 0.0f;
    bool anchorSet = false;
    std::vector<TrackAutomationPoint> points;
    /// `shape[i]` is the segment `points[i] → points[i + 1]`.
    std::vector<TrackAutomationShape> shape;
};

struct TrackAutomationState
{
    bool read = false;
    bool volumeLaneVisible = false;
    bool panLaneVisible = false;
    /// Bumped by every successful `setTrackAutomation`. The audio thread reads this
    /// from the snapshot it actually mixes. It is not written to the project file.
    uint32_t revision = 0;
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

[[nodiscard]] inline double trackAutomationClampGrabU(const double u) noexcept
{
    if (!std::isfinite(u))
    {
        return 0.5;
    }
    return std::clamp(u, kTrackAutomationGrabUMin, kTrackAutomationGrabUMax);
}

[[nodiscard]] inline TrackAutomationShape trackAutomationLinearShape(const float y0, const float y1) noexcept
{
    TrackAutomationShape shape;
    shape.kneeU = 0.5f;
    shape.kneeY = 0.5f * (y0 + y1);
    const float slope = y1 - y0;
    shape.tan0 = slope;
    shape.tanKnee = slope;
    shape.tan1 = slope;
    return shape;
}

[[nodiscard]] inline float trackAutomationHermite(const float y0,
                                                  const float y1,
                                                  const double m0,
                                                  const double m1,
                                                  const double s) noexcept
{
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
    const double h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 = s3 - s2;
    return static_cast<float>(h00 * static_cast<double>(y0) + h10 * m0 + h01 * static_cast<double>(y1) + h11 * m1);
}

[[nodiscard]] inline double trackAutomationHermiteDerivative(const float y0,
                                                            const float y1,
                                                            const double m0,
                                                            const double m1,
                                                            const double s) noexcept
{
    const double s2 = s * s;
    const double h00 = 6.0 * s2 - 6.0 * s;
    const double h10 = 3.0 * s2 - 4.0 * s + 1.0;
    const double h01 = -6.0 * s2 + 6.0 * s;
    const double h11 = 3.0 * s2 - 2.0 * s;
    return h00 * static_cast<double>(y0) + h10 * m0 + h01 * static_cast<double>(y1) + h11 * m1;
}

/// Value and dy/du of one shape at normalized `u` in [0, 1].
inline void trackAutomationShapeSample(const float y0,
                                       const TrackAutomationShape& shape,
                                       const float y1,
                                       const double u,
                                       float& outValue,
                                       double& outSlope) noexcept
{
    const double uu = std::clamp(u, 0.0, 1.0);
    const double knee = std::clamp(static_cast<double>(shape.kneeU), 1.0e-3, 1.0 - 1.0e-3);
    if (uu <= knee)
    {
        const double s = uu / knee;
        const double m0 = static_cast<double>(shape.tan0) * knee;
        const double m1 = static_cast<double>(shape.tanKnee) * knee;
        outValue = trackAutomationHermite(y0, shape.kneeY, m0, m1, s);
        outSlope = trackAutomationHermiteDerivative(y0, shape.kneeY, m0, m1, s) / knee;
        return;
    }
    const double span = 1.0 - knee;
    const double s = (uu - knee) / span;
    const double m0 = static_cast<double>(shape.tanKnee) * span;
    const double m1 = static_cast<double>(shape.tan1) * span;
    outValue = trackAutomationHermite(shape.kneeY, y1, m0, m1, s);
    outSlope = trackAutomationHermiteDerivative(shape.kneeY, y1, m0, m1, s) / span;
}

[[nodiscard]] inline float trackAutomationShapeValue(const float y0,
                                                     const TrackAutomationShape& shape,
                                                     const float y1,
                                                     const double u) noexcept
{
    float value = y0;
    double slope = 0.0;
    trackAutomationShapeSample(y0, shape, y1, u, value, slope);
    return value;
}

/// Monotone two-piece shape through `(kneeU, kneeY)`. User edits call this.
/// A stored shape from a split is not passed back through here.
inline void trackAutomationShapeFromKnee(const float y0,
                                         const float y1,
                                         double kneeU,
                                         float kneeY,
                                         TrackAutomationShape& out) noexcept
{
    kneeU = trackAutomationClampGrabU(kneeU);
    if (y0 == y1 || !std::isfinite(kneeY))
    {
        out = trackAutomationLinearShape(y0, y0);
        out.kneeU = static_cast<float>(kneeU);
        out.kneeY = y0;
        out.tan0 = 0.0f;
        out.tanKnee = 0.0f;
        out.tan1 = 0.0f;
        return;
    }
    const float lo = std::min(y0, y1);
    const float hi = std::max(y0, y1);
    kneeY = std::clamp(kneeY, lo, hi);
    const double h0 = kneeU;
    const double h1 = 1.0 - kneeU;
    const double d0 = (static_cast<double>(kneeY) - static_cast<double>(y0)) / h0;
    const double d1 = (static_cast<double>(y1) - static_cast<double>(kneeY)) / h1;
    double m0 = d0;
    double m2 = d1;
    double m1 = 0.0;
    if (d0 * d1 > 0.0)
    {
        const double w1 = 2.0 * h1 + h0;
        const double w2 = h1 + 2.0 * h0;
        m1 = (w1 + w2) / (w1 / d0 + w2 / d1);
    }
    auto clampInterval = [](double& left, double& right, const double secant) {
        if (secant == 0.0)
        {
            left = 0.0;
            right = 0.0;
            return;
        }
        const double a = left / secant;
        const double b = right / secant;
        const double sum = a * a + b * b;
        if (sum > 9.0)
        {
            const double scale = 3.0 / std::sqrt(sum);
            left = scale * a * secant;
            right = scale * b * secant;
        }
    };
    clampInterval(m0, m1, d0);
    clampInterval(m1, m2, d1);
    out.kneeU = static_cast<float>(kneeU);
    out.kneeY = kneeY;
    out.tan0 = static_cast<float>(m0);
    out.tanKnee = static_cast<float>(m1);
    out.tan1 = static_cast<float>(m2);
}

/// v29 stored one quadratic control `C`. The two Hermite halves of that quadratic
/// reproduce it, including a control that a punch had left outside the endpoints.
[[nodiscard]] inline TrackAutomationShape trackAutomationShapeFromLegacyControl(const float y0,
                                                                               const float y1,
                                                                               const float control) noexcept
{
    const double c = static_cast<double>(control);
    const double yMid = 0.25 * static_cast<double>(y0) + 0.5 * c + 0.25 * static_cast<double>(y1);
    TrackAutomationShape shape;
    shape.kneeU = 0.5f;
    shape.kneeY = static_cast<float>(yMid);
    shape.tan0 = static_cast<float>(2.0 * (c - static_cast<double>(y0)));
    shape.tanKnee = y1 - y0;
    shape.tan1 = static_cast<float>(2.0 * (static_cast<double>(y1) - c));
    return shape;
}

[[nodiscard]] inline int64_t trackAutomationSecondsToEngineSample(const double seconds,
                                                                  const double timelineRate) noexcept
{
    if (!(timelineRate > 0.0) || !std::isfinite(seconds))
    {
        return 0;
    }
    return std::llround(seconds * timelineRate);
}

[[nodiscard]] inline double trackAutomationEngineSampleToSeconds(const int64_t timelineSample,
                                                                 const double timelineRate) noexcept
{
    if (!(timelineRate > 0.0))
    {
        return 0.0;
    }
    return static_cast<double>(timelineSample) / timelineRate;
}

/// Rate that maps a timeline sample index to a project second. A valid project
/// reference rate wins. Otherwise the device or export rate is the fallback, so
/// an unset project still agrees with the device.
[[nodiscard]] inline double trackAutomationTimelineRate(const double projectRate,
                                                        const double deviceOrExportRate) noexcept
{
    if (projectRate > 0.0 && std::isfinite(projectRate))
    {
        return projectRate;
    }
    if (deviceOrExportRate > 0.0 && std::isfinite(deviceOrExportRate))
    {
        return deviceOrExportRate;
    }
    return 48000.0;
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
        curve.shape.clear();
    }
    if (curve.points.size() < 2)
    {
        curve.shape.clear();
    }
    else
    {
        curve.shape.resize(curve.points.size() - 1);
        for (size_t i = 0; i + 1 < curve.points.size(); ++i)
        {
            const float y0 = curve.points[i].value;
            const float y1 = curve.points[i + 1].value;
            TrackAutomationShape& shape = curve.shape[i];
            if (!std::isfinite(shape.kneeU) || !std::isfinite(shape.kneeY) || !std::isfinite(shape.tan0)
                || !std::isfinite(shape.tanKnee) || !std::isfinite(shape.tan1) || !(shape.kneeU > 0.0f)
                || !(shape.kneeU < 1.0f))
            {
                shape = trackAutomationLinearShape(y0, y1);
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

[[nodiscard]] inline TrackAutomationShape trackAutomationShapeOrLinear(const TrackAutomationCurve& curve,
                                                                       const int segment,
                                                                       const float y0,
                                                                       const float y1) noexcept
{
    if (segment >= 0 && segment < static_cast<int>(curve.shape.size()))
    {
        return curve.shape[static_cast<size_t>(segment)];
    }
    return trackAutomationLinearShape(y0, y1);
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
    const TrackAutomationShape shape = trackAutomationShapeOrLinear(curve, i, y0, y1);
    return trackAutomationShapeValue(y0, shape, y1, u);
}

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
    TrackAutomationShape left{};
    float midValue = 0.0f;
    TrackAutomationShape right{};
};

/// Subdivide the stored cubics at normalized `u`. The two shapes reproduce the same
/// pieces; tangents are scaled into each new segment's own normalized time.
[[nodiscard]] inline TrackAutomationSplit trackAutomationSplitShape(const float y0,
                                                                    const TrackAutomationShape& shape,
                                                                    const float y1,
                                                                    const double u) noexcept
{
    const double uu = std::clamp(u, 1.0e-4, 1.0 - 1.0e-4);
    float mid = y0;
    double midSlope = 0.0;
    trackAutomationShapeSample(y0, shape, y1, uu, mid, midSlope);

    auto subdivideSpan = [](const float a,
                            const float b,
                            const double tanA,
                            const double tanB,
                            const double spanU,
                            const double s,
                            float& outMid,
                            double& outMidTan,
                            double& leftTan0,
                            double& leftTan1,
                            double& rightTan0,
                            double& rightTan1) {
        const double ss = std::clamp(s, 1.0e-4, 1.0 - 1.0e-4);
        const double m0 = tanA * spanU;
        const double m1 = tanB * spanU;
        const double p0 = a;
        const double p1 = static_cast<double>(a) + m0 / 3.0;
        const double p2 = static_cast<double>(b) - m1 / 3.0;
        const double p3 = b;
        const double q0 = (1.0 - ss) * p0 + ss * p1;
        const double q1 = (1.0 - ss) * p1 + ss * p2;
        const double q2 = (1.0 - ss) * p2 + ss * p3;
        const double r0 = (1.0 - ss) * q0 + ss * q1;
        const double r1 = (1.0 - ss) * q1 + ss * q2;
        const double value = (1.0 - ss) * r0 + ss * r1;
        const double leftLen = spanU * ss;
        const double rightLen = spanU * (1.0 - ss);
        leftTan0 = (3.0 * (q0 - p0)) / leftLen;
        leftTan1 = (3.0 * (value - r0)) / leftLen;
        rightTan0 = (3.0 * (r1 - value)) / rightLen;
        rightTan1 = (3.0 * (p3 - q2)) / rightLen;
        outMid = static_cast<float>(value);
        outMidTan = leftTan1;
    };

    auto shapeFromOneSpan = [](const float a, const float b, const double tanA, const double tanB) {
        TrackAutomationShape out;
        const double m0 = tanA * 0.5;
        const double m1 = tanB * 0.5;
        const float knee = trackAutomationHermite(a, b, m0, m1, 0.5);
        const double slope = trackAutomationHermiteDerivative(a, b, m0, m1, 0.5) / 0.5;
        out.kneeU = 0.5f;
        out.kneeY = knee;
        out.tan0 = static_cast<float>(tanA);
        out.tanKnee = static_cast<float>(slope);
        out.tan1 = static_cast<float>(tanB);
        return out;
    };

    TrackAutomationSplit split;
    split.midValue = mid;
    const double knee = std::clamp(static_cast<double>(shape.kneeU), 1.0e-3, 1.0 - 1.0e-3);
    // Tangents below that come out of a span are dy/du of the parent segment.
    // A child segment of parent-length L stores dy/du_child = dy/du_parent * L.
    if (std::fabs(uu - knee) <= 1.0e-6)
    {
        split.left = shapeFromOneSpan(y0, shape.kneeY, shape.tan0 * uu, shape.tanKnee * uu);
        const double rightScale = 1.0 - uu;
        split.right = shapeFromOneSpan(shape.kneeY,
                                       y1,
                                       shape.tanKnee * rightScale,
                                       shape.tan1 * rightScale);
        return split;
    }

    if (uu < knee)
    {
        const double s = uu / knee;
        float spanMid = 0.0f;
        double spanTan = 0.0;
        double l0 = 0.0, l1 = 0.0, r0 = 0.0, r1 = 0.0;
        subdivideSpan(y0, shape.kneeY, shape.tan0, shape.tanKnee, knee, s, spanMid, spanTan, l0, l1, r0, r1);
        split.left = shapeFromOneSpan(y0, mid, l0 * uu, l1 * uu);
        const double rightScale = 1.0 - uu;
        split.right.kneeU = static_cast<float>((knee - uu) / rightScale);
        split.right.kneeY = shape.kneeY;
        split.right.tan0 = static_cast<float>(r0 * rightScale);
        split.right.tanKnee = static_cast<float>(static_cast<double>(shape.tanKnee) * rightScale);
        split.right.tan1 = static_cast<float>(static_cast<double>(shape.tan1) * rightScale);
        return split;
    }

    const double span = 1.0 - knee;
    const double s = (uu - knee) / span;
    float spanMid = 0.0f;
    double spanTan = 0.0;
    double l0 = 0.0, l1 = 0.0, r0 = 0.0, r1 = 0.0;
    subdivideSpan(shape.kneeY, y1, shape.tanKnee, shape.tan1, span, s, spanMid, spanTan, l0, l1, r0, r1);
    split.left.kneeU = static_cast<float>(knee / uu);
    split.left.kneeY = shape.kneeY;
    split.left.tan0 = static_cast<float>(static_cast<double>(shape.tan0) * uu);
    split.left.tanKnee = static_cast<float>(static_cast<double>(shape.tanKnee) * uu);
    split.left.tan1 = static_cast<float>(l1 * uu);
    const double rightScale = 1.0 - uu;
    split.right = shapeFromOneSpan(mid, y1, r0 * rightScale, r1 * rightScale);
    return split;
}

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
    const TrackAutomationShape shape = trackAutomationShapeOrLinear(curve, i, y0, y1);
    const TrackAutomationSplit split = trackAutomationSplitShape(y0, shape, y1, (timeSeconds - t0) / dt);
    TrackAutomationPoint mid;
    mid.timeSeconds = timeSeconds;
    mid.value = split.midValue;
    curve.points.insert(curve.points.begin() + (i + 1), mid);
    if (curve.shape.size() < curve.points.size() - 2)
    {
        curve.shape.resize(curve.points.size() - 2, trackAutomationLinearShape(y0, y1));
    }
    curve.shape[static_cast<size_t>(i)] = split.left;
    curve.shape.insert(curve.shape.begin() + (i + 1), split.right);
}

/// Replace `(touchStart, rampEnd)` with `recorded`. Shapes outside that interval
/// are the subdivided cubics, not a refit, so the outside curve stays put.
/// Recorded points become linear pieces. A point within 1e-6 s of `touchStart`
/// is stored just after it. A recorded point that matches the split at `rampEnd`
/// is merged.
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
        bool explicitShape = false;
        TrackAutomationShape shapeFromPrevious{};
    };
    std::vector<OutPoint> out;
    out.reserve(curve.points.size() + recorded.size());

    auto append = [&out](const TrackAutomationPoint& point, const bool explicitShape, const TrackAutomationShape& shape) {
        if (!out.empty()
            && std::fabs(out.back().point.timeSeconds - point.timeSeconds) <= kTrackAutomationTimeMergeSeconds
            && std::fabs(out.back().point.value - point.value) <= kTrackAutomationValueTolerance)
        {
            out.back().point.value = point.value;
            return;
        }
        OutPoint row;
        row.point = point;
        row.explicitShape = explicitShape && !out.empty();
        row.shapeFromPrevious = shape;
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
        const TrackAutomationShape shape = consecutive
                                               ? trackAutomationShapeOrLinear(curve,
                                                                              lastOriginal,
                                                                              curve.points[static_cast<size_t>(lastOriginal)].value,
                                                                              curve.points[static_cast<size_t>(i)].value)
                                               : TrackAutomationShape{};
        append(curve.points[static_cast<size_t>(i)], consecutive, shape);
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
        if (point.timeSeconds <= touchStart + kTrackAutomationTimeMergeSeconds)
        {
            point.timeSeconds = touchStart + kTrackAutomationTimeMergeSeconds * 2.0;
        }
        append(point, false, {});
    }

    lastOriginal = -1;
    for (int i = 0; i < static_cast<int>(curve.points.size()); ++i)
    {
        const double t = curve.points[static_cast<size_t>(i)].timeSeconds;
        if (t < rampEnd - kTrackAutomationTimeMergeSeconds)
        {
            continue;
        }
        const bool consecutive = lastOriginal >= 0 && lastOriginal + 1 == i && !out.empty()
                                 && std::fabs(out.back().point.timeSeconds
                                              - curve.points[static_cast<size_t>(lastOriginal)].timeSeconds)
                                        <= kTrackAutomationTimeMergeSeconds;
        const TrackAutomationShape shape = consecutive
                                               ? trackAutomationShapeOrLinear(curve,
                                                                              lastOriginal,
                                                                              curve.points[static_cast<size_t>(lastOriginal)].value,
                                                                              curve.points[static_cast<size_t>(i)].value)
                                               : TrackAutomationShape{};
        append(curve.points[static_cast<size_t>(i)], consecutive, shape);
        lastOriginal = i;
    }

    curve.points.clear();
    curve.shape.clear();
    curve.points.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (i > 0)
        {
            const float left = out[i - 1].point.value;
            const float right = out[i].point.value;
            curve.shape.push_back(out[i].explicitShape ? out[i].shapeFromPrevious
                                                       : trackAutomationLinearShape(left, right));
        }
        curve.points.push_back(out[i].point);
    }
    trackAutomationNormalize(curve, parameterId);
}
