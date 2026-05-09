#include "ui/TimelineLocatorPainter.h"

#include <cmath>
#include <cstdint>

namespace timeline_locator_paint
{
    static constexpr float kMinTickSpacingPx = 6.0f;

    static constexpr double kStepCandidatesSec[]
        = { 0.1,  0.25, 0.5,  1.0,  2.0,  5.0,  10.0, 30.0,
            60.0, 300.0, 600.0, 3600.0 };
    static constexpr int kNumStepCandidates
        = (int)(sizeof(kStepCandidatesSec) / sizeof(kStepCandidatesSec[0]));

    [[nodiscard]] static float glyphLayoutWidthPx(const juce::Font& f, const juce::String& text) noexcept
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

    [[nodiscard]] static float referenceLabelMinSpacingPx() noexcept
    {
        const juce::Font f(juce::FontOptions(11.0f));
        return glyphLayoutWidthPx(f, "00:00.000") + 8.0f;
    }

    [[nodiscard]] static double pickTickStepSec(const double pxPerSec) noexcept
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

    [[nodiscard]] static double pickLabelStepSec(const double pxPerSec, const double tickStepSec) noexcept
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

    [[nodiscard]] static juce::String formatRulerTimecode(
        const double seconds, const double stepSec) noexcept
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

    [[nodiscard]] static bool rulerTickPaintCommon(
        const juce::Rectangle<float>& bounds,
        const std::int64_t visLen,
        const double sampleRate,
        double& outPxPerSec,
        double& outTickStepSec,
        double& outLabelStepSec,
        float& outHShort)
    {
        if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        {
            return false;
        }
        const double sppProxy = (double)visLen / (double)juce::jmax(1.0f, bounds.getWidth());
        if (sppProxy <= 0.0 || !std::isfinite(sppProxy))
        {
            return false;
        }
        outPxPerSec = sampleRate / sppProxy;
        outHShort = juce::jmax(3.0f, bounds.getHeight() * 0.35f);
        if (outPxPerSec <= 0.0 || !std::isfinite(outPxPerSec))
        {
            return false;
        }
        outTickStepSec = pickTickStepSec(outPxPerSec);
        outLabelStepSec = pickLabelStepSec(outPxPerSec, outTickStepSec);
        return true;
    }

    static constexpr float kLocatorTriangleHeight = 9.0f;

    static void fillDownPointingLocatorTriangle(juce::Graphics& g,
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

void paintLocatorCycleBandAndStripe(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    const std::int64_t visibleStartSamples,
    const std::int64_t visibleLengthSamples,
    const std::int64_t leftLocatorSamples,
    const std::int64_t rightLocatorSamples,
    const bool cycleEnabled)
{
    juce::ignoreUnused(visibleStartSamples, visibleLengthSamples);

    if (rightLocatorSamples <= 0)
    {
        return;
    }

    constexpr float kLocatorBandAlphaCycleOn = 0.58f;
    constexpr float kLocatorBandAlphaCycleOff = 0.30f;
    constexpr float kLocatorBandAlphaInvalid = 0.58f;
    constexpr float kLocatorTopStripeFrac = 0.45f;

    constexpr juce::uint32 kFillPurpleBlue = 0xff7058e8u;
    constexpr juce::uint32 kFillNeutralGray = 0xffb8c2d8u;
    constexpr juce::uint32 kFillInvalidOrange = 0xfff06828u;

    const float x0 = sampleToX(leftLocatorSamples);
    const float x1 = sampleToX(rightLocatorSamples);
    const float fillLeft = juce::jmin(x0, x1);
    const float fillRight = juce::jmax(x0, x1);
    const float clipL = juce::jmax(fillLeft, rulerBounds.getX());
    const float clipR = juce::jmin(fillRight, rulerBounds.getRight());
    const float bandW = clipR - clipL;

    const bool validInterval = rightLocatorSamples > leftLocatorSamples;
    const juce::Colour mainFillCol = validInterval
        ? (cycleEnabled ? juce::Colour(kFillPurpleBlue).withAlpha(kLocatorBandAlphaCycleOn)
                        : juce::Colour(kFillNeutralGray).withAlpha(kLocatorBandAlphaCycleOff))
        : juce::Colour(kFillInvalidOrange).withAlpha(kLocatorBandAlphaInvalid);

    const juce::Colour stripeFillCol = validInterval
        ? (cycleEnabled ? juce::Colour(kFillPurpleBlue).withAlpha(0.72f)
                        : juce::Colour(kFillNeutralGray).withAlpha(0.46f))
        : juce::Colour(kFillInvalidOrange).withAlpha(0.76f);

    if (bandW > 0.0f)
    {
        const float stripeH
            = juce::jlimit(3.0f, rulerBounds.getHeight() * kLocatorTopStripeFrac, 11.5f);

        g.setColour(mainFillCol);
        g.fillRect(
            juce::Rectangle<float>(clipL, rulerBounds.getY(), bandW, rulerBounds.getHeight()));

        g.setColour(stripeFillCol);
        g.fillRect(juce::Rectangle<float>(clipL, rulerBounds.getY(), bandW, stripeH));

        const juce::Colour edgeGlow = stripeFillCol.withAlpha(
            juce::jmin(1.0f, stripeFillCol.getFloatAlpha() * 1.06f));
        g.setColour(edgeGlow);
        g.fillRect(juce::Rectangle<float>(clipL, rulerBounds.getY(), bandW, 1.25f));
        g.fillRect(juce::Rectangle<float>(clipL, rulerBounds.getBottom() - 2.35f, bandW, 1.85f));
    }
}

void paintLocatorTriangleHandles(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    const std::int64_t visibleStartSamples,
    const std::int64_t visibleLengthSamples,
    const std::int64_t leftLocatorSamples,
    const std::int64_t rightLocatorSamples,
    const bool cycleEnabled)
{
    const auto visEnd = visibleStartSamples + visibleLengthSamples;

    const auto drawTriangleHandleIfVisibleWithClip = [&](const std::int64_t s,
                                                       const juce::Colour& fill,
                                                       const juce::Colour& outline) {
        if (s < visibleStartSamples || s >= visEnd)
        {
            return;
        }
        const float xCenter = sampleToX(s);
        if (xCenter < rulerBounds.getX() - 14.0f || xCenter > rulerBounds.getRight() + 14.0f)
        {
            return;
        }
        fillDownPointingLocatorTriangle(g, xCenter, rulerBounds.getY(), fill, outline);
    };

    if (rightLocatorSamples > 0)
    {
        const bool validInterval = rightLocatorSamples > leftLocatorSamples;
        const juce::Colour handleOutline = juce::Colours::white.withAlpha(0.62f);

        juce::Colour triLFill;
        juce::Colour triRFill;
        if (!validInterval)
        {
            triLFill = juce::Colour(0xffffc070).withAlpha(0.98f);
            triRFill = juce::Colour(0xffff9640).withAlpha(0.98f);
        }
        else if (cycleEnabled)
        {
            triLFill = juce::Colour(0xffeae2ff).withAlpha(0.98f);
            triRFill = juce::Colour(0xfffff0dc).withAlpha(0.97f);
        }
        else
        {
            triLFill = juce::Colour(0xfff2f5fb).withAlpha(0.97f);
            triRFill = juce::Colour(0xffe4e9f5).withAlpha(0.97f);
        }

        drawTriangleHandleIfVisibleWithClip(leftLocatorSamples, triLFill, handleOutline);
        drawTriangleHandleIfVisibleWithClip(rightLocatorSamples, triRFill, handleOutline);
    }
    else
    {
        const juce::Colour soloLFill = juce::Colour(0xffd8dee8).withAlpha(0.96f);
        const juce::Colour soloLOutline = juce::Colours::white.withAlpha(0.48f);
        if (leftLocatorSamples >= visibleStartSamples && leftLocatorSamples < visEnd)
        {
            const float xc = sampleToX(leftLocatorSamples);
            if (xc >= rulerBounds.getX() - 14.0f && xc <= rulerBounds.getRight() + 14.0f)
            {
                fillDownPointingLocatorTriangle(g, xc, rulerBounds.getY(), soloLFill, soloLOutline);
            }
        }
    }
}

void paintRulerTickMarks(
    juce::Graphics& g,
    const juce::Rectangle<float>& bounds,
    const std::function<float(std::int64_t)>& sampleToX,
    const std::int64_t arrLen,
    const std::int64_t visStart,
    const std::int64_t visLen,
    const double sampleRate)
{
    if (arrLen <= 0)
    {
        return;
    }

    double pxPerSec = 0.0;
    double tickStepSec = 0.0;
    double labelStepSec = 0.0;
    float hShort = 0.0f;
    if (!rulerTickPaintCommon(bounds, visLen, sampleRate, pxPerSec, tickStepSec, labelStepSec, hShort))
    {
        return;
    }
    juce::ignoreUnused(labelStepSec);

    g.setColour(juce::Colour(0xff7a8aa0).withAlpha(0.55f));
    for (int k = 0;; ++k)
    {
        const std::int64_t s = (std::int64_t)std::llround((double)k * tickStepSec * sampleRate);
        if (s >= arrLen)
        {
            break;
        }
        if (s < visStart || s >= visStart + visLen)
        {
            continue;
        }
        const float x = sampleToX(s);
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
}

void paintRulerTimeLabels(
    juce::Graphics& g,
    const juce::Rectangle<float>& bounds,
    const std::function<float(std::int64_t)>& sampleToX,
    const std::int64_t arrLen,
    const std::int64_t visStart,
    const std::int64_t visLen,
    const double sampleRate,
    const std::int64_t locL,
    const std::int64_t locR)
{
    if (arrLen <= 0)
    {
        return;
    }

    double pxPerSec = 0.0;
    double tickStepSec = 0.0;
    double labelStepSec = 0.0;
    float hShort = 0.0f;
    if (!rulerTickPaintCommon(bounds, visLen, sampleRate, pxPerSec, tickStepSec, labelStepSec, hShort))
    {
        return;
    }
    juce::ignoreUnused(tickStepSec, pxPerSec);

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
        const float x = sampleToX(samp);
        if (x <= bounds.getX() || x >= bounds.getRight())
        {
            continue;
        }

        if (locL >= visStart && locL < visStart + visLen)
        {
            const float xcL = sampleToX(locL);
            if (xcL >= bounds.getX() - 14.0f && xcL <= bounds.getRight() + 14.0f
                && std::abs(x - xcL) < skipRadius)
            {
                continue;
            }
        }
        if (locR > 0 && locR >= visStart && locR < visStart + visLen)
        {
            const float xcR = sampleToX(locR);
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

} // namespace timeline_locator_paint
