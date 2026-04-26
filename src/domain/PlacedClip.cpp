// =============================================================================
// PlacedClip.cpp  —  construction of a single placement (message-thread / snapshot build path)
// =============================================================================

#include "domain/PlacedClip.h"

#include "domain/AudioClip.h"

#include <juce_core/juce_core.h>

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline) noexcept
    : PlacedClip(id, std::move(material), startSampleOnTimeline, 0, static_cast<std::int64_t>(-1))
{
}

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline,
                      const std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept
    : PlacedClip(id, std::move(material), startSampleOnTimeline, 0, visibleLengthSamplesOnDiskOrDefault)
{
}

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline,
                      const std::int64_t leftTrimSamples,
                      const std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept
    : id_(id)
    , material_(std::move(material))
    , startSampleOnTimeline_(startSampleOnTimeline)
    , leftTrimSamples_(0)
    , visibleLengthSamples_(0)
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    const int matN = material_->getNumSamples();
    if (matN <= 0)
    {
        visibleLengthSamples_ = 0;
        leftTrimSamples_ = 0;
        return;
    }
    const std::int64_t m = static_cast<std::int64_t>(matN);
    const std::int64_t l = juce::jlimit(std::int64_t{0}, m - 1, leftTrimSamples);
    leftTrimSamples_ = l;
    const std::int64_t maxV = m - l;
    if (maxV <= 0)
    {
        visibleLengthSamples_ = 0;
        return;
    }
    if (visibleLengthSamplesOnDiskOrDefault <= 0)
    {
        visibleLengthSamples_ = maxV;
    }
    else
    {
        visibleLengthSamples_ = juce::jlimit(std::int64_t{1}, maxV, visibleLengthSamplesOnDiskOrDefault);
    }
}

PlacedClip PlacedClip::withStartSampleOnTimeline(const std::int64_t newStartSampleOnTimeline) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    return PlacedClip(id_, material_, newStartSampleOnTimeline, leftTrimSamples_, visibleLengthSamples_);
}

PlacedClip PlacedClip::withRightEdgeVisibleLength(const std::int64_t newVisibleLength) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    if (material_ == nullptr)
    {
        return *this;
    }
    const int matN = material_->getNumSamples();
    if (matN <= 0)
    {
        return *this;
    }
    const std::int64_t m = static_cast<std::int64_t>(matN);
    const std::int64_t cap = m - leftTrimSamples_;
    if (cap < 1)
    {
        return *this;
    }
    const std::int64_t clamped = juce::jlimit(std::int64_t{1}, cap, newVisibleLength);
    return PlacedClip(id_, material_, startSampleOnTimeline_, leftTrimSamples_, clamped);
}

PlacedClip PlacedClip::withLeftEdgeTrim(const std::int64_t newLeftTrimSamples) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    if (material_ == nullptr)
    {
        return *this;
    }
    const int matN = material_->getNumSamples();
    if (matN <= 0)
    {
        return *this;
    }
    const std::int64_t m = static_cast<std::int64_t>(matN);
    const std::int64_t l0 = leftTrimSamples_;
    const std::int64_t v0 = visibleLengthSamples_;
    if (v0 < 1)
    {
        return *this;
    }
    // d = newL - l0; S' = S + d; V' = v0 - d. Require L' in [0, m-1], V' >= 1, S' >= 0.
    const std::int64_t dMin = juce::jmax(-l0, -startSampleOnTimeline_);
    const std::int64_t dMax = v0 - 1;
    if (dMin > dMax)
    {
        return *this;
    }
    const std::int64_t newL = juce::jlimit(l0 + dMin, l0 + dMax, newLeftTrimSamples);
    const std::int64_t d = newL - l0;
    const std::int64_t newS = startSampleOnTimeline_ + d;
    const std::int64_t newV = v0 - d;
    jassert(newS >= 0 && newL >= 0 && newL < m && newV >= 1 && newL + newV <= m);
    return PlacedClip(id_, material_, newS, newL, newV);
}

const AudioClip& PlacedClip::getAudioClip() const noexcept
{
    jassert(material_ != nullptr);
    return *material_;
}

std::int64_t PlacedClip::getEffectiveLengthSamples() const noexcept
{
    return visibleLengthSamples_;
}

int PlacedClip::getMaterialLengthSamples() const noexcept
{
    jassert(material_ != nullptr);
    return material_->getNumSamples();
}
