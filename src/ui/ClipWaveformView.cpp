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

void ClipWaveformView::timerCallback()
{
    repaint();
}

void ClipWaveformView::mouseDown(const juce::MouseEvent& e)
{
    const auto* const clip = session_.getCurrentClip();
    if (clip == nullptr)
        return;

    const int numSamplesI = clip->getNumSamples();
    if (numSamplesI <= 0)
        return;

    const juce::Rectangle<float> b = getLocalBounds().toFloat();
    if (b.getWidth() <= 0.0f)
        return;

    // Same t = x / width model as the playhead line (full clip width in local coordinates).
    const float t = juce::jlimit(0.0f, 1.0f, e.position.x / b.getWidth());
    const std::int64_t numS = static_cast<std::int64_t>(numSamplesI);
    const std::int64_t target = juce::jlimit(
        static_cast<std::int64_t>(0),
        numS,
        static_cast<std::int64_t>(std::llround((double)t * (double)numS)));

    transport_.requestSeek(target);
    repaint();
}

void ClipWaveformView::rebuildPeaksIfNeeded()
{
    const auto* const clip = session_.getCurrentClip();
    const int w = juce::jmax(1, getWidth());

    if (clip == lastClip_ && w == lastWidth_)
        return;

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
