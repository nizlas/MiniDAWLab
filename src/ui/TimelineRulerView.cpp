// =============================================================================
// TimelineRulerView.cpp  —  seek strip + musical ticks (message thread)
// =============================================================================
//
// ROLE
//   Maps horizontal pointer position to a **session-timeline-absolute** sample index (same
//   `timelineLength` / `Transport` contract as the lane) and issues `requestSeek` so the user can
//   place the playhead without using the event lane. Ticks + labels follow **project** tempo/meter
//   (`Session::getProjectMusicalTime`) at an effective display sample rate (**drawing only** —
//   playback stays sample-authoritative). Sample 0 is bar 1, beat 1.

// PEDAGOGICAL GOAL
//   Readers see why sample rate appears: bar/beat spacing uses BPM × effectiveDisplayRate for
//   **ticks/labels only**. Placement and transport remain in **samples** end-to-end.
//
// THREADING
//   All methods here are [Message thread] — JUCE component + timer, no audio device callback.
// =============================================================================

#include "ui/TimelineRulerView.h"

#include "ui/TimelineLocatorPainter.h"
#include "transport/Transport.h"

#include <cmath>
#include <cstdint>
#include <utility>

namespace
{
    // Match `ClipWaveformView` so ruler playhead and lane playhead move on the same cadence.
    constexpr int kPlayheadUpdateHz = 20;

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

    // Minimum / maximum **samples per pixel** at zoom in/out.
    constexpr double kSppMin = 0.1;
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
    const bool cycleOn = transport_.readCycleEnabledForUi();

    const double sampleRate = effectiveDisplaySampleRate(deviceManager_);
    const ProjectMusicalTime musicalTime = session_.getProjectMusicalTime();

    using namespace timeline_locator_paint;

    paintLocatorCycleBandAndStripe(
        g, bounds, sessionSampleToX, visStart, visLen, locL, locR, cycleOn);

    if (session_.getTimelineRulerTimeDisplay() == Session::TimelineRulerTimeDisplay::MusicalBarsBeats)
    {
        paintRulerMusicalTickMarks(
            g,
            bounds,
            sessionSampleToX,
            arrLen,
            visStart,
            visLen,
            sampleRate,
            spp,
            musicalTime);
    }
    else
    {
        paintRulerTickMarks(
            g,
            bounds,
            sessionSampleToX,
            arrLen,
            visStart,
            visLen,
            sampleRate);
    }
    paintLocatorTriangleHandles(
        g, bounds, sessionSampleToX, visStart, visLen, locL, locR, cycleOn);
    if (session_.getTimelineRulerTimeDisplay() == Session::TimelineRulerTimeDisplay::MusicalBarsBeats)
    {
        paintRulerMusicalLabels(
            g,
            bounds,
            sessionSampleToX,
            arrLen,
            visStart,
            visLen,
            sampleRate,
            spp,
            musicalTime,
            locL,
            locR);
    }
    else
    {
        paintRulerTimeLabels(
            g,
            bounds,
            sessionSampleToX,
            arrLen,
            visStart,
            visLen,
            sampleRate,
            locL,
            locR);
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
            bounds.getY() + kRulerPlayheadMarkerLengthPx,
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
    if (!e.mods.isShiftDown())
    {
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
