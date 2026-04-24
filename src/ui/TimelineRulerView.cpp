// =============================================================================
// TimelineRulerView.cpp  —  seek strip + second ticks (message thread)
// =============================================================================
//
// ROLE
//   Maps horizontal pointer position to a **session-timeline-absolute** sample index (same
//   `timelineLength` / `Transport` contract as the lane) and issues `requestSeek` so the user can
//   place the playhead without using the event lane. Ticks are **only** a spatial hint (seconds
//   along the line); they do not introduce a new timebase, tempo, or bar grid.
//
// PEDAGOGICAL GOAL
//   A reader should see *why* sample rate appears here: ticks are “every N **seconds** of session
//   time,” and seconds are derived from `samples / deviceRate` for **drawing** only. Playback and
//   placement still live in **samples** end-to-end.
//
// THREADING
//   All methods here are [Message thread] — JUCE component + timer, no audio device callback.
// =============================================================================

#include "ui/TimelineRulerView.h"

#include "transport/Transport.h"

#include <cmath>
#include <cstdint>

namespace
{
    // Match `ClipWaveformView` so ruler playhead and lane playhead move on the same cadence.
    constexpr int kPlayheadUpdateHz = 20;

    // Ticks at 1s, 5s, …: pick the **coarsest** step that still leaves at least this many pixels
    // between second-boundary lines so a long session does not become a solid bar.
    constexpr float kMinTickSpacingPx = 6.0f;

    // Candidate step sizes in **seconds** (plain integers; no bar/beat). Search in order: first
    // that satisfies minimum pixel spacing for the current window and timeline duration wins.
    constexpr int kStepCandidatesSec[] = { 1, 5, 10, 30, 60, 300, 600, 3600 };
    constexpr int kNumStepCandidates = (int)(sizeof(kStepCandidatesSec) / sizeof(kStepCandidatesSec[0]));

    // Ruler playhead: short vertical at the top so the hairline in the lane has a “handle” in time.
    constexpr float kPlayheadMarkerLengthPx = 7.0f;
} // namespace

TimelineRulerView::TimelineRulerView(Session& session,
                                     Transport& transport,
                                     juce::AudioDeviceManager& deviceManager)
    : session_(session)
    , transport_(transport)
    , deviceManager_(deviceManager)
{
    setInterceptsMouseClicks(true, false);
    startTimerHz(kPlayheadUpdateHz);
}

TimelineRulerView::~TimelineRulerView()
{
    stopTimer();
}

void TimelineRulerView::timerCallback()
{
    repaint();
}

std::int64_t TimelineRulerView::xToSessionSampleClamped(
    const float positionX, const float widthPx, const std::int64_t timelineLength) noexcept
{
    if (timelineLength <= 0 || widthPx <= 0.0f)
    {
        return 0;
    }
    const float t = juce::jlimit(0.0f, 1.0f, positionX / widthPx);
    return juce::jlimit(
        static_cast<std::int64_t>(0),
        timelineLength,
        static_cast<std::int64_t>(std::llround((double)t * (double)timelineLength)));
}

void TimelineRulerView::applySeekForLocalX(const float x) noexcept
{
    const std::int64_t len = session_.getTimelineLengthSamples();
    if (len <= 0)
    {
        return;
    }
    const float w = (float)getWidth();
    if (w <= 0.0f)
    {
        return;
    }
    transport_.requestSeek(xToSessionSampleClamped(x, w, len));
    repaint();
}

void TimelineRulerView::mouseDown(const juce::MouseEvent& e)
{
    applySeekForLocalX(e.position.x);
}

void TimelineRulerView::mouseDrag(const juce::MouseEvent& e)
{
    // Each move queues a new seek; the audio thread applies the latest pending target per block.
    applySeekForLocalX(e.position.x);
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

    const std::int64_t timelineLength = session_.getTimelineLengthSamples();
    if (timelineLength <= 0)
    {
        return;
    }

    const double wPx = (double)bounds.getWidth();
    const double tlenD = (double)timelineLength;
    // **Same** linear map as `ClipWaveformView::paint` for the playhead: sample s → x.
    const auto sessionSampleToX = [&](const std::int64_t s) {
        return (float)(bounds.getX() + wPx * ((double)s / tlenD));
    };

    juce::AudioIODevice* const device = deviceManager_.getCurrentAudioDevice();
    const double sampleRate = (device != nullptr) ? device->getCurrentSampleRate() : 0.0;

    if (sampleRate > 0.0)
    {
        // --- Ticks: round **seconds** only; step widens when the same 1s grid would pack tighter
        //     than `kMinTickSpacingPx`. Product: a readable rhythm of marks, not a label strip.
        const double spanSec = tlenD / sampleRate;
        if (spanSec > 0.0)
        {
            // First candidate whose tick spacing in pixels meets the minimum; if even 1s is too
            // dense, fall through to coarser steps until one fits.
            int stepSec = kStepCandidatesSec[kNumStepCandidates - 1];
            for (int candidate : kStepCandidatesSec)
            {
                const double spacingPx = ((double)candidate * wPx) / spanSec;
                if (spacingPx >= (double)kMinTickSpacingPx)
                {
                    stepSec = candidate;
                    break;
                }
            }
            g.setColour(juce::Colour(0xff7a8aa0).withAlpha(0.55f));
            for (int k = 0;; ++k)
            {
                const std::int64_t s = (std::int64_t)std::llround((double)k * (double)stepSec
                                                                  * sampleRate);
                if (s >= timelineLength)
                {
                    break;
                }
                if (s < 0)
                {
                    continue;
                }
                const float x = sessionSampleToX(s);
                const float hShort = juce::jmax(3.0f, bounds.getHeight() * 0.35f);
                g.drawLine(
                    x,
                    bounds.getBottom() - 1.0f,
                    x,
                    bounds.getBottom() - 1.0f - hShort,
                    1.0f);
            }
        }
    }

    // --- Playhead: read transport (authoritative for UI) and clamp for display, same as the lane.
    const std::int64_t ph = transport_.readPlayheadSamplesForUi();
    const std::int64_t phClamped
        = juce::jlimit(static_cast<std::int64_t>(0), timelineLength, ph);
    const float t = (float)((double)phClamped / tlenD);
    const float xLine = bounds.getX() + t * (float)wPx;
    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.drawLine(
        xLine,
        bounds.getY(),
        xLine,
        bounds.getY() + kPlayheadMarkerLengthPx,
        1.5f);
}
