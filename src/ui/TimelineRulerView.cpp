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

#include "diagnostics/UiPaintLoadCounters.h"
#include "domain/ArrangementMusicalSnap.h"
#include "ui/TimelineLocatorPainter.h"
#include "ui/PlayheadPixelMapping.h"
#include "ui/UiPlayheadClock.h"
#include "transport/Transport.h"

#include <cmath>
#include <cstdint>
#include <utility>

namespace
{
    // Structural watcher cadence (viewport/locator/tempo changes); the playhead marker is drawn by
    // `PlayheadOverlay` above this view and never drives ruler repaints.
    constexpr int kStructuralWatchHz = 30;

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
                                     UiPlayheadClock& uiPlayheadClock,
                                     std::function<bool()> isUiInputBlockedByRecording)
    : session_(session)
    , transport_(transport)
    , deviceManager_(deviceManager)
    , timelineViewport_(timelineViewport)
    , uiPlayheadClock_(uiPlayheadClock)
    , isUiInputBlockedByRecording_(std::move(isUiInputBlockedByRecording))
{
    setInterceptsMouseClicks(true, false);
    // NOT a decorative optimization — do not remove. The transparent `PlayheadOverlay` above
    // invalidates a narrow stripe over this view ~60×/s during playback; without buffering each of
    // those redraws re-runs the full tick/label/locator paint, which was part of the playback
    // repaint tax behind the main-window zoom freeze (see
    // MAIN_WINDOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md §20). Buffering makes them image blits; our own
    // `repaint()` calls (structural watcher, locator edits) invalidate the buffer as usual. This is
    // also why the playhead marker is drawn by the overlay and not here: a marker baked into this
    // buffer would go stale.
    setBufferedToImage(true);
    startTimerHz(kStructuralWatchHz);
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
    // Structural watcher only: ticks, labels, locator/cycle band, viewport, tempo/meter. The
    // playhead marker is drawn by `PlayheadOverlay` (which covers the ruler band) and never
    // repaints this view.
    const std::int64_t arrLen = session_.getArrangementExtentSamples();
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    const std::int64_t locL = session_.getLeftLocatorSamples();
    const std::int64_t locR = session_.getRightLocatorSamples();
    const bool cycleOn = transport_.readCycleEnabledForUi();
    const auto timeDisplay = session_.getTimelineRulerTimeDisplay();
    const ProjectMusicalTime musicalTime = session_.getProjectMusicalTime();

    const bool structuralChanged
        = !structuralSnapshotValid_
          || arrLen != lastArrangementExtentUi_
          || visStart != lastVisibleStartUi_
          || spp != lastSamplesPerPixelUi_
          || locL != lastLeftLocatorUi_
          || locR != lastRightLocatorUi_
          || cycleOn != lastCycleEnabledUi_
          || timeDisplay != lastTimeDisplayUi_
          || musicalTime.bpm != lastMusicalBpmUi_
          || musicalTime.numerator != lastMusicalNumeratorUi_
          || musicalTime.denominator != lastMusicalDenominatorUi_;

    lastArrangementExtentUi_ = arrLen;
    lastVisibleStartUi_ = visStart;
    lastSamplesPerPixelUi_ = spp;
    lastLeftLocatorUi_ = locL;
    lastRightLocatorUi_ = locR;
    lastCycleEnabledUi_ = cycleOn;
    lastTimeDisplayUi_ = timeDisplay;
    lastMusicalBpmUi_ = musicalTime.bpm;
    lastMusicalNumeratorUi_ = musicalTime.numerator;
    lastMusicalDenominatorUi_ = musicalTime.denominator;
    structuralSnapshotValid_ = true;

    if (structuralChanged)
    {
        repaint();
    }
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
    // The audio thread commits the seek a block later; anchoring the shared clock keeps every
    // current-time indicator on the requested sample in the meantime. The `PlayheadOverlay`
    // (which draws the marker over the ruler band) picks the value up on its next 60 Hz tick,
    // so scrub-dragging tracks the pointer without any ruler repaint.
    uiPlayheadClock_.reanchorTo(seekTarget);
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
    // Loop boundaries obey the same arrangement snap state as clip edits (free when snap is off).
    const std::int64_t snapped = snapSampleToGridIfEnabled(s,
                                                           session_.getArrangementSnapSettings(),
                                                           session_.getProjectMusicalTime(),
                                                           effectiveDisplaySampleRate(deviceManager_));
    const std::int64_t t
        = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), snapped);

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
    // Same snap treatment as the left locator: shared arrangement snap state, no loop-only setting.
    const std::int64_t snapped = snapSampleToGridIfEnabled(s,
                                                           session_.getArrangementSnapSettings(),
                                                           session_.getProjectMusicalTime(),
                                                           effectiveDisplaySampleRate(deviceManager_));
    const std::int64_t t
        = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), snapped);

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
    MINIDAW_UI_PAINT_COUNT(rulerPaints);
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

    // Dirty-region cull window (zoom-freeze fix): playhead stripe repaints during playback arrive
    // ~60×/s with a clip region a few px wide — tick/label loops then only cover that window
    // instead of the whole visible span. Padding: 2 px for tick stroke antialiasing; labels are
    // centre-anchored so their (loop-culled) window gets an extra half-max-label-width margin.
    const juce::Rectangle<int> dirtyPx = g.getClipBounds();
    constexpr double kTickCullPadPx = 2.0;
    constexpr double kLabelCullPadPx = 60.0;
    const auto cullSampleAtX = [&](const double xPx) {
        return visStart + (std::int64_t)std::llround((xPx - (double)bounds.getX()) * spp);
    };
    const std::int64_t cullTickStart = cullSampleAtX((double)dirtyPx.getX() - kTickCullPadPx);
    const std::int64_t cullTickEnd = cullSampleAtX((double)dirtyPx.getRight() + kTickCullPadPx);
    const std::int64_t cullLabelStart = cullSampleAtX((double)dirtyPx.getX() - kLabelCullPadPx);
    const std::int64_t cullLabelEnd = cullSampleAtX((double)dirtyPx.getRight() + kLabelCullPadPx);

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
            musicalTime,
            cullTickStart,
            cullTickEnd);
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
            sampleRate,
            cullTickStart,
            cullTickEnd);
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
            locR,
            cullLabelStart,
            cullLabelEnd);
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
            locR,
            cullLabelStart,
            cullLabelEnd);
    }

    // No playhead marker here: `PlayheadOverlay` covers the ruler band and draws the marker
    // segment, so playhead frames never invalidate this buffered view.
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
        // No direct repaint: the owner's viewport listener repaints ruler+lanes via a coalesced
        // flush (one dirty-marking per message batch — repaint-storm fix).
        timelineViewport_.zoomAroundSample(
            factor, x, w, arr, kSppMin, sppMax);
        return;
    }
    if (!e.mods.isShiftDown())
    {
        return;
    }
    const double panNotchPx = juce::jmax(1.0, w / 8.0);
    const double panD = -d;
    const std::int64_t step = (panD > 0.0) ? (std::int64_t)std::llround(panNotchPx * spp)
                                           : -((std::int64_t)std::llround(panNotchPx * spp));
    if (step == 0)
    {
        return;
    }
    // No direct repaint: coalesced via the owner's viewport listener (repaint-storm fix).
    timelineViewport_.panBySamples(step, w, arr);
}
