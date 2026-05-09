#include "ui/PlayheadOverlay.h"

#include "ui/TimelineRulerView.h"
#include "transport/Transport.h"

namespace
{
    constexpr int kPlayheadUpdateHz = 20;
} // namespace

PlayheadOverlay::PlayheadOverlay(
    Session& session,
    Transport& transport,
    TimelineViewportModel& timelineViewport)
    : session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
{
    setInterceptsMouseClicks(false, false);
    startTimerHz(kPlayheadUpdateHz);
}

PlayheadOverlay::~PlayheadOverlay()
{
    stopTimer();
}

void PlayheadOverlay::timerCallback()
{
    repaint();
}

void PlayheadOverlay::paint(juce::Graphics& g)
{
    juce::Rectangle<float> bounds = getLocalBounds().toFloat();
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
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }

    const double wPx = (double)bounds.getWidth();
    const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples(wPx);

    const std::int64_t ph = transport_.readPlayheadSamplesForUi();
    const std::int64_t phClamped
        = juce::jlimit(std::int64_t{ 0 }, juce::jmax(std::int64_t{ 0 }, arrLen), ph);

    if (phClamped >= visStart && phClamped < visStart + visLen)
    {
        const float xLine
            = TimelineRulerView::sessionSampleToLocalX(phClamped, bounds.getX(), visStart, spp);
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.drawLine(xLine, bounds.getY(), xLine, bounds.getBottom(), 1.5f);
    }
}
