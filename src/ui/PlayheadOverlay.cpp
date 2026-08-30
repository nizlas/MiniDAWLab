#include "ui/PlayheadOverlay.h"

#include "diagnostics/UiPaintLoadCounters.h"
#include "ui/PlayheadPixelMapping.h"
#include "ui/TimelineLocatorPainter.h"
#include "ui/UiPlayheadClock.h"
#include "transport/Transport.h"

#include <cmath>

namespace
{
    constexpr float kPlayheadStrokeThicknessPx = 1.5f;

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
} // namespace

PlayheadOverlay::PlayheadOverlay(
    Session& session,
    Transport& transport,
    TimelineViewportModel& timelineViewport,
    juce::AudioDeviceManager& deviceManager,
    UiPlayheadClock& uiPlayheadClock)
    : session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
    , deviceManager_(deviceManager)
    , uiPlayheadClock_(uiPlayheadClock)
{
    setInterceptsMouseClicks(false, false);
    // 60 Hz is affordable because a tick invalidates at most two narrow columns (or nothing when
    // the line stays in one column).
    startTimerHz(kUpdateHz);
}

PlayheadOverlay::~PlayheadOverlay()
{
    stopTimer();
}

void PlayheadOverlay::setOnPlayheadFrameAdvanced(std::function<void(double)> fn) noexcept
{
    onPlayheadFrameAdvanced_ = std::move(fn);
}

PlayheadOverlay::UiRenderStats PlayheadOverlay::snapshotUiRenderStatsAndReset() noexcept
{
    UiRenderStats s;
    s.timerTicks = statsTimerTicks_;
    s.repaintRequests = statsRepaintRequests_;
    s.repaintAreaPx = statsRepaintAreaPx_;
    s.timerIntervalMinMs = statsIntervalSamples_ > 0 ? statsIntervalMinMs_ : 0.0;
    s.timerIntervalMaxMs = statsIntervalSamples_ > 0 ? statsIntervalMaxMs_ : 0.0;
    s.timerIntervalMeanMs
        = statsIntervalSamples_ > 0 ? statsIntervalSumMs_ / (double)statsIntervalSamples_ : 0.0;
    s.lastFrameDisplaySamples = frameDisplaySamples_;
    const float x = frameCentreXForCurrentViewport();
    s.lastFrameCentreX = std::isfinite(x) ? (double)x : 0.0;

    statsTimerTicks_ = 0;
    statsRepaintRequests_ = 0;
    statsRepaintAreaPx_ = 0.0;
    statsIntervalSumMs_ = 0.0;
    statsIntervalMinMs_ = 0.0;
    statsIntervalMaxMs_ = 0.0;
    statsIntervalSamples_ = 0;
    return s;
}

float PlayheadOverlay::frameCentreXForCurrentViewport() const noexcept
{
    if (!frameValid_)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return playhead_pixel::snapToPixelCentre(
        playhead_pixel::localXForSample(frameDisplaySamples_,
                                        0.0f,
                                        timelineViewport_.getVisibleStartSamples(),
                                        spp));
}

void PlayheadOverlay::timerCallback()
{
    ++statsTimerTicks_;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (lastTimerWallMs_ >= 0.0)
    {
        const double dt = nowMs - lastTimerWallMs_;
        if (statsIntervalSamples_ == 0)
        {
            statsIntervalMinMs_ = dt;
            statsIntervalMaxMs_ = dt;
        }
        else
        {
            statsIntervalMinMs_ = juce::jmin(statsIntervalMinMs_, dt);
            statsIntervalMaxMs_ = juce::jmax(statsIntervalMaxMs_, dt);
        }
        statsIntervalSumMs_ += dt;
        ++statsIntervalSamples_;
    }
    lastTimerWallMs_ = nowMs;

    const std::int64_t arrLen = session_.getArrangementExtentSamples();

    // Single per-frame sampling of the shared clock; ruler receives exactly this value.
    const bool playing = transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing;
    const double display = uiPlayheadClock_.readDisplaySamples(
        transport_.readPlayheadSamplesForUi(), playing, effectiveDisplaySampleRate(deviceManager_));
    frameDisplaySamples_ = juce::jlimit(0.0, (double)juce::jmax(std::int64_t{0}, arrLen), display);
    if (onPlayheadFrameAdvanced_ != nullptr)
    {
        onPlayheadFrameAdvanced_(frameDisplaySamples_);
    }

    // Read the viewport *after* the frame callback: a follow autoscroll pan inside the callback is
    // then seen as a structural change in this same tick (full repaint + bookkeeping reset) instead
    // of one tick late with stale stripe coordinates.
    const double spp = timelineViewport_.getSamplesPerPixel();
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const auto bounds = getLocalBounds();
    frameValid_ = arrLen > 0 && spp > 0.0;

    if (bounds.isEmpty() || !frameValid_)
    {
        return;
    }

    const bool structuralChanged = visStart != lastVisibleStartUi_ || spp != lastSamplesPerPixelUi_
                                   || arrLen != lastArrangementExtentUi_ || bounds != lastBoundsUi_;
    lastVisibleStartUi_ = visStart;
    lastSamplesPerPixelUi_ = spp;
    lastArrangementExtentUi_ = arrLen;
    lastBoundsUi_ = bounds;

    if (structuralChanged)
    {
        lastInvalidatedCentreX_ = frameCentreXForCurrentViewport();
        ++statsRepaintRequests_;
        statsRepaintAreaPx_ += (double)bounds.getWidth() * (double)bounds.getHeight();
        repaint();
        return;
    }

    invalidateForFrameMove();
}

void PlayheadOverlay::snapFrameDisplaySamplesForSeek(const double displaySamples) noexcept
{
    const std::int64_t arrLen = session_.getArrangementExtentSamples();
    frameDisplaySamples_
        = juce::jlimit(0.0, (double)juce::jmax(std::int64_t{0}, arrLen), displaySamples);
    frameValid_ = arrLen > 0 && timelineViewport_.getSamplesPerPixel() > 0.0;
    if (onPlayheadFrameAdvanced_ != nullptr)
    {
        onPlayheadFrameAdvanced_(frameDisplaySamples_);
    }
    invalidateForFrameMove();
}

void PlayheadOverlay::invalidateForFrameMove() noexcept
{
    const auto bounds = getLocalBounds();
    if (bounds.isEmpty() || !frameValid_)
    {
        return;
    }

    const float x = frameCentreXForCurrentViewport();
    const float prev = lastInvalidatedCentreX_;

    if (!std::isfinite(prev))
    {
        lastInvalidatedCentreX_ = x;
        ++statsRepaintRequests_;
        statsRepaintAreaPx_ += (double)bounds.getWidth() * (double)bounds.getHeight();
        repaint();
        return;
    }

    if (std::isfinite(x) && (int)std::floor(prev) == (int)std::floor(x))
    {
        return;
    }

    const auto invalidate = [this, &bounds](const float centreX) {
        const auto stripe
            = playhead_pixel::dirtyStripe(
                  centreX, bounds.getY(), bounds.getBottom(), kPlayheadStrokeThicknessPx)
                  .getIntersection(bounds);
        if (stripe.isEmpty())
        {
            return;
        }
        ++statsRepaintRequests_;
        statsRepaintAreaPx_ += (double)stripe.getWidth() * (double)stripe.getHeight();
        repaint(stripe);
    };
    invalidate(prev);
    invalidate(x);
    lastInvalidatedCentreX_ = x;
}

void PlayheadOverlay::paint(juce::Graphics& g)
{
    MINIDAW_UI_PAINT_COUNT(overlayPaints);
    // Draws only the stored frame position (never reads the clock): externally triggered repaints
    // (lane content below, window damage) must reproduce the exact line the dirty-rect tracking in
    // `timerCallback` knows about, or stale columns are left behind.
    const juce::Rectangle<float> bounds = getLocalBounds().toFloat();
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
    {
        return;
    }

    const float xLine = frameCentreXForCurrentViewport();
    if (!std::isfinite(xLine) || xLine < bounds.getX() - 2.0f || xLine > bounds.getRight() + 2.0f)
    {
        return;
    }

    g.setColour(juce::Colours::white.withAlpha(0.92f));
    const float bandH = (float)juce::jlimit(0, getHeight(), rulerBandHeightPx_);
    if (bandH > 0.0f)
    {
        // This overlay owns *both* main-arrangement playhead visuals: the short ruler marker
        // segment here (the look `TimelineRulerView` used to paint) and the long lane line below
        // the band, from the same per-frame value so they share one pixel column. The ruler must
        // not draw its own marker: it is buffered to an image to keep playhead stripes off the
        // content paint path, and a marker inside that buffer would go stale (see
        // MAIN_WINDOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md §20). The gap between the marker end and the
        // band bottom is intentional.
        const float markerBottom = juce::jmin(
            bounds.getBottom(),
            bounds.getY() + timeline_locator_paint::kRulerPlayheadMarkerLengthPx);
        g.drawLine(xLine, bounds.getY(), xLine, markerBottom, kPlayheadStrokeThicknessPx);
        g.drawLine(
            xLine, bounds.getY() + bandH, xLine, bounds.getBottom(), kPlayheadStrokeThicknessPx);
    }
    else
    {
        g.drawLine(xLine, bounds.getY(), xLine, bounds.getBottom(), kPlayheadStrokeThicknessPx);
    }
}
