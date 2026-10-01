#include "ui/LevelMeterComponent.h"

#include <cmath>

namespace
{
    constexpr int kBarWidthPx = 7;
    constexpr int kBarGapPx = 2;
    constexpr int kScaleTickWidthPx = 4;
    constexpr int kScaleLabelWidthPx = 22;
    constexpr int kOverloadLampHeightPx = 9;
    constexpr int kPeakTextHeightPx = 14;
    constexpr int kOuterPadPx = 2;

    constexpr juce::uint32 kBackgroundArgb = 0xff1d1f22;
    constexpr juce::uint32 kBarTroughArgb = 0xff2a2d31;
    constexpr juce::uint32 kBarGreenArgb = 0xff3cb043;
    constexpr juce::uint32 kBarYellowArgb = 0xffd9c23f;
    constexpr juce::uint32 kBarRedArgb = 0xffd93a3a;
    constexpr juce::uint32 kHoldLineArgb = 0xfff2f2f2;
    constexpr juce::uint32 kScaleTextArgb = 0xffb8bcc2;
    constexpr juce::uint32 kOverloadOffArgb = 0xff3a1f1f;
    constexpr juce::uint32 kOverloadOnArgb = 0xffff2e2e;

    [[nodiscard]] double linearToDb(const float linear) noexcept
    {
        if (!std::isfinite(linear))
        {
            return 12.0; // shown pinned at the top; the number says "NaN"
        }
        if (linear <= 1.0e-6f)
        {
            return -200.0;
        }
        return 20.0 * std::log10(static_cast<double>(linear));
    }

    constexpr double kScaleMarksDb[] = { 0.0, -6.0, -12.0, -18.0, -24.0, -30.0, -40.0, -50.0, -60.0 };
} // namespace

LevelMeterComponent::LevelMeterComponent()
{
    setOpaque(false);
    setInterceptsMouseClicks(true, false);
}

void LevelMeterComponent::setChannelCount(const int channels)
{
    const int c = juce::jlimit(1, 2, channels);
    if (c != channels_)
    {
        channels_ = c;
        repaint();
    }
}

void LevelMeterComponent::setShowScale(const bool show)
{
    showScale_ = show;
    repaint();
}

void LevelMeterComponent::setShowScaleLabels(const bool show)
{
    showScaleLabels_ = show;
    repaint();
}

void LevelMeterComponent::setShowPeakText(const bool show)
{
    showPeakText_ = show;
    repaint();
}

int LevelMeterComponent::preferredWidthFor(const int channels, const bool withScaleLabels) noexcept
{
    const int c = juce::jlimit(1, 2, channels);
    return kOuterPadPx * 2 + c * kBarWidthPx + (c - 1) * kBarGapPx + kScaleTickWidthPx
           + (withScaleLabels ? kScaleLabelWidthPx : 0);
}

void LevelMeterComponent::pushReading(const level_meter::Reading& reading, const double nowSeconds)
{
    if (reading.blocks == 0)
    {
        return; // nothing rendered since the last drain (track off / not rendered): keep falling
    }
    if (reading.channels > 0)
    {
        setChannelCount(reading.channels);
    }
    for (int ch = 0; ch < channels_; ++ch)
    {
        const double db = linearToDb(reading.peak[ch]);
        if (db > displayedDb_[ch])
        {
            displayedDb_[ch] = db; // instantaneous rise
        }
        if (reading.peak[ch] > heldPeak_[ch] || !std::isfinite(reading.peak[ch]))
        {
            heldPeak_[ch] = reading.peak[ch];
            heldPeakRiseTime_[ch] = nowSeconds;
        }
    }
    const juce::uint32 overs = reading.overs[0] + reading.overs[1];
    if (overs > 0 || reading.nonFinite > 0)
    {
        overloadLatched_ = true;
        overloadSamples_ += overs;
        nonFiniteSeen_ = nonFiniteSeen_ || reading.nonFinite > 0;
    }
    double dcMax = 0.0;
    for (int ch = 0; ch < channels_; ++ch)
    {
        dcMax = juce::jmax(dcMax, std::abs(reading.dcOffset(ch)));
    }
    lastDc_ = dcMax;
    dcTag_ = dcMax > kDcTagThreshold;
    repaint();
}

void LevelMeterComponent::tick(const double nowSeconds)
{
    if (lastTickSeconds_ <= 0.0)
    {
        lastTickSeconds_ = nowSeconds;
        return;
    }
    const double dt = juce::jlimit(0.0, 0.5, nowSeconds - lastTickSeconds_);
    lastTickSeconds_ = nowSeconds;
    bool changed = false;
    for (int ch = 0; ch < 2; ++ch)
    {
        if (displayedDb_[ch] > kMinDb)
        {
            displayedDb_[ch] = juce::jmax(kMinDb, displayedDb_[ch] - kFallDbPerSecond * dt);
            changed = true;
        }
        // Peak hold: after the hold time the held value follows the bar down (release), but a
        // held overload stays visible in the latch + numeric text until the user resets it.
        if (heldPeak_[ch] > 0.0f && std::isfinite(heldPeak_[ch]) && nowSeconds - heldPeakRiseTime_[ch] > kPeakHoldSeconds)
        {
            const double barLinear = displayedDb_[ch] <= kMinDb ? 0.0 : std::pow(10.0, displayedDb_[ch] / 20.0);
            if (barLinear < heldPeak_[ch])
            {
                heldPeak_[ch] = static_cast<float>(juce::jmax(0.0, barLinear));
                changed = true;
            }
        }
    }
    if (changed)
    {
        repaint();
    }
}

void LevelMeterComponent::clear()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        displayedDb_[ch] = kMinDb;
        heldPeak_[ch] = 0.0f;
        heldPeakRiseTime_[ch] = 0.0;
    }
    overloadLatched_ = false;
    overloadSamples_ = 0;
    nonFiniteSeen_ = false;
    dcTag_ = false;
    lastDc_ = 0.0;
    repaint();
}

void LevelMeterComponent::resetOverloadLatch()
{
    overloadLatched_ = false;
    overloadSamples_ = 0;
    nonFiniteSeen_ = false;
    for (int ch = 0; ch < 2; ++ch)
    {
        // The held number may still show the over-full-scale value; releasing the latch also
        // releases the hold so the display returns to the live level.
        heldPeak_[ch] = 0.0f;
    }
    repaint();
    if (onOverloadLatchReset != nullptr)
    {
        onOverloadLatchReset();
    }
}

juce::String LevelMeterComponent::getPeakText() const
{
    if (nonFiniteSeen_)
    {
        return "NaN";
    }
    return level_meter::peakToDbfsText(getHeldPeakLinear());
}

double LevelMeterComponent::getDisplayedDb(const int channel) const noexcept
{
    return (channel >= 0 && channel < 2) ? displayedDb_[channel] : kMinDb;
}

float LevelMeterComponent::yForDb(const double db, const juce::Rectangle<int> barArea) noexcept
{
    const double t = juce::jlimit(0.0, 1.0, (kMaxDb - db) / (kMaxDb - kMinDb)); // 0 at top
    return static_cast<float>(barArea.getY()) + static_cast<float>(t) * static_cast<float>(barArea.getHeight());
}

juce::Rectangle<int> LevelMeterComponent::overloadLampBounds() const noexcept
{
    const juce::Rectangle<int> bars = getBarsBounds();
    return { bars.getX(), kOuterPadPx, bars.getWidth(), kOverloadLampHeightPx };
}

juce::Rectangle<int> LevelMeterComponent::peakTextBounds() const noexcept
{
    if (!showPeakText_)
    {
        return {};
    }
    return { 0, getHeight() - kOuterPadPx - kPeakTextHeightPx, getWidth(), kPeakTextHeightPx };
}

juce::Rectangle<int> LevelMeterComponent::getBarsBounds() const noexcept
{
    const int barsW = channels_ * kBarWidthPx + (channels_ - 1) * kBarGapPx;
    const int top = kOuterPadPx + kOverloadLampHeightPx + 2;
    const int bottom = getHeight() - kOuterPadPx - (showPeakText_ ? kPeakTextHeightPx + 2 : 0);
    return { kOuterPadPx, top, barsW, juce::jmax(1, bottom - top) };
}

juce::Rectangle<int> LevelMeterComponent::scaleBounds() const noexcept
{
    const juce::Rectangle<int> bars = getBarsBounds();
    return { bars.getRight() + 1, bars.getY(), getWidth() - bars.getRight() - 1 - kOuterPadPx, bars.getHeight() };
}

void LevelMeterComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kBackgroundArgb));
    const juce::Rectangle<int> bars = getBarsBounds();
    if (bars.getHeight() <= 2)
    {
        return;
    }

    // Overload lamp: red while latched, with the count; dark when clear. Click = reset.
    {
        const juce::Rectangle<int> lamp = overloadLampBounds();
        g.setColour(juce::Colour(overloadLatched_ ? kOverloadOnArgb : kOverloadOffArgb));
        g.fillRoundedRectangle(lamp.toFloat(), 2.0f);
    }

    for (int ch = 0; ch < channels_; ++ch)
    {
        const juce::Rectangle<int> bar(bars.getX() + ch * (kBarWidthPx + kBarGapPx), bars.getY(), kBarWidthPx, bars.getHeight());
        g.setColour(juce::Colour(kBarTroughArgb));
        g.fillRect(bar);

        const double db = displayedDb_[ch];
        if (db > kMinDb)
        {
            const float yTop = yForDb(db, bar);
            const float yYellow = yForDb(-12.0, bar);
            const float yRed = yForDb(-3.0, bar);
            // Green up to −12, yellow to −3, red above — painted as stacked segments.
            const float bottom = static_cast<float>(bar.getBottom());
            g.setColour(juce::Colour(kBarGreenArgb));
            g.fillRect(juce::Rectangle<float>((float)bar.getX(), juce::jmax(yTop, yYellow), (float)bar.getWidth(),
                                              bottom - juce::jmax(yTop, yYellow)));
            if (yTop < yYellow)
            {
                g.setColour(juce::Colour(kBarYellowArgb));
                g.fillRect(juce::Rectangle<float>((float)bar.getX(), juce::jmax(yTop, yRed), (float)bar.getWidth(),
                                                  yYellow - juce::jmax(yTop, yRed)));
            }
            if (yTop < yRed)
            {
                g.setColour(juce::Colour(kBarRedArgb));
                g.fillRect(juce::Rectangle<float>((float)bar.getX(), yTop, (float)bar.getWidth(), yRed - yTop));
            }
        }
        // Peak hold line (clamped into the bar; an over-full-scale hold sits on the top edge).
        if (heldPeak_[ch] > 0.0f)
        {
            const float yHold = yForDb(linearToDb(heldPeak_[ch]), bar);
            g.setColour(juce::Colour(heldPeak_[ch] > 1.0f ? kOverloadOnArgb : kHoldLineArgb));
            g.fillRect(juce::Rectangle<float>((float)bar.getX(), yHold, (float)bar.getWidth(), 1.5f));
        }
    }

    if (showScale_)
    {
        const juce::Rectangle<int> sc = scaleBounds();
        g.setColour(juce::Colour(kScaleTextArgb));
        g.setFont(juce::FontOptions(9.0f));
        float lastLabelY = -1000.0f;
        for (const double mark : kScaleMarksDb)
        {
            const float y = yForDb(mark, bars);
            g.fillRect(juce::Rectangle<float>((float)sc.getX(), y, (float)kScaleTickWidthPx, 1.0f));
            // Labels only where they do not collide on short meters (ticks always drawn).
            if (showScaleLabels_ && sc.getWidth() >= kScaleTickWidthPx + 12 && (y - lastLabelY) >= 11.0f)
            {
                lastLabelY = y;
                juce::String label = mark == 0.0 ? "0" : juce::String(static_cast<int>(mark));
                if (mark < 0.0)
                {
                    label = juce::String(juce::CharPointer_UTF8("\xe2\x88\x92")) + juce::String(static_cast<int>(-mark));
                }
                g.drawText(label,
                           juce::Rectangle<int>(sc.getX() + kScaleTickWidthPx + 1, (int)std::lround(y) - 6,
                                                sc.getWidth() - kScaleTickWidthPx - 1, 12),
                           juce::Justification::centredLeft, false);
            }
        }
    }

    if (showPeakText_)
    {
        const juce::Rectangle<int> t = peakTextBounds();
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(juce::Colour(overloadLatched_ ? kOverloadOnArgb : kScaleTextArgb));
        juce::String text = getPeakText();
        if (dcTag_)
        {
            text << " DC";
        }
        g.drawText(text, t, juce::Justification::centred, false);
    }
    else if (dcTag_)
    {
        g.setFont(juce::FontOptions(9.0f));
        g.setColour(juce::Colour(kBarYellowArgb));
        g.drawText("DC", juce::Rectangle<int>(0, getHeight() - kOuterPadPx - 11, getWidth(), 11), juce::Justification::centred, false);
    }
}

void LevelMeterComponent::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    if (overloadLatched_)
    {
        resetOverloadLatch();
    }
}
