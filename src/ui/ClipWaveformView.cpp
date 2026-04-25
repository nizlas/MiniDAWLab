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
#include "domain/AudioClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "transport/Transport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

ClipWaveformView::ClipWaveformView(
    Session& session,
    Transport& transport,
    const TrackId trackId,
    ClipWaveformLaneHost laneHost)
    : trackId_(trackId)
    , laneHost_(std::move(laneHost))
    , session_(session)
    , transport_(transport)
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
    const std::int64_t timelineLength = session_.getTimelineLengthSamples();
    if (timelineLength <= 0)
    {
        return;
    }

    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
    {
        return;
    }

    const float t = juce::jlimit(0.0f, 1.0f, e.position.x / b.getWidth());
    const std::int64_t target = juce::jlimit(
        static_cast<std::int64_t>(0),
        timelineLength,
        static_cast<std::int64_t>(std::llround((double)t * (double)timelineLength)));

    if (const std::optional<PlacedClipId> hit
        = hitTestFrontmostPlacedIdAtSessionSample(snap, tIdx, target))
    {
        selectedPlacedId_ = *hit;
        mouseDownPlacedId_ = *hit;
        clickDownX_ = e.position.x;
        dragMovementBeyondThreshold_ = false;
        const auto& tr = snap->getTrack(tIdx);
        for (int i = 0; i < tr.getNumPlacedClips(); ++i)
        {
            if (tr.getPlacedClip(i).getId() == *hit)
            {
                clickDownStartSample_ = tr.getPlacedClip(i).getStartSample();
                mouseDownMaterialNumSamples_ = tr.getPlacedClip(i).getAudioClip().getNumSamples();
                break;
            }
        }
        tentativeStartOnTimeline_ = clickDownStartSample_;
        repaint();
        return;
    }

    selectedPlacedId_.reset();
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
    repaint();
}

void ClipWaveformView::mouseDrag(const juce::MouseEvent& e)
{
    if (!mouseDownPlacedId_.has_value())
    {
        return;
    }
    const std::int64_t timelineLength = session_.getTimelineLengthSamples();
    if (timelineLength <= 0)
    {
        return;
    }
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
    const double deltaS = (static_cast<double>(dx) * static_cast<double>(timelineLength))
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
                lane, tentativeStartOnTimeline_, static_cast<std::int64_t>(mouseDownMaterialNumSamples_));
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

    if (mouseDownPlacedId_.has_value() && dragMovementBeyondThreshold_)
    {
        const bool canCrossLane = static_cast<bool>(laneHost_.findLaneAtScreen);
        if (!canCrossLane)
        {
            session_.moveClip(*mouseDownPlacedId_, tentativeStartOnTimeline_);
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
            }
            else
            {
                session_.moveClipToTrack(
                    *mouseDownPlacedId_, tentativeStartOnTimeline_, lane->getTrackId());
            }
        }
    }
    mouseDownPlacedId_.reset();
    dragMovementBeyondThreshold_ = false;
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
}

std::optional<PlacedClipId> ClipWaveformView::hitTestFrontmostPlacedIdAtSessionSample(
    const std::shared_ptr<const SessionSnapshot>& snap,
    const int trackIndex,
    const std::int64_t timelineSample) const
{
    if (snap == nullptr || trackIndex < 0 || trackIndex >= snap->getNumTracks())
    {
        return std::nullopt;
    }
    const Track& tr = snap->getTrack(trackIndex);
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        const PlacedClip& p = tr.getPlacedClip(i);
        const std::int64_t a0 = p.getStartSample();
        const std::int64_t a1
            = a0 + static_cast<std::int64_t>(p.getAudioClip().getNumSamples());
        if (a0 < a1 && timelineSample >= a0 && timelineSample < a1)
        {
            return p.getId();
        }
    }
    return std::nullopt;
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

    if (snap.get() == lastSnapshotKey_ && w == lastWidth_)
    {
        // Same immutable snapshot and same layout width: peaks still valid; avoid rescans.
        return;
    }

    juce::Logger::writeToLog(
        juce::String("[CLIMPORT] STAGE:peaks:rebuild:begin trackId=") + juce::String(trackId_)
        + " widthPx=" + juce::String(w) + " snapKey=" + juce::String::toHexString((juce::pointer_sized_int)(snap.get())));

    lastSnapshotKey_ = snap.get();
    lastWidth_ = w;
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
        strip.materialNumSamples = ac.getNumSamples();

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
        // How many horizontal **pixels** this clip occupies if the full session is `w` wide — drives
        // column count so we do not build thousands of columns for a one-pixel sliver.
        const float spanPx = wfloat
                             * static_cast<float>(static_cast<double>(ns) / static_cast<double>(timelineEndExcl));
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

    const std::int64_t timelineLength = session_.getTimelineLengthSamples();
    if (timelineLength <= 0)
    {
        // No derived extent — nothing to align events or a seek line with.
        return;
    }

    // --- (1) Shared mapping: device sample on the `Session` line → x in **this** `Component` ---
    const double wPx = (double)bounds.getWidth();
    const double tlenD = (double)timelineLength;
    const auto sessionSampleToX = [&](const std::int64_t s) {
        return static_cast<float>(bounds.getX() + wPx * ((double)s / tlenD));
    };

    const int numRows = (int)clipStrips_.size();
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

        const bool paintDragPreview = dragMovementBeyondThreshold_ && mouseDownPlacedId_.has_value()
                                      && strip.clipId == *mouseDownPlacedId_;
        const std::int64_t startForDraw
            = paintDragPreview ? tentativeStartOnTimeline_ : strip.startOnTimeline;

        const float ex0 = sessionSampleToX(startForDraw);
        const float ex1 = sessionSampleToX(
            startForDraw + static_cast<std::int64_t>(strip.materialNumSamples));
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
        if (inner.getWidth() < 1.0f || inner.getHeight() < 1.0f)
        {
            continue;
        }
        if (strip.peaks.empty())
        {
            continue;
        }

        const float runW = inner.getWidth();
        const int cols = (int)strip.peaks.size();
        if (runW < 0.5f)
        {
            continue;
        }
        const int ns = strip.materialNumSamples;
        const float segW = runW / (float)cols;
        for (int j = 0; j < cols; ++j)
        {
            // Column maps to [s0, s1) in **material**; use mid sample → timeline index for a cheap
            // cover test (avoids a second “readable” wave in underlap; edge columns stay Phase-2-rough).
            const int s0 = (j * ns) / cols;
            const int s1 = ((j + 1) * ns) / cols;
            if (s0 >= s1)
            {
                continue;
            }
            // Midpoint in material [s0, s1) for a coarse “is this column under something in front?” test.
            const int sMid = (s0 + s1) / 2;
            const std::int64_t tOnTimeline
                = startForDraw + static_cast<std::int64_t>(juce::jlimit(0, ns - 1, sMid));
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
        const bool rowDragPreview = dragMovementBeyondThreshold_ && mouseDownPlacedId_.has_value()
                                    && stripR.clipId == *mouseDownPlacedId_;
        const std::int64_t startHatch
            = rowDragPreview ? tentativeStartOnTimeline_ : stripR.startOnTimeline;
        const float rex0 = sessionSampleToX(startHatch);
        const float rex1 = sessionSampleToX(
            startHatch + static_cast<std::int64_t>(stripR.materialNumSamples));
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

    // --- (4) Playhead: read transport (authoritative) and clamp to [0, `timelineLength`] so a
    //     transient or resize cannot draw the line off the view — **display** safety only, not a
    //     new transport contract.
    const std::int64_t ph = transport_.readPlayheadSamplesForUi();
    const std::int64_t phClamped
        = juce::jlimit(static_cast<std::int64_t>(0), timelineLength, ph);
    const float t = static_cast<float>((double)phClamped / tlenD);
    const float xLine = bounds.getX() + t * bounds.getWidth();

    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.drawLine(xLine, bounds.getY(), xLine, bounds.getBottom(), 1.5f);
}
