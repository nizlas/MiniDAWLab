// =============================================================================
// ClipWaveformView.cpp  —  DAW-style event layout on the session timeline (message thread)
// =============================================================================
//
// ROLE
//   Fills a list from `loadSessionSnapshotForAudioThread()`: for each `PlacedClip` row, draw the
//   event **envelope** and a peak sketch; single-lane **selection** (UI-local) and **drag to move**
//   (committed via `Session::moveClip` / `Session::moveClipToTrack` only) sit on the same view —
//   ordering policy is not here. **Invalid cross-lane drop** uses `getForbiddenNoDropMouseCursor`
//   (`ForbiddenCursor.h` / `.cpp`); restore still uses the standard arrow.
//   In material columns whose **center** falls in session time *not* covered by any row in front
//   (lower index in the snapshot, painted later). **Covered**
//   time on a back row: no readable peaks; the overlying event shows through after back→front order.
//   A **post-pass** per row applies the same overlap *hint* (tint + thin diagonals) only in session
//   time where *that* row is the local top and an older row still underlaps — view only. Session
//   samples → x match the playhead.
//
// PEDAGOGICAL GOAL
//   A reader should **not** have to mentally simulate the full JUCE paint order to learn **why**
//   a row skips a peak column, or which interval math feeds the hatch. The helpers and branches
//   below state the **user-visible** rule in plain language first; mechanics follow.
//
// THREADING
//   `paint` / `mouseDown` / `timerCallback` are [Message thread] only. JUCE `Graphics` API here is
//   single-threaded UI drawing — not a substitute for a waveform cache on the audio thread.
//
// See ClipWaveformView.h: local topmost overlap *hint* (same graphic as before), not “row 0 only.”
// =============================================================================

#include "ui/ClipWaveformView.h"

#include "ui/TimelineClipEventChrome.h"
#include "ui/ForbiddenCursor.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "io/AudioWaveformCache.h"
#include "diagnostics/UiPaintLoadCounters.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "domain/AudioClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "transport/Transport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace tc = mini_daw::timeline_clip_chrome;

// See `getForbiddenNoDropMouseCursor` in `ForbiddenCursor.cpp` (shared with `TrackHeaderView`).

// Anonymous helpers: all **session-timeline** intervals are half-open [a, b) in device samples,
// matching `PlacedClip` placement + `PlaybackEngine` / `Transport` usage. They exist only to
// keep **paint** decisions (where to draw peaks vs overlap tint) consistent and cheap — **not** to
// duplicate engine mixing rules; this file never writes transport or the session.
namespace
{
// Off by default: logs coarse `paint` timing + raster cache stats when enabled (developer-only).
    constexpr bool kClipWaveformPaintDiagnostics = false;
    // Vertical only for peak bar clamping; horizontal uses full `eventRect` width so adjacent split
    // segments share one timeline scale with no stacked side insets (avoids a visible waveform gap).
    constexpr float kWaveInset = 2.0f;
    // Horizontal delta below which a mousedown+move is treated as a click (no `Session::moveClip`).
    constexpr float kDragThresholdPx = 3.0f;

    [[nodiscard]] std::int64_t trimViewMappingSpan(
        const std::int64_t visStart,
        const std::int64_t visLen,
        const std::int64_t clipStart,
        const int materialLen) noexcept
    {
        if (materialLen <= 0)
        {
            return juce::jmax(std::int64_t{0}, visLen);
        }
        const std::int64_t matEnd = clipStart + static_cast<std::int64_t>(materialLen);
        const std::int64_t spanToMat = juce::jmax(std::int64_t{0}, matEnd - visStart);
        return juce::jmax(visLen, spanToMat);
    }

    // Delegates to `TimelineRulerView` so lane + ruler share one mapping (samples-per-pixel, or
    // `spanSamples` for trim extended span).
    [[nodiscard]] float sessionSampleToLocalX(
        const std::int64_t s,
        const juce::Rectangle<float>& b,
        const std::int64_t visStart,
        const std::int64_t mapSpan) noexcept
    {
        return TimelineRulerView::sessionSampleToLocalXForSpan(s, b, visStart, mapSpan);
    }

    enum class LanePixelHitKind { None, TrimLeft, TrimRight, EventBody };

    struct LanePixelHit
    {
        LanePixelHitKind kind = LanePixelHitKind::None;
        int rowInTrack = -1;
        PlacedClipId id{ kInvalidPlacedClipId };
    };

    [[nodiscard]] LanePixelHit hitPlacedInLaneAtPixels(
        const std::shared_ptr<const SessionSnapshot>& snap,
        const int tIdx,
        juce::Point<float> p,
        const juce::Rectangle<float>& b,
        const juce::Rectangle<float>& eventTrackY,
        const std::int64_t visStart,
        const double samplesPerPixel) noexcept
    {
        LanePixelHit r;
        if (snap == nullptr || tIdx < 0)
        {
            return r;
        }
        if (!b.contains(p) || !eventTrackY.contains(p))
        {
            return r;
        }
        if (samplesPerPixel <= 0.0)
        {
            return r;
        }
        const Track& tr = snap->getTrack(tIdx);
        const int n = tr.getNumPlacedClips();
        // Priority: left band → right band → right corner → left corner → body (per plan). Newest row
        // (smaller `i`) wins first in each pass.
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& pc = tr.getPlacedClip(i);
            const std::int64_t a0 = pc.getStartSample();
            const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
            if (a0 >= a1)
            {
                continue;
            }
            const float ex0
                = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, samplesPerPixel);
            const float ex1
                = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, samplesPerPixel);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            if (x1 - x0 < 0.5f || eventTrackY.getHeight() < 2.0f)
            {
                continue;
            }
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(0.5f, x1 - x0), eventTrackY.getHeight() };
            const float lBandW = juce::jmin(tc::kTrimHitLeftEdgeBandPx, eventRect.getWidth());
            juce::Rectangle<float> leftEdgeHitBand{ eventRect.getX(), eventRect.getY(), lBandW, eventRect.getHeight() };
            if (leftEdgeHitBand.getWidth() >= 0.5f && leftEdgeHitBand.contains(p))
            {
                r.kind = LanePixelHitKind::TrimLeft;
                r.rowInTrack = i;
                r.id = pc.getId();
                return r;
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& pc = tr.getPlacedClip(i);
            const std::int64_t a0 = pc.getStartSample();
            const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
            if (a0 >= a1)
            {
                continue;
            }
            const float ex0
                = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, samplesPerPixel);
            const float ex1
                = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, samplesPerPixel);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            if (x1 - x0 < 0.5f || eventTrackY.getHeight() < 2.0f)
            {
                continue;
            }
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(0.5f, x1 - x0), eventTrackY.getHeight() };
            const float bandW = juce::jmin(tc::kTrimHitRightEdgeBandPx, eventRect.getWidth());
            const float bandX = juce::jmax(eventRect.getX(), eventRect.getRight() - bandW);
            juce::Rectangle<float> edgeHitBand{ bandX, eventRect.getY(), bandW, eventRect.getHeight() };
            if (edgeHitBand.getWidth() >= 0.5f && edgeHitBand.contains(p))
            {
                r.kind = LanePixelHitKind::TrimRight;
                r.rowInTrack = i;
                r.id = pc.getId();
                return r;
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& pc = tr.getPlacedClip(i);
            const std::int64_t a0 = pc.getStartSample();
            const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
            if (a0 >= a1)
            {
                continue;
            }
            const float ex0
                = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, samplesPerPixel);
            const float ex1
                = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, samplesPerPixel);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            if (x1 - x0 < 0.5f || eventTrackY.getHeight() < 2.0f)
            {
                continue;
            }
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(0.5f, x1 - x0), eventTrackY.getHeight() };
            if (eventRect.getWidth() >= tc::kMinEventWidthForTrimHandlePx
                && eventRect.getHeight() >= tc::kTrimHandleSquarePx + 2.0f)
            {
                const float hsz
                    = juce::jmin(tc::kTrimHandleSquarePx, eventRect.getWidth() * 0.4f, eventRect.getHeight() * 0.4f);
                if (hsz >= 2.0f)
                {
                    const float hLeft = juce::jmax(
                        eventRect.getX() + 0.5f, eventRect.getRight() - tc::kTrimHandleMarginPx - hsz);
                    const float hTop = juce::jmax(
                        eventRect.getY() + 0.5f, eventRect.getBottom() - tc::kTrimHandleMarginPx - hsz);
                    const juce::Rectangle<float> hRect{ hLeft, hTop, hsz, hsz };
                    if (hRect.contains(p))
                    {
                        r.kind = LanePixelHitKind::TrimRight;
                        r.rowInTrack = i;
                        r.id = pc.getId();
                        return r;
                    }
                }
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& pc = tr.getPlacedClip(i);
            const std::int64_t a0 = pc.getStartSample();
            const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
            if (a0 >= a1)
            {
                continue;
            }
            const float ex0
                = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, samplesPerPixel);
            const float ex1
                = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, samplesPerPixel);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            if (x1 - x0 < 0.5f || eventTrackY.getHeight() < 2.0f)
            {
                continue;
            }
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(0.5f, x1 - x0), eventTrackY.getHeight() };
            if (eventRect.getWidth() >= tc::kMinEventWidthForTrimHandlePx
                && eventRect.getHeight() >= tc::kTrimHandleSquarePx + 2.0f)
            {
                const float hsz
                    = juce::jmin(tc::kTrimHandleSquarePx, eventRect.getWidth() * 0.4f, eventRect.getHeight() * 0.4f);
                if (hsz >= 2.0f)
                {
                    const float hLeftL = juce::jmin(
                        eventRect.getX() + tc::kTrimHandleMarginPx, eventRect.getRight() - hsz - 0.5f);
                    const float hTop = juce::jmax(
                        eventRect.getY() + 0.5f, eventRect.getBottom() - tc::kTrimHandleMarginPx - hsz);
                    const juce::Rectangle<float> hRectL{ hLeftL, hTop, hsz, hsz };
                    if (hRectL.contains(p))
                    {
                        r.kind = LanePixelHitKind::TrimLeft;
                        r.rowInTrack = i;
                        r.id = pc.getId();
                        return r;
                    }
                }
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& pc = tr.getPlacedClip(i);
            const std::int64_t a0 = pc.getStartSample();
            const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
            if (a0 >= a1)
            {
                continue;
            }
            const float ex0
                = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, samplesPerPixel);
            const float ex1
                = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, samplesPerPixel);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0), eventTrackY.getHeight() };
            if (eventRect.contains(p))
            {
                r.kind = LanePixelHitKind::EventBody;
                r.rowInTrack = i;
                r.id = pc.getId();
                return r;
            }
        }
        return r;
    }

    [[nodiscard]] LanePixelHit hitPlacedEventBodyOnlyInLaneAtPixels(
        const std::shared_ptr<const SessionSnapshot>& snap,
        const int tIdx,
        juce::Point<float> p,
        const juce::Rectangle<float>& b,
        const juce::Rectangle<float>& eventTrackY,
        const std::int64_t visStart,
        const double samplesPerPixel) noexcept
    {
        LanePixelHit r;
        if (snap == nullptr || tIdx < 0)
        {
            return r;
        }
        if (!b.contains(p) || !eventTrackY.contains(p))
        {
            return r;
        }
        if (samplesPerPixel <= 0.0)
        {
            return r;
        }
        const Track& tr = snap->getTrack(tIdx);
        const int n = tr.getNumPlacedClips();
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& pc = tr.getPlacedClip(i);
            const std::int64_t a0 = pc.getStartSample();
            const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
            if (a0 >= a1)
            {
                continue;
            }
            const float ex0
                = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, samplesPerPixel);
            const float ex1
                = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, samplesPerPixel);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0), eventTrackY.getHeight() };
            if (eventRect.contains(p))
            {
                r.kind = LanePixelHitKind::EventBody;
                r.rowInTrack = i;
                r.id = pc.getId();
                return r;
            }
        }
        return r;
    }

    // Peak bar opacity for any row, whenever that column is not covered by a prior row in time.
    constexpr float kWaveformPeakAlpha = 0.9f;
    // Display only: does **not** affect audio, files, or session data. Applied to peak→pixels mapping.
    constexpr float kWaveformDisplayGain = 3.0f;

    // Min/max in approximately [-1,1] (recording preview blocks + pyramid-driven committed clips).
    // Clamps vertical span to `inner` after display gain; purely visual.
    void fillMinMaxPeakRectClamped(
        juce::Graphics& g,
        const float minS,
        const float maxS,
        const float halfDraw,
        const juce::Rectangle<float>& inner,
        const float midY,
        const float bx0,
        const float segW,
        const juce::Colour& colour)
    {
        const float sMin = juce::jlimit(-1.0f, 1.0f, minS);
        const float sMax = juce::jlimit(-1.0f, 1.0f, maxS);
        float yTop = midY - sMax * kWaveformDisplayGain * halfDraw;
        float yBot = midY - sMin * kWaveformDisplayGain * halfDraw;
        float yHi = juce::jmin(yTop, yBot);
        float yLo = juce::jmax(yTop, yBot);
        yHi = juce::jmax(yHi, inner.getY());
        yLo = juce::jmin(yLo, inner.getBottom());
        if (yLo <= yHi)
        {
            return;
        }
        g.setColour(colour);
        g.fillRect(bx0, yHi, juce::jmax(1.0f, segW), yLo - yHi);
    }

    // --- Live take preview: one pass’s body + min/max peak blocks (timeline-anchored).
    void paintLiveRecordingPassPreview(
        juce::Graphics& g,
        const float boundsX,
        const std::int64_t visStart,
        const double spp,
        const juce::Rectangle<float>& eventTrackY,
        const std::int64_t span0,
        const std::int64_t rawLen,
        const std::vector<RecordingPreviewPeakBlock>& peaks,
        const juce::Colour& bodyFillColour,
        const juce::Colour& borderColour,
        const juce::Colour& peakColour,
        const float halfDraw,
        const float midY)
    {
        const std::int64_t span1 = span0 + (rawLen > 0 ? rawLen : 1);
        if (span1 <= span0)
        {
            return;
        }
        float rx0 = TimelineRulerView::sessionSampleToLocalX(span0, boundsX, visStart, spp);
        float rx1 = TimelineRulerView::sessionSampleToLocalX(span1, boundsX, visStart, spp);
        const float xl = juce::jmin(rx0, rx1);
        const float xr = juce::jmax(rx0, rx1);
        juce::Rectangle<float> recRect{ xl, eventTrackY.getY(), juce::jmax(1.0f, xr - xl),
                                        eventTrackY.getHeight() };
        // Recording preview: fully opaque (no alpha blending — older passes must not show through).
        g.setColour(bodyFillColour);
        g.fillRect(recRect);
        g.setColour(borderColour);
        g.drawRect(recRect, 1.25f);

        juce::Rectangle<float> innerRec = recRect.reduced(1.5f, 1.5f);

        if (innerRec.getWidth() < 0.5f || innerRec.getHeight() < 1.0f)
        {
            return;
        }
        std::int64_t acc = 0;
        for (const auto& b : peaks)
        {
            if (b.numSourceSamples <= 0)
            {
                continue;
            }
            const std::int64_t seg0s = span0 + acc;
            const std::int64_t seg1s = seg0s + static_cast<std::int64_t>(b.numSourceSamples);
            acc += static_cast<std::int64_t>(b.numSourceSamples);
            const float sx0 = TimelineRulerView::sessionSampleToLocalX(
                seg0s, boundsX, visStart, spp);
            const float sx1 = TimelineRulerView::sessionSampleToLocalX(
                seg1s, boundsX, visStart, spp);
            const float segLeft = juce::jmin(sx0, sx1);
            const float segRight = juce::jmax(sx0, sx1);
            const float bx0 = juce::jmax(innerRec.getX(), segLeft);
            const float bx1 = juce::jmin(innerRec.getRight(), segRight);
            const float segW = juce::jmax(1.0f, bx1 - bx0);
            if (segW < 0.5f)
            {
                continue;
            }
            fillMinMaxPeakRectClamped(
                g,
                b.minSample,
                b.maxSample,
                halfDraw,
                innerRec,
                midY,
                bx0,
                segW,
                peakColour);
        }
    }

    // Merges **overlapping or touching** half-open [a,b) intervals: [0,5) and [5,8) become [0,8)
    // (same *union* in continuous time, since 5 is not included twice). **Why:** the hatch and
    // interval tricks below assume “one closed-open segment per connected region” — otherwise
    // we would double-tint or double-count a boundary at a sample where one clip ends and another
    // could begin. Used after “union of row spans” and after intersecting visible×behind.
    void mergeNonOverlapping(std::vector<std::pair<std::int64_t, std::int64_t>>& inOut)
    {
        if (inOut.size() < 2)
        {
            // 0 or 1 interval: already a canonical list; nothing to coalesce.
            return;
        }
        std::sort(inOut.begin(), inOut.end());
        size_t w = 0;
        for (size_t r = 1; r < inOut.size(); ++r)
        {
            if (inOut[r].first <= inOut[w].second)
            {
                inOut[w].second = std::max(inOut[w].second, inOut[r].second);
            }
            else
            {
                // Disjoint from the run at `w`: that connected component is **finished**; the next
                // interval starts a new merged piece at the new write index.
                ++w;
                inOut[w] = inOut[r];
            }
        }
        inOut.resize(w + 1);
    }

    // Answers: "If this row’s *event* runs from session `a` to `b`, on which sub-ranges is it
    // *still* the topmost **painted** layer?" Subtract the merged union of all **newer** clips
    // (rows with smaller index) — those sweeps are drawn later and **cover** the older paint.
    // The algorithm is plain interval subtraction: walk left to right, emit gaps between
    // occluders, tail out from `cur` to `b`. **Not** the engine’s audibility rule; **only** the
    // z-order of rectangles in this view.
    void subtractOpenFromMerged(
        const std::int64_t a,
        const std::int64_t b,
        const std::vector<std::pair<std::int64_t, std::int64_t>>& mergedSorted,
        std::vector<std::pair<std::int64_t, std::int64_t>>& out)
    {
        out.clear();
        if (a >= b)
        {
            return;
        }
        if (mergedSorted.empty())
        {
            out.push_back({ a, b });
            return;
        }
        std::int64_t cur = a;
        for (const auto& iv : mergedSorted)
        {
            if (iv.second <= cur)
            {
                // This occluder is entirely to the left of our cursor: already “cut out,” skip.
                continue;
            }
            if (iv.first >= b)
            {
                // All remaining union pieces start at or after `b` — the rest of [a,b) is **free**.
                break;
            }
            if (iv.first > cur)
            {
                // The gap before the next **covering** interval starts — a visible strip of *this* row.
                out.push_back({ cur, std::min(iv.first, b) });
            }
            cur = std::max(cur, iv.second);
            if (cur >= b)
            {
                return;
            }
        }
        if (cur < b)
        {
            // Trailing **visible** segment after the last occluder that met [a,b) — the right tail
            // of the row with nothing newer drawn on top.
            out.push_back({ cur, b });
        }
    }

    // Paints the **agreed** overlap *hint* (tint + fine diagonals) for each **merged** session
    // [L,R) where the caller has already decided this row is the *local* top over something
    // older. **Intentionally not** a second waveform: the user is told *that* more material
    // exists in time, not what it looks like. `ToX` must be the **same** linear map as the
    // playhead and event body so a sample and a pixel line up. **Not doing:** L/R “fence” lines
    // at the underlap end — the event border from paint order is enough.
    template <typename ToX>
    void drawFrontOverlapShadeAndHatch(
        juce::Graphics& g,
        const juce::Rectangle<float>& frontInner,
        const std::vector<std::pair<std::int64_t, std::int64_t>>& merged,
        ToX&& sessionSampleToX)
    {
        if (frontInner.getWidth() < 1.0f || frontInner.getHeight() < 1.0f)
        {
            return;
        }
        for (const auto& iv : merged)
        {
            const std::int64_t L = iv.first;
            const std::int64_t R = iv.second;
            if (L >= R)
            {
                continue;
            }
            const float xl = sessionSampleToX(L);
            const float xr = sessionSampleToX(R);
            // Intersect the sample span with the **row’s** inner rect so a tiny mismatch or
            // subpixel rounding cannot paint outside the card (hint stays “on” the event).
            const float a = juce::jmax(frontInner.getX(), juce::jmin(xl, xr));
            const float b = juce::jmin(frontInner.getRight(), juce::jmax(xl, xr));
            if (b - a < 0.5f)
            {
                continue;
            }
            const juce::Rectangle<float> band{ a, frontInner.getY(), b - a, frontInner.getHeight() };
            g.setColour(juce::Colours::black.withAlpha(0.10f));
            g.fillRect(band);

            juce::Graphics::ScopedSaveState gsave(g);
            // JUCE: clip to the overlap band so fill + hatching do not spill into adjacent time.
            g.reduceClipRegion(band.toNearestInt());
            // Sparse diagonals: “something else exists here” without a second wave trace. Step is
            // in **screen pixels** so hatch density stays stable when the window (not the session)
            // is resized — we are not tying line spacing to sample count.
            g.setColour(juce::Colour(0xff8eb0d4).withAlpha(0.14f));
            const float step = 7.0f;
            const float h = band.getHeight();
            for (float t = band.getX() - h; t < band.getRight() + h; t += step)
            {
                g.drawLine(t, band.getY(), t + h, band.getBottom(), 0.45f);
            }
        }
    }
} // namespace

bool ClipWaveformView::isTimelineEditGestureInProgress() const noexcept
{
    return pointerLaneMode_ != PointerLaneMode::None || mouseDownPlacedId_.has_value();
}

bool ClipWaveformView::isClipMoveGestureInProgress() const noexcept
{
    return pointerLaneMode_ == PointerLaneMode::MoveClip && mouseDownPlacedId_.has_value();
}

bool ClipWaveformView::isClipTrimGestureInProgress() const noexcept
{
    return (pointerLaneMode_ == PointerLaneMode::TrimLeft
            || pointerLaneMode_ == PointerLaneMode::TrimRight)
        && trimPlacedId_.has_value();
}

std::int64_t ClipWaveformView::snapTimelineSample(std::int64_t sampleOnTimeline) const noexcept
{
    sampleOnTimeline = juce::jmax(std::int64_t{ 0 }, sampleOnTimeline);
    if (laneHost_.snapArrangementTimelineSample != nullptr)
    {
        return laneHost_.snapArrangementTimelineSample(sampleOnTimeline);
    }
    return sampleOnTimeline;
}

ClipWaveformView::ClipWaveformView(
    Session& session,
    Transport& transport,
    const TrackId trackId,
    TimelineViewportModel& timelineViewport,
    AudioWaveformCache& waveformCache,
    ClipWaveformLaneHost laneHost)
    : trackId_(trackId)
    , laneHost_(std::move(laneHost))
    , session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
    , waveformCache_(waveformCache)
{
    jassert(trackId_ != kInvalidTrackId);
    // JUCE: selection/drag; seek is on the timeline ruler, not the empty lane.
    setInterceptsMouseClicks(true, false);
    setOpaque(false);

    // Deferred raster rebuilds are staggered per lane so several audio lanes never run their
    // (few-ms) rebuilds in the same message-loop turn after a zoom gesture settles.
    {
        static int s_laneStaggerSlot = 0;
        deferredRasterRebuildDelayMs_ = 200 + 35 * (s_laneStaggerSlot++ % 5);
    }
}

void ClipWaveformView::setDragGhost(const std::int64_t startSampleOnTimeline, const std::int64_t lengthSamples)
{
    hasDragGhost_ = true;
    dragGhostStartOnTimeline_ = startSampleOnTimeline;
    dragGhostLengthSamples_ = juce::jmax(static_cast<std::int64_t>(0), lengthSamples);
    repaint();
}

void ClipWaveformView::clearDragGhost()
{
    if (!hasDragGhost_)
    {
        return;
    }
    hasDragGhost_ = false;
    repaint();
}

void ClipWaveformView::setRecordingPreviewOverlay(
    const std::int64_t startSampleOnTimeline,
    const std::int64_t lengthSamples,
    const std::vector<RecordingPreviewPeakBlock>& peakBlocks)
{
    clearRecordingCyclePassPreviewLayers();
    recordingPreviewActive_ = true;
    recordingPreviewStartSample_ = startSampleOnTimeline;
    recordingPreviewLengthSamples_ = juce::jmax(std::int64_t{0}, lengthSamples);
    recordingPreviewPeaks_ = peakBlocks;
    repaint();
}

void ClipWaveformView::clearRecordingPreviewOverlay()
{
    const bool hadAny = recordingPreviewActive_ || recordingCycleBehindLayersActive_;
    clearRecordingCyclePassPreviewLayers();

    recordingPreviewActive_ = false;
    recordingPreviewStartSample_ = 0;
    recordingPreviewLengthSamples_ = 0;
    recordingPreviewPeaks_.clear();
    if (hadAny)
    {
        repaint();
    }
}

void ClipWaveformView::setRecordingCyclePassPreviewLayers(
    const std::vector<std::vector<RecordingPreviewPeakBlock>>& completedPassesOlderFirst,
    const std::int64_t firstSegmentTimelineStart,
    const std::int64_t firstSegmentLengthSamples,
    const std::int64_t loopLeftSample,
    const std::int64_t passWindowSamples,
    const std::int64_t currentStartSampleOnTimeline,
    const std::int64_t currentVisibleLengthSamples,
    const std::vector<RecordingPreviewPeakBlock>& currentPeaks)
{
    recordingCycleBehindPasses_ = completedPassesOlderFirst;
    recordingCycleBehindLayersActive_ = !recordingCycleBehindPasses_.empty();
    recordingCycleLoopAnchorL_ = loopLeftSample;
    recordingCyclePassWindowLenSamples_ = juce::jmax(std::int64_t{ 0 }, passWindowSamples);
    recordingCycleFirstSegmentStart_ = firstSegmentTimelineStart;
    recordingCycleFirstSegmentLength_ = juce::jmax(std::int64_t{ 0 }, firstSegmentLengthSamples);

    recordingPreviewActive_ = true;
    recordingPreviewStartSample_ = currentStartSampleOnTimeline;
    recordingPreviewLengthSamples_ = juce::jmax(std::int64_t{ 0 }, currentVisibleLengthSamples);
    recordingPreviewPeaks_ = currentPeaks;
    repaint();
}

void ClipWaveformView::clearRecordingCyclePassPreviewLayers() noexcept
{
    recordingCycleBehindLayersActive_ = false;
    recordingCycleBehindPasses_.clear();
    recordingCycleLoopAnchorL_ = 0;
    recordingCyclePassWindowLenSamples_ = 0;
    recordingCycleFirstSegmentStart_ = 0;
    recordingCycleFirstSegmentLength_ = 0;
}

void ClipWaveformView::setInvalidDropCursor()
{
    if (cursorOverriddenForInvalidDrop_)
    {
        return;
    }
    // `juce::MouseCursor::StandardCursorType` has no portable no-drop; use `ForbiddenCursor.cpp`.
    setMouseCursor(getForbiddenNoDropMouseCursor());
    cursorOverriddenForInvalidDrop_ = true;
}

void ClipWaveformView::restoreNormalCursorAfterInvalidDrop()
{
    if (!cursorOverriddenForInvalidDrop_)
    {
        return;
    }
    setMouseCursor(
        juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
    cursorOverriddenForInvalidDrop_ = false;
}

void ClipWaveformView::publishPlacedClipSelectionToLaneHost() noexcept
{
    if (laneHost_.onPlacedClipSelectionChanged)
    {
        laneHost_.onPlacedClipSelectionChanged(trackId_, selectedPlacedId_);
    }
}

ClipWaveformView::~ClipWaveformView()
{
    restoreNormalCursorAfterInvalidDrop();
}

void ClipWaveformView::applyExternalPlacedClipSelection(const std::optional<PlacedClipId> id) noexcept
{
    selectedPlacedId_ = id;
    publishPlacedClipSelectionToLaneHost();
    repaint();
}

void ClipWaveformView::clearSelectionOnly()
{
    selectedPlacedId_.reset();
    publishPlacedClipSelectionToLaneHost();
    pointerLaneMode_ = PointerLaneMode::None;
    trimPlacedId_.reset();
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    hoverEventTrimCueId_.reset();
    hoverLeftTrimHandleId_.reset();
    hoverRightTrimHandleId_.reset();
    repaint();
}

void ClipWaveformView::cancelInteractionStateForSnapshotRestore()
{
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] ClipWaveformView::cancelInteractionState trackId="
            + juce::String(static_cast<juce::uint64>(trackId_)) + " hadGhost="
            + juce::String(hasDragGhost_ ? "Y" : "n"));
    }

    restoreNormalCursorAfterInvalidDrop();

    // Undo/redo restore invalidates any pending or open inline rename (clip may no longer exist).
    ++renameArmGeneration_;
    dismissInlineClipRename(false);

    selectedPlacedId_.reset();
    publishPlacedClipSelectionToLaneHost();

    pointerLaneMode_ = PointerLaneMode::None;
    trimPlacedId_.reset();
    trimStartSample_ = 0;
    trimMaterialNumSamples_ = 0;
    trimOriginLeft_ = 0;
    trimClickDownVisibleLen_ = 0;
    trimRightEdgeToMouseOffsetSamples_ = 0;
    trimMouseOffsetToTimelineAtClick_ = 0;
    trimPreviewVisibleLen_ = 0;
    trimPreviewLeft_ = 0;
    trimPreviewStart_ = 0;

    mouseDownPlacedId_.reset();
    clickDownX_ = 0.0f;
    clickDownStartSample_ = 0;
    tentativeStartOnTimeline_ = 0;
    dragMovementBeyondThreshold_ = false;
    mouseDownEffectiveNumSamples_ = 0;

    hasDragGhost_ = false;
    dragGhostStartOnTimeline_ = 0;
    dragGhostLengthSamples_ = 0;

    hoverEventTrimCueId_.reset();
    hoverLeftTrimHandleId_.reset();
    hoverRightTrimHandleId_.reset();

    lastSnapshotKey_ = nullptr;
    lastWidth_ = 0;
    lastPeaksFingerprint_ = 0;
    clipStrips_.clear();

    waveRaster_ = juce::Image{};
    waveRasterCoveredStart_ = 0;
    waveRasterCoveredEnd_ = 0;
    waveRasterImageW_ = 0;
    waveRasterImageH_ = 0;
    waveRasterSpp_ = 0.0;
    waveRasterStripFp_ = 0;
    waveRasterPyramidFp_ = 0;
    waveRasterMarginPx_ = 0;
    waveRasterLastRebuildReason_ = WaveformRasterRebuildReason::None;

    repaint();
}

// [Message thread] Click: front-most hit test → select; empty lane → clear selection only (seek
// uses `TimelineRulerView`). Drag (same gesture) is handled in `mouseDrag` / `mouseUp`;
// `Session::moveClip` runs **only** on commit — ordering policy stays in
// `SessionSnapshot::withClipMoved`, not here.
void ClipWaveformView::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
    {
        // Middle button = TrackLanesView hand-pan; never select/edit clips.
        return;
    }
    // Any new press invalidates a pending delayed rename open (fast double clicks never rename).
    ++renameArmGeneration_;
    mouseDownOnSelectedClipNameLabel_ = false;
    if (laneHost_.onBeginMouseDown)
    {
        laneHost_.onBeginMouseDown(*this);
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        return;
    }
    const std::int64_t arrExtent = session_.getArrangementExtentSamples();
    if (arrExtent <= 0)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
    {
        return;
    }
    if (spp <= 0.0)
    {
        return;
    }
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples((double)b.getWidth());

    const juce::Rectangle<float> eventTrackY = b.reduced(0.0f, tc::kEventVerticalMargin);
    const EditTool editTool
        = (laneHost_.getActiveEditTool ? laneHost_.getActiveEditTool() : EditTool::Pointer);
    const LanePixelHit ph
        = (editTool == EditTool::Split)
              ? hitPlacedEventBodyOnlyInLaneAtPixels(
                    snap, tIdx, e.position, b, eventTrackY, visStart, spp)
              : hitPlacedInLaneAtPixels(snap, tIdx, e.position, b, eventTrackY, visStart, spp);
    if (ph.kind == LanePixelHitKind::TrimLeft)
    {
        const PlacedClip& hitPlaced = snap->getTrack(tIdx).getPlacedClip(ph.rowInTrack);
        selectedPlacedId_ = ph.id;
        publishPlacedClipSelectionToLaneHost();
        const std::int64_t S0 = hitPlaced.getStartSample();
        const std::int64_t V0 = hitPlaced.getEffectiveLengthSamples();
        const std::int64_t L0 = hitPlaced.getLeftTrimSamples();
        pointerLaneMode_ = PointerLaneMode::TrimLeft;
        trimPlacedId_ = ph.id;
        trimStartSample_ = S0;
        trimOriginLeft_ = L0;
        trimClickDownVisibleLen_ = V0;
        trimPreviewLeft_ = L0;
        trimPreviewStart_ = S0;
        trimPreviewVisibleLen_ = V0;
        const float tClickL
            = juce::jlimit(0.0f, 1.0f, e.position.x / juce::jmax(1.0f, b.getWidth()));
        const std::int64_t sAtX
            = visStart + (std::int64_t)std::llround((double)tClickL * (double)visLen);
        trimMouseOffsetToTimelineAtClick_ = sAtX - S0;
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::LeftRightResizeCursor));
        repaint();
        return;
    }
    if (ph.kind == LanePixelHitKind::TrimRight)
    {
        const PlacedClip& hitPlaced = snap->getTrack(tIdx).getPlacedClip(ph.rowInTrack);
        selectedPlacedId_ = ph.id;
        publishPlacedClipSelectionToLaneHost();
        const std::int64_t eff = hitPlaced.getEffectiveLengthSamples();
        const std::int64_t a0 = hitPlaced.getStartSample();
        const std::int64_t a1 = a0 + eff;
        pointerLaneMode_ = PointerLaneMode::TrimRight;
        trimPlacedId_ = ph.id;
        trimStartSample_ = a0;
        const int mN = hitPlaced.getMaterialLengthSamples();
        const std::int64_t ltr = hitPlaced.getLeftTrimSamples();
        const std::int64_t tailQ = juce::jmax(
            std::int64_t{0}, static_cast<std::int64_t>(mN) - ltr);
        // Right-trim view mapping: span to file end in timeline == S + (M - L) — 4th arg to `trimViewMappingSpan`.
        trimMaterialNumSamples_ = (int)juce::jmin(
            static_cast<std::int64_t>(std::numeric_limits<int>::max()), tailQ);
        trimClickDownVisibleLen_ = eff;
        trimPreviewVisibleLen_ = eff;
        const std::int64_t mapLen
            = trimViewMappingSpan(visStart, visLen, a0, trimMaterialNumSamples_);
        const double mapLenD = (double)juce::jmax(std::int64_t{1}, mapLen);
        const float tClick
            = juce::jlimit(0.0f, 1.0f, e.position.x / juce::jmax(1.0f, b.getWidth()));
        const std::int64_t sAtX
            = static_cast<std::int64_t>(std::llround(tClick * mapLenD));
        trimRightEdgeToMouseOffsetSamples_ = a1 - sAtX;
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::LeftRightResizeCursor));
        repaint();
        return;
    }
    if (ph.kind == LanePixelHitKind::EventBody)
    {
        const PlacedClip& hitPlaced = snap->getTrack(tIdx).getPlacedClip(ph.rowInTrack);
        if (editTool == EditTool::Split)
        {
            const std::int64_t S = hitPlaced.getStartSample();
            const std::int64_t V = hitPlaced.getEffectiveLengthSamples();
            const float tClickL
                = juce::jlimit(0.0f, 1.0f, e.position.x / juce::jmax(1.0f, b.getWidth()));
            const std::int64_t splitTRaw
                = visStart + (std::int64_t)std::llround((double)tClickL * (double)visLen);
            std::int64_t splitT = snapTimelineSample(splitTRaw);
            if (!(splitT > S && splitT < S + V))
            {
                splitT = juce::jlimit(S + 1, S + V - 1, splitT);
            }
            const bool clipWasSelected
                = selectedPlacedId_.has_value() && *selectedPlacedId_ == ph.id;
            selectedPlacedId_.reset();
            publishPlacedClipSelectionToLaneHost();
            if (splitT > S && splitT < S + V)
            {
                if (laneHost_.commitClipSplitAsUndoable)
                {
                    laneHost_.commitClipSplitAsUndoable(ph.id, splitT, clipWasSelected);
                }
                else
                {
                    (void)session_.splitClip(ph.id, splitT);
                }
            }
            pointerLaneMode_ = PointerLaneMode::None;
            mouseDownPlacedId_.reset();
            dragMovementBeyondThreshold_ = false;
            trimPlacedId_.reset();
            updateTrimHoverAndCursor(e.position);
            repaint();
            return;
        }
        // Explorer-style rename arm: single click on the name label of the already-selected clip.
        if (selectedPlacedId_.has_value() && *selectedPlacedId_ == ph.id
            && e.getNumberOfClicks() == 1)
        {
            const juce::Rectangle<float> nameR
                = tc::clipEventTopLeftNameBounds(eventRectForClipNow(ph.id));
            mouseDownOnSelectedClipNameLabel_ = !nameR.isEmpty() && nameR.contains(e.position);
        }
        selectedPlacedId_ = ph.id;
        publishPlacedClipSelectionToLaneHost();
        const std::int64_t eff = hitPlaced.getEffectiveLengthSamples();
        pointerLaneMode_ = PointerLaneMode::MoveClip;
        mouseDownPlacedId_ = ph.id;
        clickDownX_ = e.position.x;
        dragMovementBeyondThreshold_ = false;
        clickDownStartSample_ = hitPlaced.getStartSample();
        mouseDownEffectiveNumSamples_ = eff;
        tentativeStartOnTimeline_ = clickDownStartSample_;
        updateTrimHoverAndCursor(e.position);
        repaint();
        return;
    }

    selectedPlacedId_.reset();
    publishPlacedClipSelectionToLaneHost();
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    pointerLaneMode_ = PointerLaneMode::None;
    trimPlacedId_.reset();
    updateTrimHoverAndCursor(e.position);
}

void ClipWaveformView::mouseDrag(const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
    {
        return;
    }
    if (pointerLaneMode_ == PointerLaneMode::TrimLeft && trimPlacedId_.has_value())
    {
        const std::int64_t arrExtent = session_.getArrangementExtentSamples();
        if (arrExtent <= 0)
        {
            return;
        }
        const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
        const juce::Rectangle<float> b = getLocalBounds().toFloat();
        if (b.getWidth() <= 0.0f)
        {
            return;
        }
        const std::int64_t visLen
            = timelineViewport_.getVisibleLengthSamples((double)b.getWidth());
        const float tClickL
            = juce::jlimit(0.0f, 1.0f, e.position.x / juce::jmax(1.0f, b.getWidth()));
        const std::int64_t sAtX
            = visStart + (std::int64_t)std::llround((double)tClickL * (double)visLen);
        const std::int64_t newS = sAtX - trimMouseOffsetToTimelineAtClick_;
        std::int64_t d
            = juce::jlimit(
                juce::jmax(-trimOriginLeft_, -trimStartSample_),
                trimClickDownVisibleLen_ - 1,
                newS - trimStartSample_);
        std::int64_t snappedStart = snapTimelineSample(trimStartSample_ + d);
        d = snappedStart - trimStartSample_;
        d = juce::jlimit(
            juce::jmax(-trimOriginLeft_, -trimStartSample_),
            trimClickDownVisibleLen_ - 1,
            d);
        trimPreviewLeft_ = trimOriginLeft_ + d;
        trimPreviewStart_ = trimStartSample_ + d;
        trimPreviewVisibleLen_ = trimClickDownVisibleLen_ - d;
        repaint();
        return;
    }
    if (pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value())
    {
        const std::int64_t arrExtent = session_.getArrangementExtentSamples();
        if (arrExtent <= 0)
        {
            return;
        }
        const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
        const juce::Rectangle<float> b = getLocalBounds().toFloat();
        if (b.getWidth() <= 0.0f)
        {
            return;
        }
        const std::int64_t visLen
            = timelineViewport_.getVisibleLengthSamples((double)b.getWidth());
        const std::int64_t mapLen = trimViewMappingSpan(
            visStart, visLen, trimStartSample_, trimMaterialNumSamples_);
        const double mapLenD = (double)juce::jmax(std::int64_t{1}, mapLen);
        const float tClick = juce::jlimit(0.0f, 1.0f, e.position.x / juce::jmax(1.0f, b.getWidth()));
        const std::int64_t sAtX
            = static_cast<std::int64_t>(std::llround(tClick * mapLenD));
        const std::int64_t newRightEdge = sAtX + trimRightEdgeToMouseOffsetSamples_;
        const int matN = juce::jmax(0, trimMaterialNumSamples_);
        if (matN <= 0)
        {
            return;
        }
        const std::int64_t cap = static_cast<std::int64_t>(matN);
        trimPreviewVisibleLen_
            = juce::jlimit(std::int64_t{1}, cap, newRightEdge - trimStartSample_);
        {
            const std::int64_t rightEdge = trimStartSample_ + trimPreviewVisibleLen_;
            const std::int64_t snappedRight = snapTimelineSample(rightEdge);
            trimPreviewVisibleLen_
                = juce::jlimit(std::int64_t{1}, cap, snappedRight - trimStartSample_);
        }
        repaint();
        return;
    }
    if (!mouseDownPlacedId_.has_value())
    {
        return;
    }
    const std::int64_t arrExtent = session_.getArrangementExtentSamples();
    if (arrExtent <= 0)
    {
        return;
    }
    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
    {
        return;
    }
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const float dx = e.position.x - clickDownX_;
    if (std::abs(dx) >= kDragThresholdPx)
    {
        dragMovementBeyondThreshold_ = true;
    }
    const double deltaS = (double)dx * spp;
    tentativeStartOnTimeline_ = juce::jmax(
        static_cast<std::int64_t>(0), clickDownStartSample_ + static_cast<std::int64_t>(std::llround(deltaS)));
    tentativeStartOnTimeline_ = snapTimelineSample(tentativeStartOnTimeline_);

    const bool canCrossLane = static_cast<bool>(laneHost_.findLaneAtScreen)
                              && static_cast<bool>(laneHost_.setGhostOnLane)
                              && static_cast<bool>(laneHost_.clearAllGhosts);
    if (!canCrossLane)
    {
        repaint();
        return;
    }
    if (!dragMovementBeyondThreshold_)
    {
        if (laneHost_.clearAllGhosts)
        {
            laneHost_.clearAllGhosts();
        }
        restoreNormalCursorAfterInvalidDrop();
        repaint();
        return;
    }

    auto* const lane
        = laneHost_.findLaneAtScreen(juce::Point<int>(e.getScreenX(), e.getScreenY()));
    if (lane == nullptr)
    {
        if (laneHost_.clearAllGhosts)
        {
            laneHost_.clearAllGhosts();
        }
        setInvalidDropCursor();
    }
    else if (lane == this)
    {
        if (laneHost_.clearAllGhosts)
        {
            laneHost_.clearAllGhosts();
        }
        restoreNormalCursorAfterInvalidDrop();
    }
    else
    {
        if (laneHost_.setGhostOnLane)
        {
            laneHost_.setGhostOnLane(
                lane, tentativeStartOnTimeline_, static_cast<std::int64_t>(mouseDownEffectiveNumSamples_));
        }
        restoreNormalCursorAfterInvalidDrop();
    }
    repaint();
}

// [Message thread] Commit: `Session::moveClip` (same lane) or `Session::moveClipToTrack` (other
// lane) if the user actually dragged. Pointer outside the lane stack on release cancels: no
// publish. Clears cross-lane ghosts and restores invalid-drop cursor on the source lane.
void ClipWaveformView::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
    {
        return;
    }
    if (laneHost_.clearAllGhosts)
    {
        laneHost_.clearAllGhosts();
    }
    restoreNormalCursorAfterInvalidDrop();

    if (pointerLaneMode_ == PointerLaneMode::TrimLeft)
    {
        if (trimPlacedId_.has_value() && trimPreviewLeft_ != trimOriginLeft_)
        {
            bool didPublish = false;
            if (laneHost_.commitClipTrimAsUndoable)
            {
                didPublish = laneHost_.commitClipTrimAsUndoable(
                    *trimPlacedId_, ClipTrimEdge::Left, trimPreviewLeft_);
            }
            else
            {
                session_.setClipLeftEdgeTrim(*trimPlacedId_, trimPreviewLeft_);
                didPublish = true;
            }
            if (didPublish)
            {
                const double tw = (double)juce::jmax(1, getWidth());
                timelineViewport_.clampToExtent(tw, session_.getArrangementExtentSamples());
            }
        }
        trimPlacedId_.reset();
        pointerLaneMode_ = PointerLaneMode::None;
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        updateTrimHoverAndCursor(e.position);
        repaint();
        return;
    }
    if (pointerLaneMode_ == PointerLaneMode::TrimRight)
    {
        if (trimPlacedId_.has_value() && trimPreviewVisibleLen_ != trimClickDownVisibleLen_)
        {
            bool didPublish = false;
            if (laneHost_.commitClipTrimAsUndoable)
            {
                didPublish = laneHost_.commitClipTrimAsUndoable(
                    *trimPlacedId_, ClipTrimEdge::Right, trimPreviewVisibleLen_);
            }
            else
            {
                session_.setClipRightEdgeVisibleLength(*trimPlacedId_, trimPreviewVisibleLen_);
                didPublish = true;
            }
            if (didPublish)
            {
                const double tw = (double)juce::jmax(1, getWidth());
                timelineViewport_.clampToExtent(tw, session_.getArrangementExtentSamples());
            }
        }
        trimPlacedId_.reset();
        pointerLaneMode_ = PointerLaneMode::None;
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        updateTrimHoverAndCursor(e.position);
        repaint();
        return;
    }

    if (mouseDownPlacedId_.has_value() && dragMovementBeyondThreshold_)
    {
        const PlacedClipId movedId = *mouseDownPlacedId_;
        const bool canCrossLane = static_cast<bool>(laneHost_.findLaneAtScreen);
        ClipWaveformView* destLane = this;
        if (canCrossLane)
        {
            destLane = laneHost_.findLaneAtScreen(juce::Point<int>(e.getScreenX(), e.getScreenY()));
        }

        bool didPublish = false;
        if (destLane != nullptr)
        {
            const bool sameLane = (destLane == this);
            const bool sameStart = (tentativeStartOnTimeline_ == clickDownStartSample_);
            const bool isNoop = sameLane && sameStart;

            if (isNoop)
            {
                session_.moveClip(movedId, tentativeStartOnTimeline_);
                didPublish = true;
            }
            else if (laneHost_.commitClipMoveAsUndoable)
            {
                didPublish = laneHost_.commitClipMoveAsUndoable(
                    movedId,
                    tentativeStartOnTimeline_,
                    sameLane ? std::nullopt : std::optional<TrackId>(destLane->getTrackId()));
            }
            else
            {
                if (sameLane)
                {
                    session_.moveClip(movedId, tentativeStartOnTimeline_);
                }
                else
                {
                    session_.moveClipToTrack(
                        movedId, tentativeStartOnTimeline_, destLane->getTrackId());
                }
                didPublish = true;
            }
        }

        if (didPublish)
        {
            const double tw = (double)juce::jmax(1, getWidth());
            timelineViewport_.clampToExtent(tw, session_.getArrangementExtentSamples());
        }
    }
    if (mouseDownPlacedId_.has_value() && !dragMovementBeyondThreshold_
        && pointerLaneMode_ == PointerLaneMode::MoveClip && mouseDownOnSelectedClipNameLabel_
        && e.getNumberOfClicks() == 1)
    {
        // Slow second click on the selected clip's name label: open the inline rename editor after
        // the double-click window has passed (a newer press bumps the generation and cancels this).
        const PlacedClipId cid = *mouseDownPlacedId_;
        const int gen = renameArmGeneration_;
        juce::Component::SafePointer<ClipWaveformView> safe(this);
        juce::Timer::callAfterDelay(tc::kClipRenameSecondClickDelayMs, [safe, gen, cid] {
            if (safe != nullptr && gen == safe->renameArmGeneration_)
            {
                safe->beginInlineClipRename(cid);
            }
        });
    }
    mouseDownOnSelectedClipNameLabel_ = false;
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    pointerLaneMode_ = PointerLaneMode::None;
    updateTrimHoverAndCursor(e.position);
    repaint();
}

juce::Rectangle<float> ClipWaveformView::eventRectForClipNow(const PlacedClipId clipId) const
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || clipId == kInvalidPlacedClipId)
    {
        return {};
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        return {};
    }
    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (b.getWidth() <= 0.0f || spp <= 0.0)
    {
        return {};
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const juce::Rectangle<float> eventTrackY = b.reduced(0.0f, tc::kEventVerticalMargin);
    const Track& tr = snap->getTrack(tIdx);
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        const PlacedClip& pc = tr.getPlacedClip(i);
        if (pc.getId() != clipId)
        {
            continue;
        }
        const std::int64_t a0 = pc.getStartSample();
        const std::int64_t a1 = a0 + pc.getEffectiveLengthSamples();
        if (a0 >= a1)
        {
            return {};
        }
        const float ex0
            = TimelineRulerView::sessionSampleToLocalX(a0, b.getX(), visStart, spp);
        const float ex1
            = TimelineRulerView::sessionSampleToLocalX(a1, b.getX(), visStart, spp);
        const float x0 = juce::jmin(ex0, ex1);
        const float x1 = juce::jmax(ex0, ex1);
        return { x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0), eventTrackY.getHeight() };
    }
    return {};
}

void ClipWaveformView::beginInlineClipRename(const PlacedClipId clipId)
{
    if (clipId == kInvalidPlacedClipId || pointerLaneMode_ != PointerLaneMode::None)
    {
        return;
    }
    const juce::Rectangle<float> eventRect = eventRectForClipNow(clipId);
    if (eventRect.isEmpty())
    {
        return;
    }
    juce::String seed;
    for (const TimelineStrip& strip : clipStrips_)
    {
        if (strip.clipId == clipId)
        {
            seed = clipDisplayLabelForStrip(strip);
            break;
        }
    }
    dismissInlineClipRename(false);

    if (clipRenameEditor_ == nullptr)
    {
        clipRenameEditor_ = std::make_unique<juce::TextEditor>("clipRenameEdit");
        clipRenameEditor_->setMultiLine(false);
        clipRenameEditor_->setReturnKeyStartsNewLine(false);
        clipRenameEditor_->setFont(juce::Font(juce::FontOptions().withHeight(12.0f)));
        clipRenameEditor_->setSelectAllWhenFocused(true);
        clipRenameEditor_->onReturnKey = [this] { dismissInlineClipRename(true); };
        clipRenameEditor_->onEscapeKey = [this] { dismissInlineClipRename(false); };
        // Focus loss commits — same convention as track-header rename.
        clipRenameEditor_->onFocusLost = [this] { dismissInlineClipRename(true); };
        addChildComponent(*clipRenameEditor_);
    }

    renameEditingClipId_ = clipId;
    clipRenameEditor_->setText(seed, false);
    juce::Rectangle<int> box = tc::clipEventTopLeftNameBounds(eventRect).toNearestInt();
    box.setHeight(20);
    box.setWidth(juce::jmax(box.getWidth(), 96));
    box = box.constrainedWithin(getLocalBounds().reduced(1));
    clipRenameEditor_->setBounds(box);
    clipRenameEditor_->setVisible(true);
    clipRenameEditor_->toFront(false);
    clipRenameEditor_->grabKeyboardFocus();
    repaint();
}

void ClipWaveformView::dismissInlineClipRename(const bool commit)
{
    if (clipRenameEditor_ == nullptr || !clipRenameEditor_->isVisible() || renameDismissInProgress_)
    {
        return;
    }
    renameDismissInProgress_ = true;
    const PlacedClipId cid = renameEditingClipId_;
    const juce::String text = clipRenameEditor_->getText().trim();
    renameEditingClipId_ = kInvalidPlacedClipId;
    clipRenameEditor_->setVisible(false);

    // Empty text = cancel (keep the old name): the safest behavior for accidental clears.
    if (commit && cid != kInvalidPlacedClipId && text.isNotEmpty())
    {
        juce::String currentLabel;
        for (const TimelineStrip& strip : clipStrips_)
        {
            if (strip.clipId == cid)
            {
                currentLabel = clipDisplayLabelForStrip(strip);
                break;
            }
        }
        if (text != currentLabel)
        {
            if (laneHost_.commitClipRenameAsUndoable)
            {
                (void)laneHost_.commitClipRenameAsUndoable(cid, text);
            }
            else
            {
                session_.setPlacedClipName(cid, text);
            }
        }
    }
    renameDismissInProgress_ = false;
    repaint();
}

void ClipWaveformView::clearSelectionIfIdMissing(
    const std::shared_ptr<const SessionSnapshot>& snap)
{
    if (!selectedPlacedId_.has_value())
    {
        return;
    }
    if (snap == nullptr)
    {
        selectedPlacedId_.reset();
        publishPlacedClipSelectionToLaneHost();
        return;
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        selectedPlacedId_.reset();
        publishPlacedClipSelectionToLaneHost();
        return;
    }
    const Track& tr = snap->getTrack(tIdx);
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        if (tr.getPlacedClip(i).getId() == *selectedPlacedId_)
        {
            return;
        }
    }
    selectedPlacedId_.reset();
    hoverEventTrimCueId_.reset();
    hoverLeftTrimHandleId_.reset();
    hoverRightTrimHandleId_.reset();
    publishPlacedClipSelectionToLaneHost();
}

// [Message thread] Fills `clipStrips_` with per-row timeline + material handles. Waveform **pixels**
// come from `AudioWaveformCache` pyramids in `paint` — this pass stays free of PCM walks.
void ClipWaveformView::syncClipStripsFromSnapshotIfNeeded()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    const int w = juce::jmax(1, getWidth());

    std::uint64_t fp = 0;
    if (snap != nullptr)
    {
        const int tI = snap->findTrackIndexById(trackId_);
        if (tI >= 0)
        {
            const auto& t = snap->getTrack(tI);
            for (int j = 0; j < t.getNumPlacedClips(); ++j)
            {
                const PlacedClip& p = t.getPlacedClip(j);
                fp ^= (std::uint64_t)p.getId() * 0x9e3779b9ull;
                fp ^= (std::uint64_t)(p.getLeftTrimSamples() + 0x1e35) * 0xc6a4a7935bd1e995ull;
                fp ^= (std::uint64_t)(p.getStartSample() + 0x9e37) * 0xc2b2ae3d27d4eb4full;
                fp ^= (std::uint64_t)(p.getEffectiveLengthSamples() + 0xbf58) * 0x94d049bb133111ebull;
                if (p.getMaterial() != nullptr)
                {
                    fp ^= (std::uint64_t)(std::uintptr_t)p.getMaterial().get() * 0x85ebca6bull;
                }
            }
        }
    }
    if (snap.get() == lastSnapshotKey_ && w == lastWidth_ && fp == lastPeaksFingerprint_)
    {
        return;
    }
    lastPeaksFingerprint_ = fp;

    lastSnapshotKey_ = snap.get();
    lastWidth_ = w;
    clipStrips_.clear();
    clearSelectionIfIdMissing(snap);

    if (snap == nullptr)
    {
        selectedPlacedId_.reset();
        publishPlacedClipSelectionToLaneHost();
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        return;
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        selectedPlacedId_.reset();
        publishPlacedClipSelectionToLaneHost();
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        return;
    }
    if (snap->isEmpty())
    {
        selectedPlacedId_.reset();
        publishPlacedClipSelectionToLaneHost();
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        return;
    }

    const std::int64_t timelineEndExcl = snap->getDerivedTimelineLengthSamples();
    if (timelineEndExcl <= 0)
    {
        return;
    }

    const auto& tr = snap->getTrack(tIdx);
    const int n = tr.getNumPlacedClips();
    clipStrips_.reserve((size_t)n);

    for (int i = 0; i < n; ++i)
    {
        TimelineStrip strip;
        const PlacedClip& placed = tr.getPlacedClip(i);
        strip.clipId = placed.getId();
        strip.startOnTimeline = placed.getStartSample();
        strip.leftTrimSamples = placed.getLeftTrimSamples();
        strip.material = placed.getMaterial();
        strip.materialWindowStart = placed.getMaterialWindowStartSamples();
        strip.materialWindowEndExcl = placed.getMaterialWindowEndExclusiveSamples();
        strip.displayName = placed.getDisplayName();
        {
            const std::int64_t eff = placed.getEffectiveLengthSamples();
            strip.materialNumSamples
                = static_cast<int>(juce::jmin(
                    static_cast<std::int64_t>(std::numeric_limits<int>::max()), eff));
        }

        clipStrips_.push_back(std::move(strip));
    }
}

bool ClipWaveformView::shouldBypassWaveformRasterCache() const noexcept
{
    if (recordingPreviewActive_ || recordingCycleBehindLayersActive_)
    {
        return true;
    }
    if (hasDragGhost_)
    {
        return true;
    }
    if (pointerLaneMode_ != PointerLaneMode::None)
    {
        return true;
    }
    if (mouseDownPlacedId_.has_value() && dragMovementBeyondThreshold_)
    {
        return true;
    }
    return false;
}

bool ClipWaveformView::visibleFitsWaveRaster(const std::int64_t visStart, const std::int64_t visLen) const noexcept
{
    if (waveRaster_.isNull())
    {
        return false;
    }
    return visStart >= waveRasterCoveredStart_ && visStart + visLen <= waveRasterCoveredEnd_;
}

void ClipWaveformView::blitWaveRasterApproximate(juce::Graphics& g,
                                                 const std::int64_t visStart,
                                                 const std::int64_t visLen,
                                                 const double spp) const
{
    if (waveRaster_.isNull() || waveRasterSpp_ <= 0.0 || waveRasterImageW_ <= 0
        || waveRasterImageH_ <= 0 || visLen <= 0)
    {
        return;
    }
    const int vw = juce::jmax(1, getWidth());
    const int vh = juce::jmax(1, getHeight());

    // Exact-geometry fast case: pixel-true 1:1 blit (no resampling blur in steady state).
    if (waveRasterSpp_ == spp && waveRasterImageH_ == vh && visibleFitsWaveRaster(visStart, visLen))
    {
        int srcX = juce::roundToInt((double)(visStart - waveRasterCoveredStart_) / waveRasterSpp_);
        srcX = juce::jmax(0, juce::jmin(srcX, juce::jmax(0, waveRasterImageW_ - vw)));
        g.drawImage(waveRaster_, 0, 0, vw, vh, srcX, 0, vw, vh);
        return;
    }

    // Geometry-stale case (zoom/pan/resize before the deferred rebuild lands): map the current
    // visible sample range onto the old raster's coverage and draw the covered part scaled.
    // Uncovered regions stay unpainted (lane background) rather than showing stretched garbage.
    const double srcX0 = (double)(visStart - waveRasterCoveredStart_) / waveRasterSpp_;
    const double srcX1 = (double)(visStart + visLen - waveRasterCoveredStart_) / waveRasterSpp_;
    if (srcX1 <= srcX0)
    {
        return;
    }
    const double clampedS0 = juce::jmax(0.0, srcX0);
    const double clampedS1 = juce::jmin((double)waveRasterImageW_, srcX1);
    if (clampedS1 <= clampedS0)
    {
        return;
    }
    const double destPerSrc = (double)vw / (srcX1 - srcX0);
    const int dx0 = (int)std::floor((clampedS0 - srcX0) * destPerSrc);
    const int dx1 = (int)std::ceil((clampedS1 - srcX0) * destPerSrc);
    const int sx0 = (int)std::floor(clampedS0);
    const int sx1 = (int)std::ceil(clampedS1);
    g.drawImage(waveRaster_,
                dx0,
                0,
                juce::jmax(1, dx1 - dx0),
                vh,
                sx0,
                0,
                juce::jmax(1, juce::jmin(waveRasterImageW_, sx1) - sx0),
                waveRasterImageH_);
}

void ClipWaveformView::scheduleDeferredRasterRebuild()
{
    // Restarting the countdown on every geometry-stale paint is the generation token: an active
    // zoom/pan gesture keeps pushing the rebuild out, and the timer always rebuilds for whatever
    // the viewport says *when it fires* — obsolete intermediate spp values are never rasterized.
    startTimer(deferredRasterRebuildDelayMs_);
}

void ClipWaveformView::timerCallback()
{
    stopTimer();
    if (shouldBypassWaveformRasterCache())
    {
        // Gesture/recording previews bypass the raster; the next normal paint reschedules if needed.
        return;
    }
    const juce::Rectangle<float> bounds = getLocalBounds().toFloat();
    const double spp = timelineViewport_.getSamplesPerPixel();
    const std::int64_t arrLen = session_.getArrangementExtentSamples();
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f || spp <= 0.0 || arrLen <= 0)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples((double)bounds.getWidth());

    syncClipStripsFromSnapshotIfNeeded();
    MINIDAW_UI_PAINT_COUNT(clipLaneDeferredBuilds);
    (void)ensureWaveRasterForViewState(bounds, visStart, visLen, spp, arrLen);
    repaint();
}

std::uint64_t ClipWaveformView::computePyramidReadyFingerprint() const
{
    std::uint64_t fp = 0;
    for (const auto& s : clipStrips_)
    {
        if (s.material == nullptr)
        {
            continue;
        }
        const bool ready = waveformCache_.isPyramidReady(s.material.get());
        fp ^= (std::uint64_t)(std::uintptr_t)s.material.get() * 0x9e3779b97f4a7c15ull;
        fp ^= ready ? 0x85ebca6b932f5c01ull : 0x1271fd5ce733fb7bull;
    }
    return fp;
}

void ClipWaveformView::computeWaveRasterLayout(const int viewWpx,
                                               const std::int64_t visStart,
                                               const std::int64_t visLen,
                                               const double spp,
                                               int& outMarginPx,
                                               std::int64_t& outCoveredStart,
                                               int& outImageW) const
{
    juce::ignoreUnused(visLen);
    outMarginPx = juce::roundToInt(0.75 * (double)juce::jmax(1, viewWpx));
    outMarginPx = juce::jmax(1, outMarginPx);
    outImageW = viewWpx + 2 * outMarginPx;
    const std::int64_t marginSamples = (std::int64_t)std::llround((double)outMarginPx * spp);
    outCoveredStart = visStart - marginSamples;
    if (outCoveredStart < 0)
    {
        outCoveredStart = 0;
    }
}

bool ClipWaveformView::ensureWaveRasterForViewState(const juce::Rectangle<float>& bounds,
                                                    const std::int64_t visStart,
                                                    const std::int64_t visLen,
                                                    const double spp,
                                                    const std::int64_t arrExtent)
{
    juce::ignoreUnused(bounds, arrExtent);
    const int vw = juce::jmax(0, getWidth());
    const int vh = juce::jmax(0, getHeight());
    if (vw <= 0 || vh <= 0)
    {
        return false;
    }

    const std::uint64_t stripFp = lastPeaksFingerprint_;
    const std::uint64_t pyrFp = computePyramidReadyFingerprint();

    int marginPx = 0;
    std::int64_t coveredStart = 0;
    int imageW = 0;
    computeWaveRasterLayout(vw, visStart, visLen, spp, marginPx, coveredStart, imageW);

    const std::int64_t coveredEnd = coveredStart + (std::int64_t)std::llround((double)imageW * spp);

    bool needRebuild = waveRaster_.isNull() || waveRasterImageW_ != imageW || waveRasterImageH_ != vh
                       || waveRasterSpp_ != spp || waveRasterStripFp_ != stripFp
                       || waveRasterPyramidFp_ != pyrFp;

    WaveformRasterRebuildReason reason = WaveformRasterRebuildReason::None;
    if (needRebuild)
    {
        if (waveRaster_.isNull())
        {
            reason = WaveformRasterRebuildReason::First;
        }
        else if (waveRasterImageW_ != imageW || waveRasterImageH_ != vh)
        {
            reason = WaveformRasterRebuildReason::Size;
        }
        else if (waveRasterSpp_ != spp)
        {
            reason = WaveformRasterRebuildReason::Zoom;
        }
        else if (waveRasterStripFp_ != stripFp)
        {
            reason = WaveformRasterRebuildReason::Snapshot;
        }
        else
        {
            reason = WaveformRasterRebuildReason::Pyramid;
        }
    }

    if (!needRebuild && !visibleFitsWaveRaster(visStart, visLen))
    {
        needRebuild = true;
        reason = WaveformRasterRebuildReason::OverscanRange;
    }

    if (!needRebuild)
    {
        waveRasterLastRebuildReason_ = WaveformRasterRebuildReason::None;
        return true;
    }

    waveRasterLastRebuildReason_ = reason;

#if MINIDAW_DIAG_PLAYBACK_UI_LOAD
    const std::int64_t rebuildT0 = juce::Time::getHighResolutionTicks();
#endif
    // Reuse the existing allocation when the size is unchanged (typical zoom-only rebuild):
    // repeated multi-MB image reallocation was part of the zoom spike.
    if (waveRaster_.isValid() && waveRaster_.getWidth() == imageW && waveRaster_.getHeight() == vh)
    {
        waveRaster_.clear(waveRaster_.getBounds());
    }
    else
    {
        waveRaster_ = juce::Image(juce::Image::PixelFormat::ARGB, imageW, vh, true);
    }
    {
        juce::Graphics gr(waveRaster_);
        const juce::Rectangle<float> imgBounds(0.0f, 0.0f, (float)imageW, (float)vh);
        paintStableCommittedLayer(gr, imgBounds, coveredStart, visLen, spp);
    }
#if MINIDAW_DIAG_PLAYBACK_UI_LOAD
    MINIDAW_UI_PAINT_COUNT(clipLaneRasterRebuilds);
    MINIDAW_UI_PAINT_ADD(clipLaneRasterRebuildUs,
                         juce::Time::highResolutionTicksToSeconds(
                             juce::Time::getHighResolutionTicks() - rebuildT0)
                             * 1.0e6);
#endif

    waveRasterCoveredStart_ = coveredStart;
    waveRasterCoveredEnd_ = coveredEnd;
    waveRasterImageW_ = imageW;
    waveRasterImageH_ = vh;
    waveRasterSpp_ = spp;
    waveRasterStripFp_ = stripFp;
    waveRasterPyramidFp_ = pyrFp;
    waveRasterMarginPx_ = marginPx;
    return true;
}

void ClipWaveformView::paintStableCommittedLayer(juce::Graphics& g,
                                                 const juce::Rectangle<float>& bounds,
                                                 const std::int64_t mappingVisStart,
                                                 const std::int64_t visLen,
                                                 const double spp)
{
    juce::ignoreUnused(visLen);
    const auto sessionSampleToX = [&](const std::int64_t s) {
        return TimelineRulerView::sessionSampleToLocalX(s, bounds.getX(), mappingVisStart, spp);
    };
    const int numRows = (int)clipStrips_.size();
    const juce::Rectangle<float> eventTrackY = bounds.reduced(0.0f, tc::kEventVerticalMargin);
    const float midY = eventTrackY.getCentreY();
    const float halfDraw = juce::jmax(1.0f, eventTrackY.getHeight() * 0.5f) * 0.45f;
    for (int row = numRows - 1; row >= 0; --row)
    {
        const TimelineStrip& strip = clipStrips_[(size_t)row];
        if (strip.materialNumSamples <= 0)
        {
            continue;
        }
        const int nsForDraw = strip.materialNumSamples;
        const std::int64_t startForDraw = strip.startOnTimeline;
        const float ex0 = TimelineRulerView::sessionSampleToLocalX(
            startForDraw, bounds.getX(), mappingVisStart, spp);
        const float ex1 = TimelineRulerView::sessionSampleToLocalX(
            startForDraw + static_cast<std::int64_t>(nsForDraw), bounds.getX(), mappingVisStart, spp);
        const float x0 = juce::jmin(ex0, ex1);
        const float x1 = juce::jmax(ex0, ex1);
        juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0),
                                            eventTrackY.getHeight() };

        if (eventRect.getRight() < bounds.getX() || eventRect.getX() > bounds.getRight())
        {
            continue;
        }

        tc::paintEventChromeBody(g, eventRect, tc::unifiedClipEventBodyFill());

        juce::Rectangle<float> innerForPeakHeight = eventRect.reduced(0.0f, 1.0f + kWaveInset * 0.5f);
        if (eventRect.getWidth() >= 1.0f && innerForPeakHeight.getHeight() >= 1.0f && nsForDraw > 0)
        {
            paintRowWaveformWithPyramid(
                g,
                row,
                strip,
                eventRect,
                innerForPeakHeight,
                midY,
                halfDraw,
                startForDraw,
                strip.leftTrimSamples,
                nsForDraw,
                bounds,
                mappingVisStart,
                spp);
        }
    }

    for (int r = numRows - 1; r >= 0; --r)
    {
        const TimelineStrip& stripR = clipStrips_[(size_t)r];
        if (stripR.materialNumSamples <= 0)
        {
            continue;
        }
        std::vector<std::pair<std::int64_t, std::int64_t>> olap;
        computeLocalOverlapShadeHalfOpenIntervalsForRow(r, olap);
        if (olap.empty())
        {
            continue;
        }
        const int nsH = stripR.materialNumSamples;
        const std::int64_t startHatch = stripR.startOnTimeline;
        const float rex0
            = TimelineRulerView::sessionSampleToLocalX(startHatch, bounds.getX(), mappingVisStart, spp);
        const float rex1 = TimelineRulerView::sessionSampleToLocalX(
            startHatch + static_cast<std::int64_t>(juce::jmax(0, nsH)), bounds.getX(), mappingVisStart, spp);
        const float rxl = juce::jmin(rex0, rex1);
        const float rxr = juce::jmax(rex0, rex1);
        juce::Rectangle<float> rowEventRect{ rxl, eventTrackY.getY(), juce::jmax(1.0f, rxr - rxl),
                                               eventTrackY.getHeight() };
        if (rowEventRect.getRight() < bounds.getX() || rowEventRect.getX() > bounds.getRight())
        {
            continue;
        }
        juce::Rectangle<float> rowInner
            = rowEventRect.reduced(1.0f + kWaveInset, 1.0f + kWaveInset * 0.5f);
        if (rowInner.getWidth() >= 1.0f && rowInner.getHeight() >= 1.0f)
        {
            drawFrontOverlapShadeAndHatch(g, rowInner, olap, sessionSampleToX);
        }
    }
}

void ClipWaveformView::paintRowWaveformWithPyramid(
    juce::Graphics& g,
    const int row,
    const TimelineStrip& strip,
    const juce::Rectangle<float>& eventRect,
    const juce::Rectangle<float>& innerForPeakHeight,
    const float midY,
    const float halfDraw,
    const std::int64_t timelineStartForWaveform,
    const std::int64_t materialFileLeft,
    const int visibleMaterialSamples,
    const juce::Rectangle<float>& mappingBounds,
    const std::int64_t mappingVisStart,
    const double mappingSpp)
{
    juce::ignoreUnused(mappingVisStart, mappingSpp);
    if (visibleMaterialSamples <= 0 || strip.material == nullptr)
    {
        return;
    }
    const float cullL = mappingBounds.getX();
    const float cullR = mappingBounds.getRight();
    const float runW = eventRect.getWidth();
    if (runW < 0.5f || innerForPeakHeight.getHeight() < 1.0f)
    {
        return;
    }

    auto pyramid = waveformCache_.getOrEnqueue(strip.material);
    if (pyramid == nullptr)
    {
        g.setColour(juce::Colours::lightblue.withAlpha(0.22f));
        g.drawLine(eventRect.getX(), midY, eventRect.getRight(), midY, 1.0f);
        return;
    }
    const int nSrc = pyramid->getNumSourceSamples();
    if (nSrc <= 0)
    {
        return;
    }

    const float drawL = juce::jmax(cullL, eventRect.getX());
    const float drawR = juce::jmin(cullR, eventRect.getRight());
    if (drawR - drawL < 0.25f)
    {
        return;
    }

    const int x0 = (int)std::floor(drawL);
    const int x1 = (int)std::ceil(drawR);
    const double nsD = (double)juce::jmax(1, visibleMaterialSamples);
    const std::int64_t win0 = strip.materialWindowStart;
    const std::int64_t win1 = strip.materialWindowEndExcl;

    for (int xi = x0; xi < x1; ++xi)
    {
        const float px0 = (float)xi;
        const float px1 = (float)xi + 1.0f;
        const float segL = juce::jmax(eventRect.getX(), px0);
        const float segR = juce::jmin(eventRect.getRight(), px1);
        if (segR - segL < 0.01f)
        {
            continue;
        }
        double t0 = ((double)segL - (double)eventRect.getX()) / (double)runW;
        double t1 = ((double)segR - (double)eventRect.getX()) / (double)runW;
        t0 = juce::jlimit(0.0, 1.0, t0);
        t1 = juce::jlimit(0.0, 1.0, t1);
        if (t0 >= t1)
        {
            continue;
        }
        std::int64_t m0 = (std::int64_t)std::floor(t0 * nsD);
        std::int64_t m1 = (std::int64_t)std::ceil(t1 * nsD);
        m0 = juce::jlimit(std::int64_t{ 0 }, (std::int64_t)visibleMaterialSamples, m0);
        m1 = juce::jlimit(std::int64_t{ 0 }, (std::int64_t)visibleMaterialSamples, juce::jmax(m0 + 1, m1));

        std::int64_t f0 = materialFileLeft + m0;
        std::int64_t f1 = materialFileLeft + m1;
        f0 = juce::jmax(f0, win0);
        f1 = juce::jmin(f1, win1);
        f0 = juce::jlimit(std::int64_t{ 0 }, (std::int64_t)nSrc, f0);
        f1 = juce::jlimit(std::int64_t{ 0 }, (std::int64_t)nSrc, juce::jmax(f0 + 1, f1));

        const std::int64_t mmid = m0 + (m1 - m0) / 2;
        const std::int64_t tOnTimeline = timelineStartForWaveform + mmid;
        if (isTimelineSampleCoveredByPriorRows(row, tOnTimeline))
        {
            continue;
        }

        float mn = 0.0f;
        float mx = 0.0f;
        pyramid->queryMinMaxForFileRange(f0, f1, mn, mx);
        fillMinMaxPeakRectClamped(
            g,
            mn,
            mx,
            halfDraw,
            innerForPeakHeight,
            midY,
            segL,
            juce::jmax(1.0f, segR - segL),
            juce::Colours::lightblue.withAlpha(kWaveformPeakAlpha));
    }
}

// [Message thread] **Paint rule, not the mix rule:** if any *newer* `PlacedClip` (smaller index `k`
// than this row) **covers** timeline sample `t`, a peak bar for `row` at that time would be drawn
// *under* that newer row’s event and would read as a false second trace — we return true to **skip**
// drawing that bar. `PlaybackEngine` decides what you *hear* (first covering row in snapshot order);
// here we only mirror **JUCE** z-order: lower index = painted later = on top.
bool ClipWaveformView::isTimelineSampleCoveredByPriorRows(int row, const std::int64_t t) const
{
    for (int k = 0; k < row; ++k)
    {
        const TimelineStrip& s = clipStrips_[(size_t)k];
        if (s.materialNumSamples <= 0)
        {
            continue;
        }
        const std::int64_t a = s.startOnTimeline;
        const std::int64_t b = a + static_cast<std::int64_t>(s.materialNumSamples);
        if (t >= a && t < b)
        {
            return true;
        }
    }
    return false;
}

// [Message thread] **Phases of the rule (read top-down):** (1) take this row’s full span in session
// time. (2) **Subtract** the union of all *newer* rows (smaller index) to get where this event is
// still the **top** paint. (3) **Intersect** with the union of *older* rows (larger index) to keep
// only times when something is genuinely **stacked under** this card. (4) merge for one hatch pass.
// If either (2) or (3) is empty, there is no “underlap story” to tell for this row on screen.
void ClipWaveformView::computeLocalOverlapShadeHalfOpenIntervalsForRow(
    const int row,
    std::vector<std::pair<std::int64_t, std::int64_t>>& outMerged) const
{
    outMerged.clear();
    const int n = (int)clipStrips_.size();
    if (row < 0 || row >= n)
    {
        return;
    }
    const TimelineStrip& sr = clipStrips_[(size_t)row];
    if (sr.materialNumSamples <= 0)
    {
        return;
    }
    // --- Phase A: this row’s [ar, br) — the full horizontal extent of its event body. ---
    const std::int64_t ar = sr.startOnTimeline;
    const std::int64_t br = ar + static_cast<std::int64_t>(sr.materialNumSamples);

    // --- Phase B: union of all rows **newer** than `row` (indices k < row) — they paint *after* this
    //     row, so they **erase** our peek at [ar, br) in those sub-ranges. ---
    std::vector<std::pair<std::int64_t, std::int64_t>> uFront;
    uFront.reserve((size_t)juce::jmax(0, row));
    for (int k = 0; k < row; ++k)
    {
        const TimelineStrip& t = clipStrips_[(size_t)k];
        if (t.materialNumSamples <= 0)
        {
            continue;
        }
        const std::int64_t a = t.startOnTimeline;
        const std::int64_t b = a + static_cast<std::int64_t>(t.materialNumSamples);
        uFront.push_back({ a, b });
    }
    mergeNonOverlapping(uFront);

    // --- Phase C: visible = row span **minus** newer-row union — the times this row is the **top** face. ---
    std::vector<std::pair<std::int64_t, std::int64_t>> visible;
    subtractOpenFromMerged(ar, br, uFront, visible);
    if (visible.empty())
    {
        // Fully covered: every sample in [ar,br) has a newer clip on top, so we never show an
        // overlap *hint* on *this* row’s event (it is invisible).
        return;
    }

    // --- Phase D: union of all rows **older** than `row` (j > row) — material still **exists in the
    //     session** under the stack; we will only mark times where (C) and (D) both apply. ---
    std::vector<std::pair<std::int64_t, std::int64_t>> uBack;
    uBack.reserve((size_t)juce::jmax(0, n - row - 1));
    for (int j = row + 1; j < n; ++j)
    {
        const TimelineStrip& t = clipStrips_[(size_t)j];
        if (t.materialNumSamples <= 0)
        {
            continue;
        }
        const std::int64_t a = t.startOnTimeline;
        const std::int64_t b = a + static_cast<std::int64_t>(t.materialNumSamples);
        uBack.push_back({ a, b });
    }
    mergeNonOverlapping(uBack);
    if (uBack.empty())
    {
        // No older clips in the project at this row: nothing is “under” to signal.
        return;
    }

    // --- Phase E: (visible as top) ∩ (older content exists) — the only **honest** underlap; merge
    //     again so the hatch iterator never double-hits. ---
    for (const auto& vis : visible)
    {
        for (const auto& ub : uBack)
        {
            const std::int64_t L = std::max(vis.first, ub.first);
            const std::int64_t R = std::min(vis.second, ub.second);
            if (L < R)
            {
                outMerged.push_back({ L, R });
            }
        }
    }
    mergeNonOverlapping(outMerged);
}

// [Message thread] **Paint:** sync strips → either overscanned raster blit + dynamic chrome, or full
// uncached paint while editing/recording previews bypass the cache. Playhead draws in `PlayheadOverlay`.
void ClipWaveformView::paint(juce::Graphics& g)
{
    MINIDAW_UI_PAINT_COUNT(clipLanePaints);
    const std::int64_t diagT0 = kClipWaveformPaintDiagnostics
                                    ? static_cast<std::int64_t>(juce::Time::getHighResolutionTicks())
                                    : static_cast<std::int64_t>(0);

    const juce::Rectangle<float> bounds = getLocalBounds().toFloat();
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
    {
        return;
    }

    const std::int64_t arrLen = session_.getArrangementExtentSamples();
    if (arrLen <= 0)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double wPx = (double)bounds.getWidth();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples(wPx);

    // Narrow-stripe fast path (zoom-freeze fix): playhead stripe underpaints hit this lane ~60×/s
    // during playback with a clip region a few px wide. Those paints must never sync strips,
    // fingerprint the pyramid cache (mutex per strip), or rebuild the wave raster — they blit the
    // existing raster (possibly one message-batch stale after a zoom: the viewport mutation that
    // changed spp always queues a coalesced full repaint that rebuilds and corrects it) and draw
    // dirty-culled chrome only. Gesture modes (drag/trim/recording) bypass the raster entirely and
    // keep their existing full path so live previews stay correct.
    {
        const juce::Rectangle<int> dirtyPx = g.getClipBounds();
        const bool narrowStripePaint = dirtyPx.getWidth() < juce::jmax(32, getWidth() / 4);
        if (narrowStripePaint && !waveRaster_.isNull() && waveRasterSpp_ > 0.0
            && !shouldBypassWaveformRasterCache())
        {
            blitWaveRasterApproximate(g, visStart, visLen, spp);
            paintDynamicChrome(g, bounds, visStart, visLen, spp);
            return;
        }
    }

    syncClipStripsFromSnapshotIfNeeded();

    auto reasonToStr = [](const WaveformRasterRebuildReason r) -> const char* {
        switch (r)
        {
        case WaveformRasterRebuildReason::None:
            return "none";
        case WaveformRasterRebuildReason::First:
            return "first";
        case WaveformRasterRebuildReason::Size:
            return "size";
        case WaveformRasterRebuildReason::Zoom:
            return "zoom";
        case WaveformRasterRebuildReason::Snapshot:
            return "snapshot";
        case WaveformRasterRebuildReason::Pyramid:
            return "pyramid";
        case WaveformRasterRebuildReason::OverscanRange:
            return "overscanRange";
        default:
            return "?";
        }
    };

    if (shouldBypassWaveformRasterCache())
    {
        MINIDAW_UI_PAINT_COUNT(clipLaneUncachedPaints);
        paintUncachedFull(g, bounds, visStart, visLen, spp);
        if (kClipWaveformPaintDiagnostics)
        {
            const double us = juce::Time::highResolutionTicksToSeconds(
                                  juce::Time::getHighResolutionTicks() - diagT0)
                              * 1.0e6;
            juce::Logger::writeToLog(juce::String("[ClipWaveformView] paint bypassCache trackId=")
                                      + juce::String((int)trackId_) + " µs=" + juce::String(us, 1));
        }
        return;
    }

    // Geometry-stale raster (zoom / pan beyond overscan / resize): never rebuild inside paint
    // (zoom-freeze fix, slice 2 — the synchronous multi-lane rebuild in the coalesced full repaint
    // after each wheel step was the remaining freeze spike). Blit the old raster scaled onto the
    // new mapping, draw chrome, and arm the deferred one-shot rebuild. Content changes
    // (strips/pyramid fingerprints) still rebuild synchronously below — rare and correctness-first.
    if (!waveRaster_.isNull() && waveRasterStripFp_ == lastPeaksFingerprint_
        && waveRasterPyramidFp_ == computePyramidReadyFingerprint())
    {
        const bool geometryExact = waveRasterSpp_ == spp && waveRasterImageH_ == getHeight()
                                   && visibleFitsWaveRaster(visStart, visLen);
        if (!geometryExact)
        {
            MINIDAW_UI_PAINT_COUNT(clipLaneStaleBlits);
            blitWaveRasterApproximate(g, visStart, visLen, spp);
            paintDynamicChrome(g, bounds, visStart, visLen, spp);
            scheduleDeferredRasterRebuild();
            return;
        }
    }

    const bool hadRasterBefore = !waveRaster_.isNull();
    const bool likelyHit = hadRasterBefore && visibleFitsWaveRaster(visStart, visLen)
                           && waveRasterSpp_ == spp && waveRasterStripFp_ == lastPeaksFingerprint_
                           && waveRasterPyramidFp_ == computePyramidReadyFingerprint();

    const std::int64_t tRebuildStart
        = kClipWaveformPaintDiagnostics && !likelyHit
              ? static_cast<std::int64_t>(juce::Time::getHighResolutionTicks())
              : (std::int64_t)0;

    const bool ok = ensureWaveRasterForViewState(bounds, visStart, visLen, spp, arrLen);
    const std::int64_t tAfterRebuild
        = kClipWaveformPaintDiagnostics && !likelyHit
              ? static_cast<std::int64_t>(juce::Time::getHighResolutionTicks())
              : (std::int64_t)0;

    std::uint64_t rebuildUs = 0;
    if (kClipWaveformPaintDiagnostics && !likelyHit && tAfterRebuild > tRebuildStart)
    {
        rebuildUs = (std::uint64_t)(juce::Time::highResolutionTicksToSeconds(
                                      tAfterRebuild - tRebuildStart)
                                  * 1.0e6);
    }

    if (!ok || waveRaster_.isNull())
    {
        MINIDAW_UI_PAINT_COUNT(clipLaneUncachedPaints);
        paintUncachedFull(g, bounds, visStart, visLen, spp);
        if (kClipWaveformPaintDiagnostics)
        {
            const double us = juce::Time::highResolutionTicksToSeconds(
                                  juce::Time::getHighResolutionTicks() - diagT0)
                              * 1.0e6;
            juce::Logger::writeToLog(
                juce::String("[ClipWaveformView] paint uncachedFallback trackId=")
                + juce::String((int)trackId_) + " µs=" + juce::String(us, 1));
        }
        return;
    }

    const std::int64_t tBlit0 = kClipWaveformPaintDiagnostics
                                    ? static_cast<std::int64_t>(juce::Time::getHighResolutionTicks())
                                    : (std::int64_t)0;

    const int vw = juce::jmax(1, getWidth());
    int srcX = juce::roundToInt((double)(visStart - waveRasterCoveredStart_) / spp);
    srcX = juce::jmax(0, juce::jmin(srcX, juce::jmax(0, waveRasterImageW_ - vw)));
    g.drawImage(waveRaster_, 0, 0, vw, getHeight(), srcX, 0, vw, getHeight());

    const std::int64_t tBlit1 = kClipWaveformPaintDiagnostics
                                    ? static_cast<std::int64_t>(juce::Time::getHighResolutionTicks())
                                    : (std::int64_t)0;

    paintDynamicChrome(g, bounds, visStart, visLen, spp);

    if (kClipWaveformPaintDiagnostics)
    {
        const bool cacheHit = (waveRasterLastRebuildReason_ == WaveformRasterRebuildReason::None);
        const double totalUs = juce::Time::highResolutionTicksToSeconds(
                                   juce::Time::getHighResolutionTicks() - diagT0)
                               * 1.0e6;
        const std::uint64_t blitUs = (tBlit1 > tBlit0)
                                         ? (std::uint64_t)(juce::Time::highResolutionTicksToSeconds(
                                                               tBlit1 - tBlit0)
                                                           * 1.0e6)
                                         : 0u;
        juce::Logger::writeToLog(
            juce::String("[ClipWaveformView] paint trackId=") + juce::String((int)trackId_)
            + " cacheHit=" + juce::String(cacheHit ? "hit" : "miss")
            + " rebuildReason=" + juce::String(reasonToStr(waveRasterLastRebuildReason_))
            + " covered=[" + juce::String(waveRasterCoveredStart_) + ","
            + juce::String(waveRasterCoveredEnd_) + ") vis=[" + juce::String(visStart) + ","
            + juce::String(visStart + visLen) + ") marginPx=" + juce::String(waveRasterMarginPx_)
            + " rebuildApproxUs=" + juce::String((std::int64_t)rebuildUs)
            + " blitApproxUs=" + juce::String((std::int64_t)blitUs) + " totalUs=" + juce::String(totalUs, 1));
    }
}

juce::String ClipWaveformView::clipDisplayLabelForStrip(const TimelineStrip& strip)
{
    const juce::String custom = strip.displayName.trim();
    if (custom.isNotEmpty())
    {
        return custom;
    }
    if (strip.material != nullptr)
    {
        return juce::File::createFileWithoutCheckingPath(strip.material->getSourceFilePath())
            .getFileNameWithoutExtension();
    }
    return {};
}

void ClipWaveformView::paintUncachedFull(juce::Graphics& g,
                                         const juce::Rectangle<float>& bounds,
                                         const std::int64_t visStart,
                                         const std::int64_t visLen,
                                         const double spp)
{
    // --- (1) Shared mapping: session sample → x (matches `TimelineRulerView` samples-per-pixel).
    // ---
    const auto sessionSampleToX = [&](const std::int64_t s) {
        return TimelineRulerView::sessionSampleToLocalX(s, bounds.getX(), visStart, spp);
    };

    // Dirty-region cull (zoom-freeze fix): strips fully outside the dirty clip region are skipped —
    // matters when playhead-stripe underpaints arrive during a gesture (bypass) while playing.
    const juce::Rectangle<float> dirtyPx = g.getClipBounds().toFloat().expanded(2.0f, 0.0f);

    const int numRows = (int)clipStrips_.size();
    // Vertical event stack: a bit of margin from the view edge; silent audio still has the same rect.
    const juce::Rectangle<float> eventTrackY = bounds.reduced(0.0f, tc::kEventVerticalMargin);
    const float midY = eventTrackY.getCentreY();
    const float halfDraw = juce::jmax(1.0f, eventTrackY.getHeight() * 0.5f) * 0.45f;

    // --- (1b) Cross-lane **drop ghost** on a target lane (not the overlap underlap hint).
    if (hasDragGhost_ && dragGhostLengthSamples_ > 0)
    {
        const float gs0 = sessionSampleToX(dragGhostStartOnTimeline_);
        const float gs1
            = sessionSampleToX(dragGhostStartOnTimeline_ + dragGhostLengthSamples_);
        const float gLeft = juce::jmin(gs0, gs1);
        const float gRight = juce::jmax(gs0, gs1);
        juce::Rectangle<float> ghostRect{ gLeft, eventTrackY.getY(), juce::jmax(1.0f, gRight - gLeft),
                                            eventTrackY.getHeight() };
        g.setColour(juce::Colour(0xff5a7a9a).withAlpha(0.28f));
        g.fillRoundedRectangle(ghostRect, tc::kEventCorner);
        g.setColour(juce::Colour(0xffa0b8d8).withAlpha(0.5f));
        g.drawRoundedRectangle(ghostRect, tc::kEventCorner, 1.0f);
    }

    // --- (2) Event bodies + inner peaks, **back to front in snapshot** (largest index → 0). The
    //     *last* iteration (`row==0`) is the **newest** clip — it clobbers any shared pixels from
    //     older rows. Peak columns: skip when the **center** material sample, mapped to the
    //     session line, is already covered by a *newer* row so we do not show a *readable* under-wave.
    //     Uncovered tails of older clips still get a full-height sketch. ---
    for (int row = numRows - 1; row >= 0; --row)
    {
        const TimelineStrip& strip = clipStrips_[(size_t)row];
        if (strip.materialNumSamples <= 0)
        {
            continue;
        }

        const bool rowTrimR
            = pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
              && strip.clipId == *trimPlacedId_;
        const bool rowTrimL
            = pointerLaneMode_ == PointerLaneMode::TrimLeft && trimPlacedId_.has_value()
              && strip.clipId == *trimPlacedId_;
        const int nsForDraw
            = (rowTrimR || rowTrimL)
                  ? (int)juce::jmin(static_cast<std::int64_t>(std::numeric_limits<int>::max()),
                                    trimPreviewVisibleLen_)
                  : strip.materialNumSamples;
        if (nsForDraw <= 0)
        {
            continue;
        }
        const bool paintDragPreview
            = !rowTrimR && !rowTrimL && dragMovementBeyondThreshold_ && mouseDownPlacedId_.has_value()
              && strip.clipId == *mouseDownPlacedId_;
        const std::int64_t startForDraw
            = rowTrimL
                  ? trimPreviewStart_
                  : (paintDragPreview ? tentativeStartOnTimeline_ : strip.startOnTimeline);
        float ex0;
        float ex1;
        if (rowTrimR)
        {
            // Right trim: span uses full tail (M - L) on the session line from S.
            const std::int64_t xMapLen
                = trimViewMappingSpan(visStart, visLen, trimStartSample_, trimMaterialNumSamples_);
            const std::int64_t mapW = juce::jmax(std::int64_t{1}, xMapLen);
            ex0 = sessionSampleToLocalX(trimStartSample_, bounds, visStart, mapW);
            ex1 = sessionSampleToLocalX(
                trimStartSample_ + static_cast<std::int64_t>(nsForDraw), bounds, visStart, mapW);
        }
        else if (rowTrimL)
        {
            // Left trim: **samples-per-pixel** window only (no `trimViewMappingSpan`).
            ex0 = TimelineRulerView::sessionSampleToLocalX(
                trimPreviewStart_, bounds.getX(), visStart, spp);
            ex1 = TimelineRulerView::sessionSampleToLocalX(
                trimPreviewStart_ + static_cast<std::int64_t>(nsForDraw), bounds.getX(), visStart, spp);
        }
        else
        {
            ex0 = TimelineRulerView::sessionSampleToLocalX(
                startForDraw, bounds.getX(), visStart, spp);
            ex1 = TimelineRulerView::sessionSampleToLocalX(
                startForDraw + static_cast<std::int64_t>(nsForDraw), bounds.getX(), visStart, spp);
        }
        const float x0 = juce::jmin(ex0, ex1);
        const float x1 = juce::jmax(ex0, ex1);
        juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0), eventTrackY.getHeight() };

        if (eventRect.getRight() < bounds.getX() || eventRect.getX() > bounds.getRight())
        {
            continue;
        }
        if (!eventRect.intersects(dirtyPx))
        {
            continue;
        }

        tc::paintEventChromeBody(g, eventRect, tc::unifiedClipEventBodyFill());
        if (selectedPlacedId_.has_value() && strip.clipId == *selectedPlacedId_)
        {
            tc::paintEventChromeSelectionOverlay(g, eventRect);
        }

        juce::Rectangle<float> innerForPeakHeight
            = eventRect.reduced(0.0f, 1.0f + kWaveInset * 0.5f);
        if (eventRect.getWidth() >= 1.0f && innerForPeakHeight.getHeight() >= 1.0f)
        {
            const int ns = nsForDraw;
            if (ns > 0)
            {
                if (rowTrimR)
                {
                    paintRowWaveformWithPyramid(
                        g,
                        row,
                        strip,
                        eventRect,
                        innerForPeakHeight,
                        midY,
                        halfDraw,
                        trimStartSample_,
                        strip.leftTrimSamples,
                        ns,
                        bounds,
                        visStart,
                        spp);
                }
                else if (rowTrimL)
                {
                    paintRowWaveformWithPyramid(
                        g,
                        row,
                        strip,
                        eventRect,
                        innerForPeakHeight,
                        midY,
                        halfDraw,
                        trimPreviewStart_,
                        trimPreviewLeft_,
                        ns,
                        bounds,
                        visStart,
                        spp);
                }
                else
                {
                    paintRowWaveformWithPyramid(
                        g,
                        row,
                        strip,
                        eventRect,
                        innerForPeakHeight,
                        midY,
                        halfDraw,
                        startForDraw,
                        strip.leftTrimSamples,
                        ns,
                        bounds,
                        visStart,
                        spp);
                }
            }
        }

        const bool hideTrimCues
            = (pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
               && *trimPlacedId_ == strip.clipId)
              || (pointerLaneMode_ == PointerLaneMode::TrimLeft && trimPlacedId_.has_value()
                  && *trimPlacedId_ == strip.clipId);
        const bool onEventBodyTrimCue
            = hoverEventTrimCueId_ == strip.clipId && !hoverLeftTrimHandleId_.has_value()
              && !hoverRightTrimHandleId_.has_value();
        const bool showLeftTrimHoverCue
            = !hideTrimCues
              && (hoverLeftTrimHandleId_ == strip.clipId
                  || onEventBodyTrimCue);
        const bool showRightTrimHoverCue
            = !hideTrimCues
              && (hoverRightTrimHandleId_ == strip.clipId
                  || onEventBodyTrimCue);
        if (showLeftTrimHoverCue)
        {
            tc::paintEventChromeTrimHandle(g, eventRect, true);
        }
        if (showRightTrimHoverCue)
        {
            tc::paintEventChromeTrimHandle(g, eventRect, false);
        }
        tc::paintEventTopLeftNameLabel(g, eventRect, clipDisplayLabelForStrip(strip));
    }

    // --- (3) Same shade+hatch *style* for every row the interval function marks. **Order r = n-1…0**:
    //     the **newest** row’s pass (`r==0`) is **last** so if a pixel is somehow shared, the front
    //     card’s hint is what you see (matches the mental “top clip” model).
    for (int r = numRows - 1; r >= 0; --r)
    {
        const TimelineStrip& stripR = clipStrips_[(size_t)r];
        if (stripR.materialNumSamples <= 0)
        {
            continue;
        }
        std::vector<std::pair<std::int64_t, std::int64_t>> olap;
        computeLocalOverlapShadeHalfOpenIntervalsForRow(r, olap);
        if (olap.empty())
        {
            continue;
        }
        const bool rowTrimHR
            = pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
              && stripR.clipId == *trimPlacedId_;
        const bool rowTrimHL
            = pointerLaneMode_ == PointerLaneMode::TrimLeft && trimPlacedId_.has_value()
              && stripR.clipId == *trimPlacedId_;
        const int nsH
            = (rowTrimHR || rowTrimHL)
                  ? (int)juce::jmin(static_cast<std::int64_t>(std::numeric_limits<int>::max()),
                                    trimPreviewVisibleLen_)
                  : stripR.materialNumSamples;
        const bool rowDragPreview
            = !rowTrimHR && !rowTrimHL && dragMovementBeyondThreshold_ && mouseDownPlacedId_.has_value()
              && stripR.clipId == *mouseDownPlacedId_;
        const std::int64_t startHatch
            = rowTrimHL
                  ? trimPreviewStart_
                  : (rowDragPreview ? tentativeStartOnTimeline_ : stripR.startOnTimeline);
        float rex0;
        float rex1;
        if (rowTrimHR)
        {
            const std::int64_t mapH = trimViewMappingSpan(
                visStart, visLen, trimStartSample_, trimMaterialNumSamples_);
            const std::int64_t mapHW = juce::jmax(std::int64_t{1}, mapH);
            rex0 = sessionSampleToLocalX(trimStartSample_, bounds, visStart, mapHW);
            rex1 = sessionSampleToLocalX(
                trimStartSample_ + static_cast<std::int64_t>(juce::jmax(0, nsH)),
                bounds,
                visStart,
                mapHW);
        }
        else if (rowTrimHL)
        {
            rex0
                = TimelineRulerView::sessionSampleToLocalX(trimPreviewStart_, bounds.getX(), visStart, spp);
            rex1 = TimelineRulerView::sessionSampleToLocalX(
                trimPreviewStart_ + static_cast<std::int64_t>(juce::jmax(0, nsH)),
                bounds.getX(),
                visStart,
                spp);
        }
        else
        {
            rex0
                = TimelineRulerView::sessionSampleToLocalX(startHatch, bounds.getX(), visStart, spp);
            rex1 = TimelineRulerView::sessionSampleToLocalX(
                startHatch + static_cast<std::int64_t>(juce::jmax(0, nsH)),
                bounds.getX(),
                visStart,
                spp);
        }
        const float rxl = juce::jmin(rex0, rex1);
        const float rxr = juce::jmax(rex0, rex1);
        juce::Rectangle<float> rowEventRect{ rxl, eventTrackY.getY(), juce::jmax(1.0f, rxr - rxl),
                                             eventTrackY.getHeight() };
        if (rowEventRect.getRight() < bounds.getX() || rowEventRect.getX() > bounds.getRight())
        {
            continue;
        }
        juce::Rectangle<float> rowInner
            = rowEventRect.reduced(1.0f + kWaveInset, 1.0f + kWaveInset * 0.5f);
        if (rowInner.getWidth() >= 1.0f && rowInner.getHeight() >= 1.0f)
        {
            drawFrontOverlapShadeAndHatch(g, rowInner, olap, sessionSampleToX);
        }
    }

    // --- (3b) Live recording preview (UI only; not in `SessionSnapshot`): growing body + preview
    //     min/max blocks; **after** committed events + overlap hatch, **before** playhead.
    //     Cycle OD: oldest completed-pass layers first, brightest current-pass on top.
    if (recordingCycleBehindLayersActive_)
    {
        const std::int64_t anchor = recordingCycleLoopAnchorL_;
        const std::int64_t pw = recordingCyclePassWindowLenSamples_;
        const size_t nBehind = recordingCycleBehindPasses_.size();
        for (size_t pi = 0; pi < nBehind; ++pi)
        {
            // Older passes use slightly darker fixed opaque fills so stacked laps remain distinguishable.
            static constexpr std::uint32_t kBehindBodies[] = {
                0xff173d2c,
                0xff1c4834,
                0xff23543e,
                0xff2d624a,
                0xff386f56,
                0xff467e64
            };
            const int pal = juce::jmin(
                (int)(sizeof(kBehindBodies) / sizeof(kBehindBodies[0])) - 1,
                static_cast<int>(pi));
            // Segment 0 (the very first completed pass) sits at S with length R-S; subsequent
            // wrapped passes sit at L with passLen.
            const std::int64_t segAnchor = (pi == 0) ? recordingCycleFirstSegmentStart_ : anchor;
            const std::int64_t segLen = (pi == 0) ? recordingCycleFirstSegmentLength_ : pw;
            paintLiveRecordingPassPreview(
                g,
                bounds.getX(),
                visStart,
                spp,
                eventTrackY,
                segAnchor,
                segLen,
                recordingCycleBehindPasses_[pi],
                juce::Colour(kBehindBodies[(size_t)pal]),
                juce::Colour(0xff5ec998),
                juce::Colour(0xffbdfce0),
                halfDraw,
                midY);
        }
    }
    if (recordingPreviewActive_)
    {
        paintLiveRecordingPassPreview(
            g,
            bounds.getX(),
            visStart,
            spp,
            eventTrackY,
            recordingPreviewStartSample_,
            recordingPreviewLengthSamples_,
            recordingPreviewPeaks_,
            juce::Colour(0xff32b878),
            juce::Colour(0xff92f0bc),
            juce::Colour(0xffeefff4),
            halfDraw,
            midY);
    }
}

void ClipWaveformView::paintDynamicChrome(juce::Graphics& g,
                                          const juce::Rectangle<float>& bounds,
                                          const std::int64_t visStart,
                                          const std::int64_t visLen,
                                          const double spp)
{
    juce::ignoreUnused(visLen);
    const int numRows = (int)clipStrips_.size();
    const juce::Rectangle<float> eventTrackY = bounds.reduced(0.0f, tc::kEventVerticalMargin);
    const float midY = eventTrackY.getCentreY();
    const float halfDraw = juce::jmax(1.0f, eventTrackY.getHeight() * 0.5f) * 0.45f;

    // Dirty-region cull (zoom-freeze fix): chrome for strips fully outside the dirty clip region
    // (playhead-stripe underpaints during playback) is skipped; full repaints cover everything.
    const juce::Rectangle<float> dirtyPx = g.getClipBounds().toFloat().expanded(2.0f, 0.0f);

    if (hasDragGhost_ && dragGhostLengthSamples_ > 0)
    {
        const float gs0 = TimelineRulerView::sessionSampleToLocalX(
            dragGhostStartOnTimeline_, bounds.getX(), visStart, spp);
        const float gs1 = TimelineRulerView::sessionSampleToLocalX(
            dragGhostStartOnTimeline_ + dragGhostLengthSamples_, bounds.getX(), visStart, spp);
        const float gLeft = juce::jmin(gs0, gs1);
        const float gRight = juce::jmax(gs0, gs1);
        juce::Rectangle<float> ghostRect{ gLeft, eventTrackY.getY(), juce::jmax(1.0f, gRight - gLeft),
                                          eventTrackY.getHeight() };
        g.setColour(juce::Colour(0xff5a7a9a).withAlpha(0.28f));
        g.fillRoundedRectangle(ghostRect, tc::kEventCorner);
        g.setColour(juce::Colour(0xffa0b8d8).withAlpha(0.5f));
        g.drawRoundedRectangle(ghostRect, tc::kEventCorner, 1.0f);
    }

    for (int row = numRows - 1; row >= 0; --row)
    {
        const TimelineStrip& strip = clipStrips_[(size_t)row];
        if (strip.materialNumSamples <= 0)
        {
            continue;
        }

        const float ex0 = TimelineRulerView::sessionSampleToLocalX(
            strip.startOnTimeline, bounds.getX(), visStart, spp);
        const float ex1 = TimelineRulerView::sessionSampleToLocalX(
            strip.startOnTimeline + static_cast<std::int64_t>(strip.materialNumSamples),
            bounds.getX(),
            visStart,
            spp);
        const float x0 = juce::jmin(ex0, ex1);
        const float x1 = juce::jmax(ex0, ex1);
        juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0),
                                          eventTrackY.getHeight() };

        if (!eventRect.intersects(dirtyPx))
        {
            continue;
        }

        if (selectedPlacedId_.has_value() && strip.clipId == *selectedPlacedId_)
        {
            tc::paintEventChromeSelectionOverlay(g, eventRect);
        }

        const bool hideTrimCues
            = (pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
               && *trimPlacedId_ == strip.clipId)
              || (pointerLaneMode_ == PointerLaneMode::TrimLeft && trimPlacedId_.has_value()
                  && *trimPlacedId_ == strip.clipId);
        const bool onEventBodyTrimCue
            = hoverEventTrimCueId_ == strip.clipId && !hoverLeftTrimHandleId_.has_value()
              && !hoverRightTrimHandleId_.has_value();
        const bool showLeftTrimHoverCue
            = !hideTrimCues
              && (hoverLeftTrimHandleId_ == strip.clipId || onEventBodyTrimCue);
        const bool showRightTrimHoverCue
            = !hideTrimCues
              && (hoverRightTrimHandleId_ == strip.clipId || onEventBodyTrimCue);
        if (showLeftTrimHoverCue)
        {
            tc::paintEventChromeTrimHandle(g, eventRect, true);
        }
        if (showRightTrimHoverCue)
        {
            tc::paintEventChromeTrimHandle(g, eventRect, false);
        }
        // Live label over the blitted raster: never stale after rename (raster fingerprint does not
        // include the display name).
        tc::paintEventTopLeftNameLabel(g, eventRect, clipDisplayLabelForStrip(strip));
    }

    if (recordingCycleBehindLayersActive_)
    {
        const std::int64_t anchor = recordingCycleLoopAnchorL_;
        const std::int64_t pw = recordingCyclePassWindowLenSamples_;
        const size_t nBehind = recordingCycleBehindPasses_.size();
        for (size_t pi = 0; pi < nBehind; ++pi)
        {
            static constexpr std::uint32_t kBehindBodies[] = {
                0xff173d2c,
                0xff1c4834,
                0xff23543e,
                0xff2d624a,
                0xff386f56,
                0xff467e64
            };
            const int pal = juce::jmin(
                (int)(sizeof(kBehindBodies) / sizeof(kBehindBodies[0])) - 1, static_cast<int>(pi));
            const std::int64_t segAnchor = (pi == 0) ? recordingCycleFirstSegmentStart_ : anchor;
            const std::int64_t segLen = (pi == 0) ? recordingCycleFirstSegmentLength_ : pw;
            paintLiveRecordingPassPreview(
                g,
                bounds.getX(),
                visStart,
                spp,
                eventTrackY,
                segAnchor,
                segLen,
                recordingCycleBehindPasses_[pi],
                juce::Colour(kBehindBodies[(size_t)pal]),
                juce::Colour(0xff5ec998),
                juce::Colour(0xffbdfce0),
                halfDraw,
                midY);
        }
    }
    if (recordingPreviewActive_)
    {
        paintLiveRecordingPassPreview(
            g,
            bounds.getX(),
            visStart,
            spp,
            eventTrackY,
            recordingPreviewStartSample_,
            recordingPreviewLengthSamples_,
            recordingPreviewPeaks_,
            juce::Colour(0xff32b878),
            juce::Colour(0xff92f0bc),
            juce::Colour(0xffeefff4),
            halfDraw,
            midY);
    }
}

void ClipWaveformView::updateTrimHoverAndCursor(const juce::Point<float> pos) noexcept
{
    if (cursorOverriddenForInvalidDrop_)
    {
        return;
    }
    if (pointerLaneMode_ == PointerLaneMode::TrimRight
        || pointerLaneMode_ == PointerLaneMode::TrimLeft)
    {
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::LeftRightResizeCursor));
        return;
    }
    if (mouseDownPlacedId_.has_value() && pointerLaneMode_ == PointerLaneMode::MoveClip)
    {
        if (hoverEventTrimCueId_ || hoverLeftTrimHandleId_ || hoverRightTrimHandleId_)
        {
            hoverEventTrimCueId_.reset();
            hoverLeftTrimHandleId_.reset();
            hoverRightTrimHandleId_.reset();
            repaint();
        }
        return;
    }

    const EditTool editToolForSplit
        = (laneHost_.getActiveEditTool ? laneHost_.getActiveEditTool() : EditTool::Pointer);
    if (editToolForSplit == EditTool::Split && pointerLaneMode_ == PointerLaneMode::None
        && !mouseDownPlacedId_.has_value())
    {
        const std::optional<PlacedClipId> beforeCueS = hoverEventTrimCueId_;
        const std::optional<PlacedClipId> beforeLS = hoverLeftTrimHandleId_;
        const std::optional<PlacedClipId> beforeRS = hoverRightTrimHandleId_;
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();
        if (beforeCueS != hoverEventTrimCueId_ || beforeLS != hoverLeftTrimHandleId_
            || beforeRS != hoverRightTrimHandleId_)
        {
            repaint();
        }
        const std::shared_ptr<const SessionSnapshot> snapS
            = session_.loadSessionSnapshotForAudioThread();
        const std::int64_t contentEndS = session_.getContentEndSamples();
        if (snapS == nullptr || contentEndS <= 0)
        {
            if (!cursorOverriddenForInvalidDrop_)
            {
                setMouseCursor(
                    juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
            }
            return;
        }
        const int tIdxS = snapS->findTrackIndexById(trackId_);
        if (tIdxS < 0)
        {
            if (!cursorOverriddenForInvalidDrop_)
            {
                setMouseCursor(
                    juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
            }
            return;
        }
        const juce::Rectangle<float> bS = getLocalBounds().toFloat();
        if (bS.getWidth() <= 0.0f)
        {
            return;
        }
        const juce::Rectangle<float> eventTrackYS = bS.reduced(0.0f, tc::kEventVerticalMargin);
        const std::int64_t visStartS = timelineViewport_.getVisibleStartSamples();
        const double sppS = timelineViewport_.getSamplesPerPixel();
        if (sppS <= 0.0)
        {
            return;
        }
        const LanePixelHit phS = hitPlacedEventBodyOnlyInLaneAtPixels(
            snapS, tIdxS, pos, bS, eventTrackYS, visStartS, sppS);
        if (phS.kind == LanePixelHitKind::EventBody && !cursorOverriddenForInvalidDrop_)
        {
            setMouseCursor(juce::MouseCursor(juce::MouseCursor::StandardCursorType::IBeamCursor));
        }
        else if (!cursorOverriddenForInvalidDrop_)
        {
            setMouseCursor(
                juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
        }
        return;
    }

    const std::optional<PlacedClipId> beforeCue = hoverEventTrimCueId_;
    const std::optional<PlacedClipId> beforeL = hoverLeftTrimHandleId_;
    const std::optional<PlacedClipId> beforeR = hoverRightTrimHandleId_;
    const std::shared_ptr<const SessionSnapshot> snap
        = session_.loadSessionSnapshotForAudioThread();
    const std::int64_t contentEnd = session_.getContentEndSamples();
    if (snap == nullptr || contentEnd <= 0)
    {
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();
        if (beforeCue != hoverEventTrimCueId_ || beforeL != hoverLeftTrimHandleId_
            || beforeR != hoverRightTrimHandleId_)
        {
            repaint();
        }
        if (!cursorOverriddenForInvalidDrop_ && pointerLaneMode_ != PointerLaneMode::TrimRight
            && pointerLaneMode_ != PointerLaneMode::TrimLeft)
        {
            setMouseCursor(
                juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
        }
        return;
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();
        if (beforeCue != hoverEventTrimCueId_ || beforeL != hoverLeftTrimHandleId_
            || beforeR != hoverRightTrimHandleId_)
        {
            repaint();
        }
        if (!cursorOverriddenForInvalidDrop_ && pointerLaneMode_ != PointerLaneMode::TrimRight
            && pointerLaneMode_ != PointerLaneMode::TrimLeft)
        {
            setMouseCursor(
                juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
        }
        return;
    }
    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
    {
        return;
    }
    const juce::Rectangle<float> eventTrackY = b.reduced(0.0f, tc::kEventVerticalMargin);
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const LanePixelHit ph
        = hitPlacedInLaneAtPixels(snap, tIdx, pos, b, eventTrackY, visStart, spp);
    hoverEventTrimCueId_.reset();
    hoverLeftTrimHandleId_.reset();
    hoverRightTrimHandleId_.reset();
    if (ph.kind == LanePixelHitKind::TrimLeft)
    {
        hoverLeftTrimHandleId_ = ph.id;
    }
    else if (ph.kind == LanePixelHitKind::TrimRight)
    {
        hoverRightTrimHandleId_ = ph.id;
    }
    else if (ph.kind == LanePixelHitKind::EventBody)
    {
        hoverEventTrimCueId_ = ph.id;
    }
    if (beforeCue != hoverEventTrimCueId_ || beforeL != hoverLeftTrimHandleId_
        || beforeR != hoverRightTrimHandleId_)
    {
        repaint();
    }
    if (ph.kind == LanePixelHitKind::TrimLeft || ph.kind == LanePixelHitKind::TrimRight)
    {
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::LeftRightResizeCursor));
    }
    else if (!cursorOverriddenForInvalidDrop_)
    {
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
    }
}

void ClipWaveformView::mouseMove(const juce::MouseEvent& e)
{
    updateTrimHoverAndCursor(e.position);
}

void ClipWaveformView::mouseExit(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    if (hoverEventTrimCueId_ || hoverLeftTrimHandleId_ || hoverRightTrimHandleId_)
    {
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();
        repaint();
    }
    if (!cursorOverriddenForInvalidDrop_ && pointerLaneMode_ != PointerLaneMode::TrimRight
        && pointerLaneMode_ != PointerLaneMode::TrimLeft)
    {
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
    }
}
