// =============================================================================
// TimelineViewportModel — implementation (message thread)
// =============================================================================

#include "ui/TimelineViewportModel.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace
{
    [[nodiscard]] std::int64_t atLeastOneI64(const std::int64_t v) noexcept
    {
        return juce::jmax(std::int64_t{1}, v);
    }
} // namespace

TimelineViewportModel::TimelineViewportModel(OnVisibleRangeChanged onRangeChanged)
    : onVisibleRangeChanged_(std::move(onRangeChanged))
{
}

void TimelineViewportModel::setOnVisibleRangeChanged(OnVisibleRangeChanged onRangeChanged)
{
    onVisibleRangeChanged_ = std::move(onRangeChanged);
}

std::int64_t TimelineViewportModel::getVisibleStartSamples() const noexcept
{
    return juce::jmax(std::int64_t{0}, visibleStartSamples_);
}

double TimelineViewportModel::getSamplesPerPixel() const noexcept
{
    return samplesPerPixel_;
}

void TimelineViewportModel::setSamplesPerPixelIfUnset(const double samplesPerPixel) noexcept
{
    if (samplesPerPixel_ > 0.0)
    {
        return;
    }
    if (samplesPerPixel <= 0.0 || !std::isfinite(samplesPerPixel))
    {
        return;
    }
    samplesPerPixel_ = samplesPerPixel;
    if (onVisibleRangeChanged_)
    {
        onVisibleRangeChanged_();
    }
}

std::int64_t TimelineViewportModel::getVisibleLengthSamples(const double widthPx) const noexcept
{
    if (samplesPerPixel_ <= 0.0)
    {
        return 1;
    }
    if (widthPx <= 0.0)
    {
        return 1;
    }
    return atLeastOneI64(
        (std::int64_t)std::llround((double)widthPx * samplesPerPixel_));
}

std::int64_t TimelineViewportModel::getVisibleEndSamples(const double widthPx) const noexcept
{
    return getVisibleStartSamples() + getVisibleLengthSamples(widthPx);
}

void TimelineViewportModel::panBySamples(
    const std::int64_t delta, const double widthPx, const std::int64_t arrangementExtent) noexcept
{
    if (samplesPerPixel_ <= 0.0 || delta == 0)
    {
        return;
    }
    if (widthPx <= 0.0)
    {
        return;
    }
    const std::int64_t len = getVisibleLengthSamples(widthPx);
    const std::int64_t ext = juce::jmax(std::int64_t{0}, arrangementExtent);
    if (ext <= 0)
    {
        return;
    }
    std::int64_t s = juce::jmax(std::int64_t{0}, visibleStartSamples_ + delta);
    const std::int64_t maxStart = juce::jmax(std::int64_t{0}, ext - len);
    s = juce::jmin(s, maxStart);
    s = juce::jmin(s, ext);
    if (s == visibleStartSamples_)
    {
        return;
    }
    visibleStartSamples_ = s;
    if (onVisibleRangeChanged_)
    {
        onVisibleRangeChanged_();
    }
}

void TimelineViewportModel::clampToExtent(
    const double widthPx, const std::int64_t arrangementExtent) noexcept
{
    if (samplesPerPixel_ <= 0.0)
    {
        return;
    }
    if (widthPx <= 0.0)
    {
        return;
    }
    const std::int64_t len = getVisibleLengthSamples(widthPx);
    const std::int64_t ext = juce::jmax(std::int64_t{0}, arrangementExtent);
    if (ext <= 0)
    {
        return;
    }
    std::int64_t s = juce::jmax(std::int64_t{0}, visibleStartSamples_);
    if (s + len > ext)
    {
        s = juce::jmax(std::int64_t{0}, ext - len);
    }
    if (s == visibleStartSamples_)
    {
        return;
    }
    visibleStartSamples_ = s;
    if (onVisibleRangeChanged_)
    {
        onVisibleRangeChanged_();
    }
}

void TimelineViewportModel::zoomAroundSample(
    const double factor,
    const double pointerXPx,
    const double widthPx,
    const std::int64_t arrangementExtent,
    const double samplesPerPixelMin,
    const double samplesPerPixelMax) noexcept
{
    if (samplesPerPixel_ <= 0.0)
    {
        return;
    }
    if (widthPx <= 0.0)
    {
        return;
    }
    if (factor <= 0.0 || !std::isfinite(factor))
    {
        return;
    }
    if (std::abs(factor - 1.0) < 1.0e-9)
    {
        return;
    }
    if (!std::isfinite(pointerXPx) || !std::isfinite(widthPx) || !std::isfinite(samplesPerPixelMin)
        || !std::isfinite(samplesPerPixelMax))
    {
        return;
    }
    if (samplesPerPixelMin > samplesPerPixelMax)
    {
        return;
    }
    const std::int64_t ext = juce::jmax(std::int64_t{0}, arrangementExtent);
    if (ext <= 0)
    {
        return;
    }
    const std::int64_t vstart0 = juce::jmax(std::int64_t{0}, visibleStartSamples_);
    const double spp0 = samplesPerPixel_;
    const double xClamped
        = juce::jlimit(0.0, widthPx, static_cast<double>(pointerXPx));
    const std::int64_t sAtPointer
        = vstart0 + (std::int64_t)std::llround(xClamped * spp0);
    double spp1 = juce::jlimit(samplesPerPixelMin, samplesPerPixelMax, spp0 * factor);
    if (!std::isfinite(spp1))
    {
        return;
    }
    std::int64_t vstart1 = sAtPointer - (std::int64_t)std::llround(xClamped * spp1);
    vstart1 = juce::jmax(std::int64_t{0}, vstart1);
    const std::int64_t visLen1 = (std::int64_t)std::llround(widthPx * spp1);
    const std::int64_t visLen1Clamped = juce::jmax(std::int64_t{1}, visLen1);
    const std::int64_t maxStart = juce::jmax(std::int64_t{0}, ext - visLen1Clamped);
    vstart1 = juce::jmin(vstart1, maxStart);
    vstart1 = juce::jmin(vstart1, ext);
    if (vstart1 == visibleStartSamples_ && spp1 == samplesPerPixel_)
    {
        return;
    }
    visibleStartSamples_ = vstart1;
    samplesPerPixel_ = spp1;
    if (onVisibleRangeChanged_)
    {
        onVisibleRangeChanged_();
    }
}
