// =============================================================================
// TimelineViewportModel — implementation (message thread)
// =============================================================================

#include "ui/TimelineViewportModel.h"

#include <juce_core/juce_core.h>

namespace
{
    [[nodiscard]] std::int64_t atLeastOne(const std::int64_t v) noexcept
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

std::int64_t TimelineViewportModel::getVisibleLengthSamples() const noexcept
{
    return atLeastOne(visibleLengthSamples_);
}

std::int64_t TimelineViewportModel::getVisibleEndSamples() const noexcept
{
    return getVisibleStartSamples() + getVisibleLengthSamples();
}

void TimelineViewportModel::setVisibleLengthIfUnset(const std::int64_t samples) noexcept
{
    if (visibleLengthSamples_ > 0)
    {
        return;
    }
    const std::int64_t v = atLeastOne(samples);
    visibleLengthSamples_ = v;
    if (onVisibleRangeChanged_)
    {
        onVisibleRangeChanged_();
    }
}

void TimelineViewportModel::panBySamples(
    const std::int64_t delta, const std::int64_t arrangementExtent) noexcept
{
    if (visibleLengthSamples_ <= 0 || delta == 0)
    {
        return;
    }
    const std::int64_t len = atLeastOne(visibleLengthSamples_);
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

void TimelineViewportModel::clampToExtent(const std::int64_t arrangementExtent) noexcept
{
    if (visibleLengthSamples_ <= 0)
    {
        return;
    }
    const std::int64_t len = atLeastOne(visibleLengthSamples_);
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
