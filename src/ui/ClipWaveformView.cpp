// =============================================================================
// ClipWaveformView.cpp  —  see ClipWaveformView.h for role; this file is layout + draw only
// =============================================================================
//
// RENDERING PIPELINE (browse order)
//   1) timerCallback (periodic) → repaint() → 2) paint
//   2) paint → rebuildPeaksIfNeeded: if clip or view width changed, recompute a fixed number of
//      columns of peak values (downsampled max-abs) for a cheap waveform sketch.
//   3) paint draws those columns as vertical bars, then draws a vertical line for the playhead
//      using the same 0..numSamples-1 index space as the audio engine uses in Phase 1.
//
// JUCE
//   • Timer / startTimerHz — periodic “tick” on the message thread.
//   • jlimit — clamp values to a range (e.g. playhead, click position).
//   • findColour / fillAll / drawLine — standard Component drawing; not audio.
// =============================================================================

#include "ui/ClipWaveformView.h"

#include "domain/AudioClip.h"
#include "transport/Transport.h"

#include <cmath> // std::abs, std::llround

namespace
{
    constexpr int kMaxPeakColumns = 4000;
    constexpr int kPlayheadUpdateHz = 20;
}

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
    // The playhead moves on the audio side every block; we only need a steady frame rate to
    // re-read it in paint. No audio data is touched here — this is a visual refresh, not a sync.
    repaint();
}

// [Message thread] Click-to-seek: same 0..numSamples-1 index space the engine uses in Phase 1.
void ClipWaveformView::mouseDown(const juce::MouseEvent& e)
{
    const auto* const clip = session_.getCurrentClip();
    if (clip == nullptr)
    {
        // No clip: nothing to align a seek with — the waveform is empty.
        return;
    }

    const int numSamplesI = clip->getNumSamples();
    if (numSamplesI <= 0)
        return;

    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
        return;

    // Map horizontal click to a *fraction* of the file, then to a sample index. We do not
    // set the playhead here — that stays Transport’s job; we only queue a seek the audio
    // callback will apply (same contract as the transport/playback docs).
    const float t = juce::jlimit(0.0f, 1.0f, e.position.x / b.getWidth());
    const std::int64_t numS = static_cast<std::int64_t>(numSamplesI);
    const std::int64_t target = juce::jlimit(
        static_cast<std::int64_t>(0),
        numS,
        static_cast<std::int64_t>(std::llround((double)t * (double)numS)));

    transport_.requestSeek(target);
    repaint();
}

// [Message thread]
void ClipWaveformView::rebuildPeaksIfNeeded()
{
    const auto* const clip = session_.getCurrentClip();
    const int w = juce::jmax(1, getWidth());

    if (clip == lastClip_ && w == lastWidth_)
    {
        // Clip identity and view width are unchanged — peak array is still valid.
        return;
    }

    lastClip_ = clip;
    lastWidth_ = w;
    peaks_.clear();

    if (clip == nullptr)
        return;

    const juce::AudioBuffer<float>& audio = clip->getAudio();
    const int numCh = audio.getNumChannels();
    const int numSamples = audio.getNumSamples();
    if (numCh <= 0 || numSamples <= 0)
        return;

    // Visual-only: split the file into a fixed number of screen columns; each column is the
    // max abs sample across *all* channels in that time slice — a cheap sketch, not a loudness
    // meter. At most kMaxPeakColumns so very wide views do not make this path costly on the UI thread.
    const int numColumns = juce::jmin(w, kMaxPeakColumns);
    peaks_.resize((size_t)numColumns, 0.0f);

    for (int col = 0; col < numColumns; ++col)
    {
        const int s0 = (col * numSamples) / numColumns;
        const int s1 = ((col + 1) * numSamples) / numColumns;
        float peak = 0.0f;
        for (int s = s0; s < s1; ++s)
        {
            for (int c = 0; c < numCh; ++c)
            {
                const float v = audio.getSample(c, s);
                peak = juce::jmax(peak, std::abs(v));
            }
        }
        peaks_[(size_t)col] = juce::jlimit(0.0f, 1.0f, peak);
    }
}

// [Message thread]
void ClipWaveformView::paint(juce::Graphics& g)
{
    rebuildPeaksIfNeeded();

    g.fillAll(findColour(juce::ResizableWindow::backgroundColourId).darker(0.2f));

    auto bounds = getLocalBounds().toFloat();
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    const float midY = bounds.getCentreY();
    const float halfDraw = bounds.getHeight() * 0.45f;
    g.setColour(juce::Colours::lightblue);

    if (!peaks_.empty())
    {
        // Draw the precomputed “mountain line” of bars — height is proportional to the peak
        // value in each column, centered vertically so the sketch reads like a traditional waveform.
        const int n = (int)peaks_.size();
        const float colW = bounds.getWidth() / (float)n;

        for (int i = 0; i < n; ++i)
        {
            const float h = peaks_[(size_t)i] * halfDraw;
            const float x = bounds.getX() + (float)i * colW;
            g.fillRect(x, midY - h, juce::jmax(1.0f, colW), h * 2.0f);
        }
    }

    if (const auto* const clip = session_.getCurrentClip())
    {
        const int numSamplesI = clip->getNumSamples();
        if (numSamplesI > 0)
        {
            // Playhead: same sample index the engine uses, read here for *display* only. Clamp so
            // a transient read never draws off the right edge; that keeps the line aligned with
            // what the user hears in Phase 1.
            const std::int64_t numSamples = static_cast<std::int64_t>(numSamplesI);
            const std::int64_t ph = transport_.readPlayheadSamplesForUi();
            const std::int64_t phClamped = juce::jlimit(
                static_cast<std::int64_t>(0),
                numSamples,
                ph);
            const float t = (float)phClamped / (float)numSamples;
            const float x = bounds.getX() + t * bounds.getWidth();

            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 1.5f);
        }
    }
}
