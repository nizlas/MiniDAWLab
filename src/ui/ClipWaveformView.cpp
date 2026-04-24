// =============================================================================
// ClipWaveformView.cpp  —  DAW-style event layout on the session timeline (message thread)
// =============================================================================
//
// ROLE
//   Fills a list from `loadSessionSnapshotForAudioThread()`: for each `PlacedClip` row, draw the
//   event **envelope** and a peak sketch in material columns whose **center** falls in session
//   time *not* covered by any row in front (lower index in the snapshot, painted later). **Covered**
//   time on a back row: no readable peaks; the overlying event shows through after back→front order.
//   A **post-pass** per row applies the same overlap *hint* (tint + thin diagonals) only in session
//   time where *that* row is the local top and an older row still underlaps — view only. Session
//   samples → x match the playhead.
//
// THREADING
//   `paint` / `mouseDown` / `timerCallback` are [Message thread] only. JUCE `Graphics` API here is
//   single-threaded UI drawing — not a substitute for a waveform cache on the audio thread.
//
// See ClipWaveformView.h: local topmost overlap *hint* (same graphic as before), not “row 0 only.”
// =============================================================================

#include "ui/ClipWaveformView.h"

#include "domain/AudioClip.h"
#include "transport/Transport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    constexpr int kMaxPeakColumnsPerClip = 2000;
    constexpr int kPlayheadUpdateHz = 20;
    // Visual scale for rounded corners so events read as “blocks” on the timeline, not as raw bars.
    constexpr float kEventCorner = 2.5f;
    constexpr float kEventVerticalMargin = 4.0f;
    constexpr float kWaveInset = 2.0f; // keep waveform off the 1px border so the frame reads first

    // All rows share the same event chrome. “Behind” is **not** a dimmer default for the whole
    // event — in uncovered time, a lower row is painted like any visible clip; covered spans are
    // occluded by rows drawn later, not by a different palette choice here.
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

    // Merges half-open [a,b) intervals on the **session** time line so adjacent/adjoining
    // overlaps (same visual band on the front) are drawn once.
    void mergeNonOverlapping(std::vector<std::pair<std::int64_t, std::int64_t>>& inOut)
    {
        if (inOut.size() < 2)
        {
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
                ++w;
                inOut[w] = inOut[r];
            }
        }
        inOut.resize(w + 1);
    }

    // [a,b) \ mergedUnion : half-open; merged is sorted, disjoint, from mergeNonOverlapping.
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
                continue;
            }
            if (iv.first >= b)
            {
                break;
            }
            if (iv.first > cur)
            {
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
            out.push_back({ cur, b });
        }
    }

    // [Message thread] Overlap: restrained **darker** wash + **very fine** diagonals; caller
    // supplies merged [L,R) in session time **already** clipped to the *local* top event’s area.
    // Style unchanged; no vertical “end of underlap” lines.
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
            g.reduceClipRegion(band.toNearestInt());
            // Sparse diagonals: “something else exists here” without a second wave trace.
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

ClipWaveformView::ClipWaveformView(Session& session, Transport& transport)
    : session_(session)
    , transport_(transport)
{
    setInterceptsMouseClicks(true, false);
    startTimerHz(kPlayheadUpdateHz);
}

ClipWaveformView::~ClipWaveformView()
{
    stopTimer();
}

// [Message thread]
void ClipWaveformView::timerCallback()
{
    // Timer tick only **schedules** repaints: we do not recompute peaks here, but we do re-read
    // the latest playhead in `paint` so the line tracks transport without coupling to the callback.
    repaint();
}

// [Message thread] User seeks by timeline position: same mapping as the playhead line, no transport ownership.
void ClipWaveformView::mouseDown(const juce::MouseEvent& e)
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->isEmpty())
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

    transport_.requestSeek(target);
    repaint();
}

// [Message thread] Rebuilds downsampled peaks in **material** 0..N-1; `paint` places them in **timeline** space.
void ClipWaveformView::rebuildPeaksIfNeeded()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    const int w = juce::jmax(1, getWidth());

    if (snap.get() == lastSnapshotKey_ && w == lastWidth_)
    {
        return;
    }

    lastSnapshotKey_ = snap.get();
    lastWidth_ = w;
    clipStrips_.clear();

    if (snap == nullptr || snap->isEmpty())
    {
        return;
    }

    const std::int64_t timelineEndExcl = snap->getDerivedTimelineLengthSamples();
    if (timelineEndExcl <= 0)
    {
        return;
    }

    const int n = snap->getNumPlacedClips();
    clipStrips_.reserve((size_t)n);
    const float wfloat = static_cast<float>(w);

    for (int i = 0; i < n; ++i)
    {
        TimelineStrip strip;
        const PlacedClip& placed = snap->getPlacedClip(i);
        const AudioClip& ac = placed.getAudioClip();
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
        const float spanPx = wfloat
                             * static_cast<float>(static_cast<double>(ns) / static_cast<double>(timelineEndExcl));
        const int colCount = juce::jmax(1, juce::jmin(kMaxPeakColumnsPerClip, (int)std::ceil((double)spanPx)));
        strip.peaks.resize((size_t)colCount, 0.0f);

        for (int col = 0; col < colCount; ++col)
        {
            const int s0 = (col * ns) / colCount;
            const int s1 = ((col + 1) * ns) / colCount;
            float peak = 0.0f;
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
}

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
        // System: one continuous clip event on the device timeline, half-open [a, b).
        if (t >= a && t < b)
        {
            return true;
        }
    }
    return false;
}

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
    const std::int64_t ar = sr.startOnTimeline;
    const std::int64_t br = ar + static_cast<std::int64_t>(sr.materialNumSamples);

    // “In front of” this row: lower index = painted later, occludes. Union of k < row.
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

    std::vector<std::pair<std::int64_t, std::int64_t>> visible;
    subtractOpenFromMerged(ar, br, uFront, visible);
    if (visible.empty())
    {
        return;
    }

    // “Behind” in snapshot order: higher index = older = drawn under this row. Union of j > row.
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
        return;
    }

    // Product: t where this row is the *minimum* index active (wins) and an older row still
    // contributes — the only time this row should carry the overlap *hint* on *its* event body.
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

// [Message thread] Full paint: one **event** per clip row, then the playhead on top (always last).
void ClipWaveformView::paint(juce::Graphics& g)
{
    rebuildPeaksIfNeeded();

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

    // --- 1) Shared mapping: session sample → view X (identical to the playhead math below) ---
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

    // --- 2) Back → front: same event chrome for every row. Peak columns are skipped only when the
    //     column’s **center** sample lies in time covered by a *prior* row (lower index = painted
    //     on top in a later iteration). **Do not** dim an entire back row: uncovered tails are full
    //     read like a foreground clip. ---
    constexpr float kEventStroke = 1.0f;
    for (int row = numRows - 1; row >= 0; --row)
    {
        const TimelineStrip& strip = clipStrips_[(size_t)row];
        if (strip.materialNumSamples <= 0)
        {
            continue;
        }

        const float ex0 = sessionSampleToX(strip.startOnTimeline);
        const float ex1 = sessionSampleToX(
            strip.startOnTimeline + static_cast<std::int64_t>(strip.materialNumSamples));
        const float x0 = juce::jmin(ex0, ex1);
        const float x1 = juce::jmax(ex0, ex1);
        juce::Rectangle<float> eventRect{ x0, eventTrackY.getY(), juce::jmax(1.0f, x1 - x0), eventTrackY.getHeight() };

        g.setColour(eventBodyFill());
        g.fillRoundedRectangle(eventRect, kEventCorner);
        g.setColour(eventBodyBorder());
        g.drawRoundedRectangle(eventRect, kEventCorner, kEventStroke);

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
                = strip.startOnTimeline + static_cast<std::int64_t>(juce::jlimit(0, ns - 1, sMid));
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

    // --- 3b) Same shade+hatch *style* as before, but on **each** row r where that row is the
    //     **local** top (no lower index on that t) and at least one **older** index j>r still
    //     covers t. (Draw r = n-1..0 so the newest row’s pass remains last in z.)
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
        const float rex0 = sessionSampleToX(stripR.startOnTimeline);
        const float rex1 = sessionSampleToX(
            stripR.startOnTimeline + static_cast<std::int64_t>(stripR.materialNumSamples));
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

    // --- 4) Playhead on top: always the same `Transport` / seek coordinate line as in earlier steps. ---
    const std::int64_t ph = transport_.readPlayheadSamplesForUi();
    const std::int64_t phClamped
        = juce::jlimit(static_cast<std::int64_t>(0), timelineLength, ph);
    const float t = static_cast<float>((double)phClamped / tlenD);
    const float xLine = bounds.getX() + t * bounds.getWidth();

    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.drawLine(xLine, bounds.getY(), xLine, bounds.getBottom(), 1.5f);
}
