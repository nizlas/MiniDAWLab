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

#include "ui/ForbiddenCursor.h"
#include "ui/TimelineViewportModel.h"
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

// See `getForbiddenNoDropMouseCursor` in `ForbiddenCursor.cpp` (shared with `TrackHeaderView`).

// Anonymous helpers: all **session-timeline** intervals are half-open [a, b) in device samples,
// matching `PlacedClip` placement + `PlaybackEngine` / `Transport` usage. They exist only to
// keep **paint** decisions (where to draw peaks vs overlap tint) consistent and cheap — **not** to
// duplicate engine mixing rules; this file never writes transport or the session.
namespace
{
    // Cap: each clip’s peak vector could otherwise grow to one column per *sample*; we only need
    // pixel-resolution sketching, not analytics-grade peaks (see `rebuildPeaksIfNeeded` span rule).
    constexpr int kMaxPeakColumnsPerClip = 2000;
    // Compromise: often enough to feel “live” on the playhead line without repainting the whole
    // view on every frame (full rate would be overkill for this teaching codebase).
    constexpr int kPlayheadUpdateHz = 20;
    // Visual scale for rounded corners so events read as “blocks” on the timeline, not as raw bars.
    constexpr float kEventCorner = 2.5f;
    constexpr float kEventVerticalMargin = 4.0f;
    constexpr float kWaveInset = 2.0f; // keep waveform off the 1px border so the frame reads first
    // Horizontal delta below which a mousedown+move is treated as a click (no `Session::moveClip`).
    constexpr float kDragThresholdPx = 3.0f;
    // Cubase-style **trim**: small square in the bottom-right of the event (separate from move).
    constexpr float kTrimHandleSquarePx = 5.0f;
    constexpr float kMinEventWidthForTrimHandlePx = 12.0f;
    // Inset of the square from the event’s bottom-right edge (keeps the stroke visible).
    constexpr float kTrimHandleMarginPx = 1.5f;
    // Full-height hit zone on the right edge of the event (in addition to the corner square) so
    // grabbing the “trim line” works after a strong inward trim, not just the 5px corner.
    constexpr float kTrimHitRightEdgeBandPx = 6.0f;

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

    [[nodiscard]] float sessionSampleToLocalX(
        const std::int64_t s,
        const juce::Rectangle<float>& b,
        const std::int64_t visStart,
        const std::int64_t mapSpan) noexcept
    {
        if (mapSpan <= 0)
        {
            return b.getX();
        }
        return static_cast<float>(
            b.getX()
            + (double)(s - visStart) * (double)b.getWidth() / (double)mapSpan);
    }

    // Same **column** budget rule as `rebuildPeaksIfNeeded` for a given **visible** material length.
    [[nodiscard]] int peakColumnCountForVisibleLength(
        const float viewWidthPx,
        const int visibleMaterialSamples,
        const std::int64_t sessionTimelineEndExcl) noexcept
    {
        if (viewWidthPx <= 0.0f || visibleMaterialSamples <= 0)
        {
            return 1;
        }
        const std::int64_t tEx = juce::jmax(std::int64_t{1}, sessionTimelineEndExcl);
        const float spanPx
            = viewWidthPx
              * (float)(static_cast<double>(visibleMaterialSamples) / static_cast<double>(tEx));
        return juce::jmax(1, juce::jmin(kMaxPeakColumnsPerClip, (int)std::ceil((double)spanPx)));
    }

    enum class LanePixelHitKind { None, TrimHandle, EventBody };

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
        const std::int64_t visLen) noexcept
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
        if (visLen <= 0)
        {
            return r;
        }
        const std::int64_t mapW = juce::jmax(std::int64_t{1}, visLen);
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
            const float ex0 = sessionSampleToLocalX(a0, b, visStart, mapW);
            const float ex1 = sessionSampleToLocalX(a1, b, visStart, mapW);
            const float x0 = juce::jmin(ex0, ex1);
            const float x1 = juce::jmax(ex0, ex1);
            if (x1 - x0 < 0.5f)
            {
                continue;
            }
            if (eventTrackY.getHeight() < 2.0f)
            {
                continue;
            }
            juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(0.5f, x1 - x0), eventTrackY.getHeight() };
            const float bandW = juce::jmin(kTrimHitRightEdgeBandPx, eventRect.getWidth());
            const float bandX = juce::jmax(eventRect.getX(), eventRect.getRight() - bandW);
            juce::Rectangle<float> edgeHitBand{ bandX, eventRect.getY(), bandW, eventRect.getHeight() };
            if (edgeHitBand.getWidth() >= 0.5f && edgeHitBand.contains(p))
            {
                r.kind = LanePixelHitKind::TrimHandle;
                r.rowInTrack = i;
                r.id = pc.getId();
                return r;
            }
            if (eventRect.getWidth() >= kMinEventWidthForTrimHandlePx
                && eventRect.getHeight() >= kTrimHandleSquarePx + 2.0f)
            {
                const float hsz
                    = juce::jmin(kTrimHandleSquarePx, eventRect.getWidth() * 0.4f, eventRect.getHeight() * 0.4f);
                if (hsz >= 2.0f)
                {
                    const float hLeft = juce::jmax(
                        eventRect.getX() + 0.5f, eventRect.getRight() - kTrimHandleMarginPx - hsz);
                    const float hTop = juce::jmax(
                        eventRect.getY() + 0.5f, eventRect.getBottom() - kTrimHandleMarginPx - hsz);
                    const juce::Rectangle<float> hRect{ hLeft, hTop, hsz, hsz };
                    if (hRect.contains(p))
                    {
                        r.kind = LanePixelHitKind::TrimHandle;
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
            const float ex0 = sessionSampleToLocalX(a0, b, visStart, mapW);
            const float ex1 = sessionSampleToLocalX(a1, b, visStart, mapW);
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

    // All rows share the same event chrome. **Product:** a clip that is only *partly* covered in
    // time should not read as a permanently “muted track” in its **uncovered** tails — the rule is
    // local, not a global style by snapshot index. Covered spans are *occluded* by a newer row’s
    // paint, not by a different palette in this function.
    juce::Colour eventBodyFill()
    {
        return juce::Colour(0xff343c4d);
    }

    juce::Colour eventBodyBorder()
    {
        return juce::Colour(0xff7a8aa0).withAlpha(0.9f);
    }

    // Peak bar opacity for any row, whenever that column is not covered by a prior row in time.
    constexpr float kWaveformPeakAlpha = 0.9f;

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

ClipWaveformView::ClipWaveformView(
    Session& session,
    Transport& transport,
    const TrackId trackId,
    TimelineViewportModel& timelineViewport,
    ClipWaveformLaneHost laneHost)
    : trackId_(trackId)
    , laneHost_(std::move(laneHost))
    , session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
{
    jassert(trackId_ != kInvalidTrackId);
    // JUCE: selection/drag; seek is on the timeline ruler, not the empty lane.
    setInterceptsMouseClicks(true, false);
    startTimerHz(kPlayheadUpdateHz);
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

void ClipWaveformView::clearSelectionOnly()
{
    selectedPlacedId_.reset();
    pointerLaneMode_ = PointerLaneMode::None;
    trimPlacedId_.reset();
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    hoverEventTrimCueId_.reset();
    hoverTrimHandleId_.reset();
    repaint();
}

ClipWaveformView::~ClipWaveformView()
{
    restoreNormalCursorAfterInvalidDrop();
    // JUCE: `Timer` must be stopped before destruction so the message loop never calls back in.
    stopTimer();
}

// [Message thread] Throttled repaints: **contract** is “eventually consistent” playhead line —
// we do not try to match every host audio callback; `paint` re-reads `readPlayheadSamplesForUi` so
// the line stays aligned with the transport the user already trusts for audio, without a second
// playhead copy stored on the view.
void ClipWaveformView::timerCallback()
{
    repaint();
}

// [Message thread] Click: front-most hit test → select; empty lane → clear selection only (seek
// uses `TimelineRulerView`). Drag (same gesture) is handled in `mouseDrag` / `mouseUp`;
// `Session::moveClip` runs **only** on commit — ordering policy stays in
// `SessionSnapshot::withClipMoved`, not here.
void ClipWaveformView::mouseDown(const juce::MouseEvent& e)
{
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
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples();

    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
    {
        return;
    }

    const juce::Rectangle<float> eventTrackY = b.reduced(0.0f, kEventVerticalMargin);
    const LanePixelHit ph
        = hitPlacedInLaneAtPixels(snap, tIdx, e.position, b, eventTrackY, visStart, visLen);
    if (ph.kind == LanePixelHitKind::TrimHandle)
    {
        const PlacedClip& hitPlaced = snap->getTrack(tIdx).getPlacedClip(ph.rowInTrack);
        selectedPlacedId_ = ph.id;
        const std::int64_t eff = hitPlaced.getEffectiveLengthSamples();
        const std::int64_t a0 = hitPlaced.getStartSample();
        const std::int64_t a1 = a0 + eff;
        pointerLaneMode_ = PointerLaneMode::TrimRight;
        trimPlacedId_ = ph.id;
        trimStartSample_ = a0;
        trimMaterialNumSamples_ = hitPlaced.getMaterialLengthSamples();
        trimClickDownVisibleLen_ = eff;
        trimPreviewVisibleLen_ = eff;
        const std::int64_t mapLen = trimViewMappingSpan(
            visStart, visLen, a0, hitPlaced.getMaterialLengthSamples());
        const double mapLenD = (double)juce::jmax(std::int64_t{1}, mapLen);
        const float tClick
            = juce::jlimit(0.0f, 1.0f, e.position.x / juce::jmax(1.0f, b.getWidth()));
        const std::int64_t sAtX
            = static_cast<std::int64_t>(std::llround(tClick * mapLenD));
        trimRightEdgeToMouseOffsetSamples_ = a1 - sAtX;
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        hoverEventTrimCueId_.reset();
        hoverTrimHandleId_.reset();
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::LeftRightResizeCursor));
        repaint();
        return;
    }
    if (ph.kind == LanePixelHitKind::EventBody)
    {
        const PlacedClip& hitPlaced = snap->getTrack(tIdx).getPlacedClip(ph.rowInTrack);
        selectedPlacedId_ = ph.id;
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
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    pointerLaneMode_ = PointerLaneMode::None;
    trimPlacedId_.reset();
    updateTrimHoverAndCursor(e.position);
}

void ClipWaveformView::mouseDrag(const juce::MouseEvent& e)
{
    if (pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value())
    {
        const std::int64_t arrExtent = session_.getArrangementExtentSamples();
        if (arrExtent <= 0)
        {
            return;
        }
        const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
        const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples();
        const juce::Rectangle<float> b = getLocalBounds().toFloat();
        if (b.getWidth() <= 0.0f)
        {
            return;
        }
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
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples();
    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
    {
        return;
    }
    const float dx = e.position.x - clickDownX_;
    if (std::abs(dx) >= kDragThresholdPx)
    {
        dragMovementBeyondThreshold_ = true;
    }
    const double deltaS = (static_cast<double>(dx) * static_cast<double>(visLen))
                          / static_cast<double>(b.getWidth());
    tentativeStartOnTimeline_ = juce::jmax(
        static_cast<std::int64_t>(0), clickDownStartSample_ + static_cast<std::int64_t>(std::llround(deltaS)));

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
    if (laneHost_.clearAllGhosts)
    {
        laneHost_.clearAllGhosts();
    }
    restoreNormalCursorAfterInvalidDrop();

    if (pointerLaneMode_ == PointerLaneMode::TrimRight)
    {
        if (trimPlacedId_.has_value() && trimPreviewVisibleLen_ != trimClickDownVisibleLen_)
        {
            session_.setClipRightEdgeVisibleLength(*trimPlacedId_, trimPreviewVisibleLen_);
            timelineViewport_.clampToExtent(session_.getArrangementExtentSamples());
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
        const bool canCrossLane = static_cast<bool>(laneHost_.findLaneAtScreen);
        if (!canCrossLane)
        {
            session_.moveClip(*mouseDownPlacedId_, tentativeStartOnTimeline_);
            timelineViewport_.clampToExtent(session_.getArrangementExtentSamples());
        }
        else
        {
            auto* const lane
                = laneHost_.findLaneAtScreen(juce::Point<int>(e.getScreenX(), e.getScreenY()));
            if (lane == nullptr)
            {
                // Cancel — no `Session` publish
            }
            else if (lane == this)
            {
                session_.moveClip(*mouseDownPlacedId_, tentativeStartOnTimeline_);
                timelineViewport_.clampToExtent(session_.getArrangementExtentSamples());
            }
            else
            {
                session_.moveClipToTrack(
                    *mouseDownPlacedId_, tentativeStartOnTimeline_, lane->getTrackId());
                timelineViewport_.clampToExtent(session_.getArrangementExtentSamples());
            }
        }
    }
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    pointerLaneMode_ = PointerLaneMode::None;
    updateTrimHoverAndCursor(e.position);
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
        return;
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        selectedPlacedId_.reset();
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
    hoverTrimHandleId_.reset();
}

// [Message thread] Fills `clipStrips_` with per-row peak columns so `paint` only maps numbers to x.
// **What we compute:** a max absolute sample in each *material* sub-range [s0,s1), roughly one
// column per **pixel** this clip’s duration spans in the current view (capped) — the sketch
// coarsens when zoomed out, not a full PCM trace. O(rows × columns × samples per column) when the
// snapshot or width **changes** (cached by raw snapshot pointer + width).
void ClipWaveformView::rebuildPeaksIfNeeded()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    const int w = juce::jmax(1, getWidth());
    const std::int64_t vStart = timelineViewport_.getVisibleStartSamples();
    const std::int64_t vLen = timelineViewport_.getVisibleLengthSamples();

    if (snap.get() == lastSnapshotKey_ && w == lastWidth_ && vStart == lastVisibleStartForPeaks_
        && vLen == lastVisibleLengthForPeaks_)
    {
        // Same snapshot, width, and visible x-span: peaks still valid; avoid rescans.
        return;
    }

    juce::Logger::writeToLog(
        juce::String("[CLIMPORT] STAGE:peaks:rebuild:begin trackId=") + juce::String(trackId_)
        + " widthPx=" + juce::String(w) + " snapKey=" + juce::String::toHexString((juce::pointer_sized_int)(snap.get())));

    lastSnapshotKey_ = snap.get();
    lastWidth_ = w;
    lastVisibleStartForPeaks_ = vStart;
    lastVisibleLengthForPeaks_ = vLen;
    clipStrips_.clear();
    clearSelectionIfIdMissing(snap);

    if (snap == nullptr)
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:peaks:rebuild:abort reason=null_snap trackId=") + juce::String(trackId_));
        selectedPlacedId_.reset();
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        return;
    }
    const int tIdx = snap->findTrackIndexById(trackId_);
    if (tIdx < 0)
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:peaks:rebuild:abort reason=no_track trackId=") + juce::String(trackId_));
        selectedPlacedId_.reset();
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        return;
    }
    if (snap->isEmpty())
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:peaks:rebuild:abort reason=empty_session trackId=") + juce::String(trackId_));
        selectedPlacedId_.reset();
        mouseDownPlacedId_.reset();
        dragMovementBeyondThreshold_ = false;
        return;
    }

    const std::int64_t timelineEndExcl = snap->getDerivedTimelineLengthSamples();
    if (timelineEndExcl <= 0)
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:peaks:rebuild:abort reason=timeline_length_zero trackId=")
            + juce::String(trackId_));
        return;
    }

    const auto& tr = snap->getTrack(tIdx);
    const int n = tr.getNumPlacedClips();
    clipStrips_.reserve((size_t)n);
    const float wfloat = static_cast<float>(w);

    // Per clip: resample the PCM to `colCount` peak magnitudes in **material** [0, N), then
    // `paint` maps those columns to **timeline** x using `startOnTimeline` — not done here.
    for (int i = 0; i < n; ++i)
    {
        TimelineStrip strip;
        const PlacedClip& placed = tr.getPlacedClip(i);
        const AudioClip& ac = placed.getAudioClip();
        strip.clipId = placed.getId();
        strip.startOnTimeline = placed.getStartSample();
        {
            const std::int64_t eff = placed.getEffectiveLengthSamples();
            strip.materialNumSamples
                = static_cast<int>(juce::jmin(
                    static_cast<std::int64_t>(std::numeric_limits<int>::max()), eff));
        }

        if (strip.materialNumSamples <= 0)
        {
            clipStrips_.push_back(std::move(strip));
            continue;
        }

        const juce::AudioBuffer<float>& audio = ac.getAudio();
        const int numCh = audio.getNumChannels();
        if (numCh <= 0)
        {
            clipStrips_.push_back(std::move(strip));
            continue;
        }

        const int ns = strip.materialNumSamples;
        // How many horizontal **pixels** this clip occupies for the current **visible** span — drives
        // column count so we do not build thousands of columns for a one-pixel sliver. Uses
        // `TimelineViewportModel`, not the derived length alone, so trim does not change scale.
        const std::int64_t visSpan
            = juce::jmax(std::int64_t{1}, timelineViewport_.getVisibleLengthSamples());
        const float spanPx = wfloat
                             * static_cast<float>(static_cast<double>(ns) / static_cast<double>(visSpan));
        const int colCount = juce::jmax(1, juce::jmin(kMaxPeakColumnsPerClip, (int)std::ceil((double)spanPx)));
        strip.peaks.resize((size_t)colCount, 0.0f);

        for (int col = 0; col < colCount; ++col)
        {
            const int s0 = (col * ns) / colCount;
            const int s1 = ((col + 1) * ns) / colCount;
            float peak = 0.0f;
            // Same *loudness* idea as a simple DAW trace: for this **column**, take the max |sample|
            // across channels (Phase 1 stereo / mono path — not a LUFS meter).
            for (int s = s0; s < s1; ++s)
            {
                for (int c = 0; c < numCh; ++c)
                {
                    peak = juce::jmax(peak, std::abs(audio.getSample(c, s)));
                }
            }
            strip.peaks[(size_t)col] = juce::jlimit(0.0f, 1.0f, peak);
        }

        clipStrips_.push_back(std::move(strip));
    }
    juce::Logger::writeToLog(
        juce::String("[CLIMPORT] STAGE:peaks:rebuild:done trackId=") + juce::String(trackId_) + " stripsOnLane="
        + juce::String((int)clipStrips_.size()));
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

// [Message thread] **Paint pipeline in five beats (scan top-down in this function):**
//   (0) Rebuild downsampled peaks if snapshot or view width changed.
//   (1) One shared linear **session sample → x** (same as playhead).
//   (2) Draw every row’s event **chassis** + peak bars (high row index first → low index last so
//       the newest clip’s paint wins on overlaps).
//   (3) Overlap *hint* pass per row (same order) — tint and hatch only on rows that the interval
//       math says deserve it (tint + thin diagonals).
//   (4) Playhead on top (always) so the line is never lost behind events.
void ClipWaveformView::paint(juce::Graphics& g)
{
    // (0) see `rebuildPeaksIfNeeded` — cheap no-op on steady state.
    rebuildPeaksIfNeeded();

    // JUCE: default window background, slightly darkened so events read as “cards” (pure chrome).
    g.fillAll(findColour(juce::ResizableWindow::backgroundColourId).darker(0.2f));

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
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples();
    const double vlenD = (double)juce::jmax(std::int64_t{1}, visLen);

    // --- (1) Shared mapping: session sample → x in this component (matches `TimelineRulerView`).
    // ---
    const double wPx = (double)bounds.getWidth();
    const auto sessionSampleToX = [&](const std::int64_t s) {
        return static_cast<float>(
            bounds.getX() + wPx * ((double)(s - visStart) / vlenD));
    };

    const int numRows = (int)clipStrips_.size();
    const std::shared_ptr<const SessionSnapshot> paintSnap
        = session_.loadSessionSnapshotForAudioThread();
    const int paintTrackIdx
        = (paintSnap != nullptr) ? paintSnap->findTrackIndexById(trackId_) : -1;
    const std::int64_t endForPeakColumns = juce::jmax(std::int64_t{1}, arrLen);
    const float wfloatForPeaks = static_cast<float>(juce::jmax(1, getWidth()));
    // Vertical event stack: a bit of margin from the view edge; silent audio still has the same rect.
    const juce::Rectangle<float> eventTrackY = bounds.reduced(0.0f, kEventVerticalMargin);
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
        g.fillRoundedRectangle(ghostRect, kEventCorner);
        g.setColour(juce::Colour(0xffa0b8d8).withAlpha(0.5f));
        g.drawRoundedRectangle(ghostRect, kEventCorner, 1.0f);
    }

    // --- (2) Event bodies + inner peaks, **back to front in snapshot** (largest index → 0). The
    //     *last* iteration (`row==0`) is the **newest** clip — it clobbers any shared pixels from
    //     older rows. Peak columns: skip when the **center** material sample, mapped to the
    //     session line, is already covered by a *newer* row so we do not show a *readable* under-wave.
    //     Uncovered tails of older clips still get a full-height sketch. ---
    constexpr float kEventStroke = 1.0f;
    for (int row = numRows - 1; row >= 0; --row)
    {
        const TimelineStrip& strip = clipStrips_[(size_t)row];
        if (strip.materialNumSamples <= 0)
        {
            continue;
        }

        const bool rowTrim
            = pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
              && strip.clipId == *trimPlacedId_;
        const int nsForDraw = rowTrim
            ? (int)juce::jmin(static_cast<std::int64_t>(std::numeric_limits<int>::max()),
                              trimPreviewVisibleLen_)
            : strip.materialNumSamples;
        if (nsForDraw <= 0)
        {
            continue;
        }
        const bool paintDragPreview
            = !rowTrim && dragMovementBeyondThreshold_ && mouseDownPlacedId_.has_value()
              && strip.clipId == *mouseDownPlacedId_;
        const std::int64_t startForDraw
            = paintDragPreview ? tentativeStartOnTimeline_ : strip.startOnTimeline;
        // While trimming, map x using room up to the full **material** end so the handle can
        // extend the visible region in view. Denominator is the view span plus material extent.
        const std::int64_t xMapLen
            = rowTrim ? trimViewMappingSpan(
                  visStart, visLen, trimStartSample_, trimMaterialNumSamples_)
                      : visLen;
        const std::int64_t mapW = juce::jmax(std::int64_t{1}, xMapLen);
        const float ex0 = sessionSampleToLocalX(startForDraw, bounds, visStart, mapW);
        const float ex1 = sessionSampleToLocalX(
            startForDraw + static_cast<std::int64_t>(nsForDraw), bounds, visStart, mapW);
        const float x0 = juce::jmin(ex0, ex1);
        const float x1 = juce::jmax(ex0, ex1);
        juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0), eventTrackY.getHeight() };

        g.setColour(eventBodyFill());
        g.fillRoundedRectangle(eventRect, kEventCorner);
        g.setColour(eventBodyBorder());
        g.drawRoundedRectangle(eventRect, kEventCorner, kEventStroke);
        if (selectedPlacedId_.has_value() && strip.clipId == *selectedPlacedId_)
        {
            g.setColour(juce::Colour(0xff9eb8d8).withAlpha(0.95f));
            g.drawRoundedRectangle(eventRect, kEventCorner, 1.2f);
        }

        juce::Rectangle<float> inner = eventRect.reduced(1.0f + kWaveInset, 1.0f + kWaveInset * 0.5f);
        if (inner.getWidth() >= 1.0f && inner.getHeight() >= 1.0f)
        {
            const int ns = nsForDraw;
            if (rowTrim)
            {
                // **Crop** the sketch to [0, trimPreview) at constant samples/px: recompute the column
                // count for the *preview* length. Using cached `strip.peaks`+narrow `inner` would fit
                // too many columns and looks like a horizontal **squeeze** of the same bitmap.
                const PlacedClip* srcPlaced = nullptr;
                if (paintTrackIdx >= 0 && paintSnap != nullptr)
                {
                    const Track& trP = paintSnap->getTrack(paintTrackIdx);
                    for (int k = 0; k < trP.getNumPlacedClips(); ++k)
                    {
                        if (trP.getPlacedClip(k).getId() == strip.clipId)
                        {
                            srcPlaced = &trP.getPlacedClip(k);
                            break;
                        }
                    }
                }
                if (srcPlaced != nullptr && ns > 0)
                {
                    const AudioClip& ac = srcPlaced->getAudioClip();
                    const juce::AudioBuffer<float>& audio = ac.getAudio();
                    const int numCh = audio.getNumChannels();
                    if (numCh > 0)
                    {
                        const int colCount
                            = peakColumnCountForVisibleLength(wfloatForPeaks, ns, endForPeakColumns);
                        const float runW = inner.getWidth();
                        if (runW >= 0.5f)
                        {
                            const float segW = runW / (float)juce::jmax(1, colCount);
                            for (int j = 0; j < colCount; ++j)
                            {
                                const int s0 = (j * ns) / colCount;
                                const int s1 = ((j + 1) * ns) / colCount;
                                if (s0 >= s1)
                                {
                                    continue;
                                }
                                const int sMid = (s0 + s1) / 2;
                                const std::int64_t tOnTimeline
                                    = startForDraw
                                      + static_cast<std::int64_t>(juce::jlimit(0, ns - 1, sMid));
                                if (isTimelineSampleCoveredByPriorRows(row, tOnTimeline))
                                {
                                    continue;
                                }
                                float peak = 0.0f;
                                for (int s = s0; s < s1; ++s)
                                {
                                    for (int c = 0; c < numCh; ++c)
                                    {
                                        peak = juce::jmax(peak, std::abs(audio.getSample(c, s)));
                                    }
                                }
                                peak = juce::jlimit(0.0f, 1.0f, peak);
                                const float xj = inner.getX() + (float)j * segW;
                                const float h = peak * halfDraw;
                                g.setColour(juce::Colours::lightblue.withAlpha(kWaveformPeakAlpha));
                                g.fillRect(xj, midY - h, juce::jmax(1.0f, segW), h * 2.0f);
                            }
                        }
                    }
                }
            }
            else if (!strip.peaks.empty())
            {
                const float runW = inner.getWidth();
                const int cols = (int)strip.peaks.size();
                if (runW >= 0.5f)
                {
                    const int ns2 = nsForDraw;
                    const float segW = runW / (float)cols;
                    for (int j = 0; j < cols; ++j)
                    {
                        // Column maps to [s0, s1) in **material**; use mid sample → timeline for cover test.
                        const int s0 = (j * ns2) / cols;
                        const int s1 = ((j + 1) * ns2) / cols;
                        if (s0 >= s1)
                        {
                            continue;
                        }
                        const int sMid = (s0 + s1) / 2;
                        const std::int64_t tOnTimeline
                            = startForDraw
                              + static_cast<std::int64_t>(juce::jlimit(0, ns2 - 1, sMid));
                        if (isTimelineSampleCoveredByPriorRows(row, tOnTimeline))
                        {
                            continue;
                        }

                        const float xj = inner.getX() + (float)j * segW;
                        const float h = strip.peaks[(size_t)j] * halfDraw;
                        g.setColour(juce::Colours::lightblue.withAlpha(kWaveformPeakAlpha));
                        g.fillRect(xj, midY - h, juce::jmax(1.0f, segW), h * 2.0f);
                    }
                }
            }
        }

        const bool showTrimHoverCue
            = hoverEventTrimCueId_.has_value() && *hoverEventTrimCueId_ == strip.clipId
              && !(pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
                   && *trimPlacedId_ == strip.clipId);
        if (showTrimHoverCue)
        {
            if (eventRect.getWidth() >= kMinEventWidthForTrimHandlePx
                && eventRect.getHeight() >= kTrimHandleSquarePx + 2.0f)
            {
                const float hsz
                    = juce::jmin(kTrimHandleSquarePx, eventRect.getWidth() * 0.4f, eventRect.getHeight() * 0.4f);
                if (hsz >= 2.0f)
                {
                    const float hLeft = juce::jmax(
                        eventRect.getX() + 0.5f, eventRect.getRight() - kTrimHandleMarginPx - hsz);
                    const float hTop = juce::jmax(
                        eventRect.getY() + 0.5f, eventRect.getBottom() - kTrimHandleMarginPx - hsz);
                    const juce::Rectangle<float> hR{ hLeft, hTop, hsz, hsz };
                    g.setColour(juce::Colour(0xff3d4a5a).brighter(0.25f).withAlpha(0.88f));
                    g.fillRoundedRectangle(hR, 1.0f);
                    g.setColour(juce::Colour(0xff9eb0c8).withAlpha(0.95f));
                    g.drawRoundedRectangle(hR, 1.0f, 0.75f);
                }
            }
        }
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
        const bool rowTrimH
            = pointerLaneMode_ == PointerLaneMode::TrimRight && trimPlacedId_.has_value()
              && stripR.clipId == *trimPlacedId_;
        const int nsH = rowTrimH
            ? (int)juce::jmin(static_cast<std::int64_t>(std::numeric_limits<int>::max()),
                              trimPreviewVisibleLen_)
            : stripR.materialNumSamples;
        const bool rowDragPreview
            = !rowTrimH && dragMovementBeyondThreshold_ && mouseDownPlacedId_.has_value()
              && stripR.clipId == *mouseDownPlacedId_;
        const std::int64_t startHatch
            = rowDragPreview ? tentativeStartOnTimeline_ : stripR.startOnTimeline;
        const std::int64_t mapH
            = rowTrimH
                  ? trimViewMappingSpan(
                        visStart, visLen, trimStartSample_, trimMaterialNumSamples_)
                  : visLen;
        const std::int64_t mapHW = juce::jmax(std::int64_t{1}, mapH);
        const float rex0 = sessionSampleToLocalX(startHatch, bounds, visStart, mapHW);
        const float rex1 = sessionSampleToLocalX(
            startHatch + static_cast<std::int64_t>(juce::jmax(0, nsH)), bounds, visStart, mapHW);
        const float rxl = juce::jmin(rex0, rex1);
        const float rxr = juce::jmax(rex0, rex1);
        juce::Rectangle<float> rowEventRect{ rxl, eventTrackY.getY(), juce::jmax(1.0f, rxr - rxl),
                                             eventTrackY.getHeight() };
        juce::Rectangle<float> rowInner
            = rowEventRect.reduced(1.0f + kWaveInset, 1.0f + kWaveInset * 0.5f);
        if (rowInner.getWidth() >= 1.0f && rowInner.getHeight() >= 1.0f)
        {
            drawFrontOverlapShadeAndHatch(g, rowInner, olap, sessionSampleToX);
        }
    }

    // --- (4) Playhead: clamp to arrangement extent; line only when inside the visible window.
    const std::int64_t ph = transport_.readPlayheadSamplesForUi();
    const std::int64_t phClamped
        = juce::jlimit(
            std::int64_t{0}, juce::jmax(std::int64_t{0}, arrLen), ph);
    if (phClamped >= visStart && phClamped < visStart + visLen)
    {
        const float xLine = sessionSampleToX(phClamped);
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.drawLine(xLine, bounds.getY(), xLine, bounds.getBottom(), 1.5f);
    }
}

void ClipWaveformView::updateTrimHoverAndCursor(const juce::Point<float> pos) noexcept
{
    if (cursorOverriddenForInvalidDrop_)
    {
        return;
    }
    if (pointerLaneMode_ == PointerLaneMode::TrimRight)
    {
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::LeftRightResizeCursor));
        return;
    }
    if (mouseDownPlacedId_.has_value() && pointerLaneMode_ == PointerLaneMode::MoveClip)
    {
        if (hoverEventTrimCueId_ || hoverTrimHandleId_)
        {
            hoverEventTrimCueId_.reset();
            hoverTrimHandleId_.reset();
            repaint();
        }
        return;
    }

    const std::optional<PlacedClipId> beforeCue = hoverEventTrimCueId_;
    const std::optional<PlacedClipId> beforeH = hoverTrimHandleId_;
    const std::shared_ptr<const SessionSnapshot> snap
        = session_.loadSessionSnapshotForAudioThread();
    const std::int64_t contentEnd = session_.getContentEndSamples();
    if (snap == nullptr || contentEnd <= 0)
    {
        hoverEventTrimCueId_.reset();
        hoverTrimHandleId_.reset();
        if (beforeCue != hoverEventTrimCueId_ || beforeH != hoverTrimHandleId_)
        {
            repaint();
        }
        if (!cursorOverriddenForInvalidDrop_ && pointerLaneMode_ != PointerLaneMode::TrimRight)
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
        hoverTrimHandleId_.reset();
        if (beforeCue != hoverEventTrimCueId_ || beforeH != hoverTrimHandleId_)
        {
            repaint();
        }
        if (!cursorOverriddenForInvalidDrop_ && pointerLaneMode_ != PointerLaneMode::TrimRight)
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
    const juce::Rectangle<float> eventTrackY = b.reduced(0.0f, kEventVerticalMargin);
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples();
    const LanePixelHit ph
        = hitPlacedInLaneAtPixels(snap, tIdx, pos, b, eventTrackY, visStart, visLen);
    if (ph.kind == LanePixelHitKind::TrimHandle)
    {
        hoverEventTrimCueId_ = ph.id;
        hoverTrimHandleId_ = ph.id;
    }
    else if (ph.kind == LanePixelHitKind::EventBody)
    {
        hoverEventTrimCueId_ = ph.id;
        hoverTrimHandleId_.reset();
    }
    else
    {
        hoverEventTrimCueId_.reset();
        hoverTrimHandleId_.reset();
    }
    if (beforeCue != hoverEventTrimCueId_ || beforeH != hoverTrimHandleId_)
    {
        repaint();
    }
    if (ph.kind == LanePixelHitKind::TrimHandle)
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
    if (hoverEventTrimCueId_ || hoverTrimHandleId_)
    {
        hoverEventTrimCueId_.reset();
        hoverTrimHandleId_.reset();
        repaint();
    }
    if (!cursorOverriddenForInvalidDrop_ && pointerLaneMode_ != PointerLaneMode::TrimRight)
    {
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
    }
}
