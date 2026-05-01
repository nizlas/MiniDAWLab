// =============================================================================
// TimelineRulerView.cpp  —  seek strip + second ticks (message thread)
// =============================================================================
//
// ROLE
//   Maps horizontal pointer position to a **session-timeline-absolute** sample index (same
//   `timelineLength` / `Transport` contract as the lane) and issues `requestSeek` so the user can
//   place the playhead without using the event lane. Ticks + floating time labels are spatial
//   hints derived from timeline samples via an effective sample rate (**display only** — no tempo
//   / bar grid; labels follow raw transport/playhead timing, **not** playback audible offset).

// PEDAGOGICAL GOAL
//   A reader should see *why* sample rate appears here: ticks + labels are spaced in **seconds** of
//   session time, and seconds are derived from `samples / effectiveDisplayRate` for **drawing** only.
//   Playback and placement still live in **samples** end-to-end.
//
// THREADING
//   All methods here are [Message thread] — JUCE component + timer, no audio device callback.
// =============================================================================

#include "ui/TimelineRulerView.h"

#include "transport/Transport.h"

#include <cmath>
#include <cstdint>
#include <utility>

namespace
{
    // Match `ClipWaveformView` so ruler playhead and lane playhead move on the same cadence.
    constexpr int kPlayheadUpdateHz = 20;

    // Seconds between ticks / labels along the ruler: smallest step that clears min pixel spacing,
    // so zoomed sessions do not collapse into bars; sub-second ticks when zoomed far in only.
    constexpr float kMinTickSpacingPx = 6.0f;

    // Shared ladder by ticks + labels (no bar/beat). Order: finest first.
    constexpr double kStepCandidatesSec[]
        = { 0.1,  0.25, 0.5,  1.0,  2.0,  5.0,  10.0, 30.0,
            60.0, 300.0, 600.0, 3600.0 };
    constexpr int kNumStepCandidates
        = (int)(sizeof(kStepCandidatesSec) / sizeof(kStepCandidatesSec[0]));

    [[nodiscard]] double effectiveDisplaySampleRate(juce::AudioDeviceManager& dm) noexcept
    {
        if (juce::AudioIODevice* d = dm.getCurrentAudioDevice())
        {
            const double r = d->getCurrentSampleRate();
            if (r > 0.0 && std::isfinite(r))
            {
                return r;
            }
        }
        return 48000.0;
    }

    [[nodiscard]] float glyphLayoutWidthPx(const juce::Font& f, const juce::String& text) noexcept
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(f, text, 0.0f, 0.0f);
        const int n = glyphs.getNumGlyphs();
        if (n <= 0)
        {
            return 0.0f;
        }
        return glyphs.getBoundingBox(0, n, true).getWidth();
    }

    [[nodiscard]] float referenceLabelMinSpacingPx() noexcept
    {
        const juce::Font f(juce::FontOptions(11.0f));
        return glyphLayoutWidthPx(f, "00:00.000") + 8.0f;
    }

    [[nodiscard]] double pickTickStepSec(const double pxPerSec) noexcept
    {
        if (pxPerSec <= 0.0 || !std::isfinite(pxPerSec))
        {
            return kStepCandidatesSec[kNumStepCandidates - 1];
        }
        double chosen = kStepCandidatesSec[kNumStepCandidates - 1];
        for (const double step : kStepCandidatesSec)
        {
            if (step * pxPerSec >= (double)kMinTickSpacingPx)
            {
                chosen = step;
                break;
            }
        }
        return chosen;
    }

    [[nodiscard]] double pickLabelStepSec(const double pxPerSec, const double tickStepSec) noexcept
    {
        const float minLabelPx = referenceLabelMinSpacingPx();
        if (pxPerSec <= 0.0 || !std::isfinite(pxPerSec))
        {
            return juce::jmax(tickStepSec, kStepCandidatesSec[kNumStepCandidates - 1]);
        }
        for (const double step : kStepCandidatesSec)
        {
            if (step + 1e-15 < tickStepSec)
            {
                continue;
            }
            if (step * pxPerSec >= (double)minLabelPx)
            {
                return step;
            }
        }
        double coarsestGeTick = tickStepSec;
        for (const double step : kStepCandidatesSec)
        {
            if (step + 1e-15 >= tickStepSec)
            {
                coarsestGeTick = step;
            }
        }
        return coarsestGeTick;
    }

    [[nodiscard]] juce::String formatRulerTimecode(const double seconds, const double stepSec) noexcept
    {
        if (!std::isfinite(seconds) || !std::isfinite(stepSec))
        {
            return {};
        }
        if (std::abs(seconds) < 1e-12)
        {
            return "0 s";
        }
        if (seconds < 60.0)
        {
            if (stepSec >= 1.0 - 1e-15)
            {
                return juce::String((juce::int64)std::llround(seconds)) + " s";
            }
            if (stepSec >= 0.1 - 1e-15)
            {
                return juce::String(seconds, 1) + " s";
            }
            return juce::String(seconds, 3) + " s";
        }
        const auto totalMs = (std::int64_t)std::llround(seconds * 1000.0);
        const std::int64_t m = totalMs / 60000;
        const std::int64_t s = (totalMs % 60000) / 1000;
        const std::int64_t ms = totalMs % 1000;
        if (stepSec >= 1.0 - 1e-15)
        {
            return juce::String::formatted("%lld:%02lld", (long long)m, (long long)s);
        }
        return juce::String::formatted("%lld:%02lld.%03lld", (long long)m, (long long)s, (long long)ms);
    }

    // Ruler playhead: short vertical at the top so the hairline in the lane has a “handle” in time.
    constexpr float kPlayheadMarkerLengthPx = 7.0f;
    constexpr float kRulerLabelStripHeightPx = 12.0f;

    // Locator band + shapes are drawn below the playhead so the playhead stays on top.
    // Alphas tuned for readability on dark UI (Cubase-ish strength).
    constexpr float kLocatorBandAlphaCycleOn = 0.58f;
    constexpr float kLocatorBandAlphaCycleOff = 0.30f;
    constexpr float kLocatorBandAlphaInvalid = 0.58f;

    constexpr float kLocatorTopStripeFrac = 0.45f;
    constexpr float kLocatorTriangleHalfWidth = 5.5f;
    constexpr float kLocatorTriangleHeight = 9.0f;

    // Minimum / maximum **samples per pixel** at zoom in/out.
    constexpr double kSppMin = 0.1;

    void fillDownPointingLocatorTriangle(juce::Graphics& g,
        const float centerX,
        const float rulerTop,
        const juce::Colour& fillCol,
        const juce::Colour& outlineCol)
    {
        const float baseTop = rulerTop + 0.25f;
        const float apexX = centerX;
        const float apexY = baseTop + kLocatorTriangleHeight;
        const float hw = kLocatorTriangleHalfWidth;
        juce::Path p;
        p.addTriangle(apexX - hw, baseTop, apexX + hw, baseTop, apexX, apexY);
        g.setColour(fillCol);
        g.fillPath(p);
        g.setColour(outlineCol);
        g.strokePath(p, juce::PathStrokeType{ 1.0f });
    }
} // namespace

TimelineRulerView::TimelineRulerView(Session& session,
                                     Transport& transport,
                                     juce::AudioDeviceManager& deviceManager,
                                     TimelineViewportModel& timelineViewport,
                                     std::function<bool()> isUiInputBlockedByRecording)
    : session_(session)
    , transport_(transport)
    , deviceManager_(deviceManager)
    , timelineViewport_(timelineViewport)
    , isUiInputBlockedByRecording_(std::move(isUiInputBlockedByRecording))
{
    setInterceptsMouseClicks(true, false);
    startTimerHz(kPlayheadUpdateHz);
}

TimelineRulerView::~TimelineRulerView()
{
    stopTimer();
}

void TimelineRulerView::resized()
{
    const double w = (double)getWidth();
    if (w > 0.0)
    {
        timelineViewport_.clampToExtent(w, session_.getArrangementExtentSamples());
    }
}

void TimelineRulerView::timerCallback()
{
    repaint();
}

std::int64_t TimelineRulerView::xToSessionSampleClamped(
    const float positionX,
    const float widthPx,
    const std::int64_t visibleStart,
    const double samplesPerPixel) noexcept
{
    if (widthPx <= 0.0f || samplesPerPixel <= 0.0 || !std::isfinite(samplesPerPixel))
    {
        return visibleStart;
    }
    const double xcl = juce::jlimit(0.0, (double)widthPx, (double)positionX);
    return visibleStart + (std::int64_t)std::llround(xcl * samplesPerPixel);
}

float TimelineRulerView::sessionSampleToLocalX(
    const std::int64_t s,
    const float originX,
    const std::int64_t visibleStart,
    const double samplesPerPixel) noexcept
{
    if (samplesPerPixel <= 0.0 || !std::isfinite(samplesPerPixel))
    {
        return originX;
    }
    return originX
           + (float)(((double)(s - visibleStart)) / (double)samplesPerPixel);
}

float TimelineRulerView::sessionSampleToLocalXForSpan(
    const std::int64_t s,
    const juce::Rectangle<float>& b,
    const std::int64_t visibleStart,
    const std::int64_t spanSamples) noexcept
{
    if (spanSamples <= 0)
    {
        return b.getX();
    }
    return b.getX()
           + (float)(((double)(s - visibleStart) * (double)b.getWidth()) / (double)spanSamples);
}

void TimelineRulerView::applySeekForLocalX(const float x) noexcept
{
    if (isUiInputBlockedByRecording_ && isUiInputBlockedByRecording_())
    {
        juce::Logger::writeToLog("[Ruler] seek ignored (recording or count-in)");
        return;
    }

    const std::int64_t arr = session_.getArrangementExtentSamples();
    if (arr <= 0)
    {
        return;
    }
    const float w = (float)getWidth();
    if (w <= 0.0f)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const std::int64_t s = xToSessionSampleClamped(x, w, visStart, spp);
    const std::int64_t seekTarget
        = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), s);
    transport_.requestSeek(seekTarget);
    repaint();
}

void TimelineRulerView::applyLeftLocatorForLocalX(const float x) noexcept
{
    if (isUiInputBlockedByRecording_ && isUiInputBlockedByRecording_())
    {
        // Mid-take locator changes can desynchronize cycle wrap math, the offline split, and the
        // UI overlay. Block until recording stops.
        juce::Logger::writeToLog("[Ruler] Ctrl L locator edit ignored (recording or count-in)");
        return;
    }

    const std::int64_t arr = session_.getArrangementExtentSamples();
    if (arr <= 0)
    {
        return;
    }
    const float w = (float)getWidth();
    if (w <= 0.0f)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const std::int64_t s = xToSessionSampleClamped(x, w, visStart, spp);
    const std::int64_t t
        = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), s);

    // First-creation auto-enable: only fire when the user is creating a valid range from a state
    // where no R locator has ever been set (R == 0). Once R is non-zero, the cycle on/off state
    // is sticky against locator edits — making a valid range invalid then valid again does NOT
    // re-enable cycle. Project load doesn't go through this path, so saved locators don't trigger.
    const std::int64_t oldR = session_.getRightLocatorSamples();

    session_.setLeftLocatorAtSample(t);

    const std::int64_t newR = session_.getRightLocatorSamples();
    const bool newValid = newR > t && newR > 0;
    if (oldR == 0 && newValid && !transport_.readCycleEnabledForUi())
    {
        transport_.requestCycleEnabled(true);
        juce::Logger::writeToLog("[Cycle] auto-enabled by L locator edit (first-creation: oldR==0)");
    }
    repaint();
}

void TimelineRulerView::applyRightLocatorForLocalX(const float x) noexcept
{
    if (isUiInputBlockedByRecording_ && isUiInputBlockedByRecording_())
    {
        juce::Logger::writeToLog("[Ruler] Alt R locator edit ignored (recording or count-in)");
        return;
    }

    const std::int64_t arr = session_.getArrangementExtentSamples();
    if (arr <= 0)
    {
        return;
    }
    const float w = (float)getWidth();
    if (w <= 0.0f)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const std::int64_t s = xToSessionSampleClamped(x, w, visStart, spp);
    const std::int64_t t
        = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), s);

    const std::int64_t oldR = session_.getRightLocatorSamples();

    session_.setRightLocatorAtSample(t);

    const std::int64_t newL = session_.getLeftLocatorSamples();
    const bool newValid = t > newL && t > 0;
    if (oldR == 0 && newValid && !transport_.readCycleEnabledForUi())
    {
        transport_.requestCycleEnabled(true);
        juce::Logger::writeToLog("[Cycle] auto-enabled by R locator edit (first-creation: oldR==0)");
    }
    repaint();
}

void TimelineRulerView::tryToggleCycleEnabled() noexcept
{
    if (isUiInputBlockedByRecording_)
    {
        const bool blocked = isUiInputBlockedByRecording_();
        if (blocked)
        {
            juce::Logger::writeToLog("[Cycle] toggle ignored (recording or count-in)");
            return;
        }
    }
    transport_.requestCycleEnabled(!transport_.readCycleEnabledForUi());
    juce::Logger::writeToLog(juce::String{"[Cycle] "}
                             + (transport_.readCycleEnabledForUi() ? "on" : "off"));
    repaint();
}

void TimelineRulerView::mouseDown(const juce::MouseEvent& e)
{
    const float h = (float)getHeight();
    const bool upperHalf = h > 0.0f && e.position.y < h * 0.5f;

    if (e.mods.isAltDown())
    {
        applyRightLocatorForLocalX(e.position.x);
    }
    else if (e.mods.isCtrlDown())
    {
        applyLeftLocatorForLocalX(e.position.x);
    }
    else if (upperHalf)
    {
        tryToggleCycleEnabled();
    }
    else
    {
        applySeekForLocalX(e.position.x);
    }
}

void TimelineRulerView::mouseDrag(const juce::MouseEvent& e)
{
    const float h = (float)getHeight();
    const bool lowerHalf = h > 0.0f && e.position.y >= h * 0.5f;

    if (e.mods.isAltDown())
    {
        applyRightLocatorForLocalX(e.position.x);
    }
    else if (e.mods.isCtrlDown())
    {
        applyLeftLocatorForLocalX(e.position.x);
    }
    else if (lowerHalf)
    {
        applySeekForLocalX(e.position.x);
    }
}

void TimelineRulerView::mouseUp(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
}

void TimelineRulerView::paint(juce::Graphics& g)
{
    const juce::Rectangle<float> bounds = getLocalBounds().toFloat();
    g.fillAll(findColour(juce::ResizableWindow::backgroundColourId).darker(0.1f));
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawLine(
        bounds.getX(),
        bounds.getBottom() - 0.5f,
        bounds.getRight(),
        bounds.getBottom() - 0.5f,
        1.0f);

    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
    {
        return;
    }

    const std::int64_t arrLen = session_.getArrangementExtentSamples();
    if (arrLen <= 0)
    {
        return;
    }
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double wPx = (double)bounds.getWidth();
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples(wPx);
    // **Same** samples-per-pixel map as `ClipWaveformView::paint` for ticks + playhead: sample s → x.
    const auto sessionSampleToX = [&](const std::int64_t s) {
        return sessionSampleToLocalX(s, bounds.getX(), visStart, spp);
    };

    const std::int64_t locL = session_.getLeftLocatorSamples();
    const std::int64_t locR = session_.getRightLocatorSamples();

    const double sampleRate = effectiveDisplaySampleRate(deviceManager_);
    const double pxPerSec = sampleRate / spp;
    const float hShort = juce::jmax(3.0f, bounds.getHeight() * 0.35f);

    if (pxPerSec > 0.0 && std::isfinite(pxPerSec))
    {
        const double tickStepSec = pickTickStepSec(pxPerSec);
        const double labelStepSec = pickLabelStepSec(pxPerSec, tickStepSec);

        // --- Ticks: step from shared ladder; pixels per second = sampleRate / spp.
        g.setColour(juce::Colour(0xff7a8aa0).withAlpha(0.55f));
        for (int k = 0;; ++k)
        {
            const std::int64_t s
                = (std::int64_t)std::llround((double)k * tickStepSec * sampleRate);
            if (s >= arrLen)
            {
                break;
            }
            if (s < visStart || s >= visStart + visLen)
            {
                continue;
            }
            const float x = sessionSampleToX(s);
            if (x < bounds.getX() - 1.0f || x > bounds.getRight() + 1.0f)
            {
                continue;
            }
            g.drawLine(
                x,
                bounds.getBottom() - 1.0f,
                x,
                bounds.getBottom() - 1.0f - hShort,
                1.0f);
        }

        // --- Timecode labels (bottom band); raw timeline seconds, not playback-alignment shifted.
        const juce::Font labelFont(juce::FontOptions(11.0f));
        g.setFont(labelFont);
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        const float labelBaselineY = bounds.getBottom() - 1.0f - hShort - 3.0f;
        const float skipRadius = kLocatorTriangleHalfWidth + 2.0f;

        for (int k = 0;; ++k)
        {
            const std::int64_t samp
                = (std::int64_t)std::llround((double)k * labelStepSec * sampleRate);
            if (samp >= arrLen)
            {
                break;
            }
            if (samp < visStart || samp >= visStart + visLen)
            {
                continue;
            }
            const float x = sessionSampleToX(samp);
            if (x <= bounds.getX() || x >= bounds.getRight())
            {
                continue;
            }

            if (locL >= visStart && locL < visStart + visLen)
            {
                const float xcL = sessionSampleToX(locL);
                if (xcL >= bounds.getX() - 14.0f && xcL <= bounds.getRight() + 14.0f
                    && std::abs(x - xcL) < skipRadius)
                {
                    continue;
                }
            }
            if (locR > 0 && locR >= visStart && locR < visStart + visLen)
            {
                const float xcR = sessionSampleToX(locR);
                if (xcR >= bounds.getX() - 14.0f && xcR <= bounds.getRight() + 14.0f
                    && std::abs(x - xcR) < skipRadius)
                {
                    continue;
                }
            }

            const double sec = (double)samp / sampleRate;
            const juce::String text = formatRulerTimecode(sec, labelStepSec);
            const float tw = glyphLayoutWidthPx(labelFont, text);
            if (x - tw * 0.5f <= bounds.getX() || x + tw * 0.5f >= bounds.getRight())
            {
                continue;
            }

            juce::Rectangle<float> labelRect(
                x - tw * 0.5f,
                labelBaselineY - kRulerLabelStripHeightPx,
                tw,
                kRulerLabelStripHeightPx);
            g.drawText(text, labelRect, juce::Justification::centredBottom);
        }
    }

    // --- Locators: strong band + triangle handles (**before** playhead so playhead stays on top)
    {
        constexpr juce::uint32 kFillPurpleBlue = 0xff7058e8u;
        constexpr juce::uint32 kFillNeutralGray = 0xffb8c2d8u;
        constexpr juce::uint32 kFillInvalidOrange = 0xfff06828u;

        const bool cycleOn = transport_.readCycleEnabledForUi();

        const auto drawTriangleHandleIfVisibleWithClip = [&](const std::int64_t s,
                                                            const juce::Colour& fill,
                                                            const juce::Colour& outline)
        {
            if (s < visStart || s >= visStart + visLen)
            {
                return;
            }
            const float xCenter = sessionSampleToX(s);
            if (xCenter < bounds.getX() - 14.0f || xCenter > bounds.getRight() + 14.0f)
            {
                return;
            }
            fillDownPointingLocatorTriangle(g, xCenter, bounds.getY(), fill, outline);
        };

        if (locR > 0)
        {
            const float x0 = sessionSampleToX(locL);
            const float x1 = sessionSampleToX(locR);
            const float fillLeft = juce::jmin(x0, x1);
            const float fillRight = juce::jmax(x0, x1);
            const float clipL = juce::jmax(fillLeft, bounds.getX());
            const float clipR = juce::jmin(fillRight, bounds.getRight());
            const float bandW = clipR - clipL;

            const bool validInterval = locR > locL;
            const juce::Colour mainFillCol = validInterval
                ? (cycleOn ? juce::Colour(kFillPurpleBlue).withAlpha(kLocatorBandAlphaCycleOn)
                           : juce::Colour(kFillNeutralGray).withAlpha(kLocatorBandAlphaCycleOff))
                : juce::Colour(kFillInvalidOrange).withAlpha(kLocatorBandAlphaInvalid);

            const juce::Colour stripeFillCol = validInterval
                ? (cycleOn ? juce::Colour(kFillPurpleBlue).withAlpha(0.72f)
                           : juce::Colour(kFillNeutralGray).withAlpha(0.46f))
                : juce::Colour(kFillInvalidOrange).withAlpha(0.76f);

            if (bandW > 0.0f)
            {
                const float stripeH
                    = juce::jlimit(3.0f, bounds.getHeight() * kLocatorTopStripeFrac, 11.5f);

                g.setColour(mainFillCol);
                g.fillRect(juce::Rectangle<float>(clipL,
                                                    bounds.getY(),
                                                    bandW,
                                                    bounds.getHeight()));

                g.setColour(stripeFillCol);
                g.fillRect(juce::Rectangle<float>(clipL, bounds.getY(), bandW, stripeH));

                const juce::Colour edgeGlow = stripeFillCol.withAlpha(
                    juce::jmin(1.0f, stripeFillCol.getFloatAlpha() * 1.06f));
                g.setColour(edgeGlow);
                g.fillRect(juce::Rectangle<float>(clipL, bounds.getY(), bandW, 1.25f));
                g.fillRect(juce::Rectangle<float>(clipL, bounds.getBottom() - 2.35f, bandW, 1.85f));
            }

            const juce::Colour handleOutline = juce::Colours::white.withAlpha(0.62f);

            juce::Colour triLFill;
            juce::Colour triRFill;
            if (!validInterval)
            {
                triLFill = juce::Colour(0xffffc070).withAlpha(0.98f);
                triRFill = juce::Colour(0xffff9640).withAlpha(0.98f);
            }
            else if (cycleOn)
            {
                triLFill = juce::Colour(0xffeae2ff).withAlpha(0.98f);
                triRFill = juce::Colour(0xfffff0dc).withAlpha(0.97f);
            }
            else
            {
                triLFill = juce::Colour(0xfff2f5fb).withAlpha(0.97f);
                triRFill = juce::Colour(0xffe4e9f5).withAlpha(0.97f);
            }

            drawTriangleHandleIfVisibleWithClip(locL, triLFill, handleOutline);
            drawTriangleHandleIfVisibleWithClip(locR, triRFill, handleOutline);
        }
        else
        {
            const juce::Colour soloLFill = juce::Colour(0xffd8dee8).withAlpha(0.96f);
            const juce::Colour soloLOutline = juce::Colours::white.withAlpha(0.48f);
            if (locL >= visStart && locL < visStart + visLen)
            {
                const float xc = sessionSampleToX(locL);
                if (xc >= bounds.getX() - 14.0f && xc <= bounds.getRight() + 14.0f)
                {
                    fillDownPointingLocatorTriangle(g, xc, bounds.getY(), soloLFill, soloLOutline);
                }
            }
        }
    }

    // --- Playhead: only when in the visible [visStart, visStart+visLen) window
    const std::int64_t ph = transport_.readPlayheadSamplesForUi();
    const std::int64_t phClamped
        = juce::jlimit(
            std::int64_t{0}, juce::jmax(std::int64_t{0}, arrLen), ph);
    if (phClamped >= visStart && phClamped < visStart + visLen)
    {
        const float xLine = sessionSampleToX(phClamped);
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.drawLine(
            xLine,
            bounds.getY(),
            xLine,
            bounds.getY() + kPlayheadMarkerLengthPx,
            1.5f);
    }
}

void TimelineRulerView::mouseWheelMove(
    const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const std::int64_t arr = session_.getArrangementExtentSamples();
    if (arr <= 0)
    {
        return;
    }
    const double w = (double)getWidth();
    if (w <= 0.0)
    {
        return;
    }
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const double d = (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);
    if (d == 0.0)
    {
        return;
    }
    if (e.mods.isCtrlDown())
    {
        const double x = (double)e.position.x;
        const double factor = std::pow(0.85, d);
        const double sppMax = juce::jmax(1.0, (double)juce::jmax(std::int64_t{1}, arr) / w);
        timelineViewport_.zoomAroundSample(
            factor, x, w, arr, kSppMin, sppMax);
        repaint();
        return;
    }
    const double panNotchPx = juce::jmax(1.0, w / 8.0);
    const std::int64_t step = (d > 0.0) ? (std::int64_t)std::llround(panNotchPx * spp)
                                       : -((std::int64_t)std::llround(panNotchPx * spp));
    if (step == 0)
    {
        return;
    }
    timelineViewport_.panBySamples(step, w, arr);
    repaint();
}
