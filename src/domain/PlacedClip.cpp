// =============================================================================
// PlacedClip.cpp  —  construction + trim clamps (material window for shared takes)
// =============================================================================

#include "domain/PlacedClip.h"

#include "domain/AudioClip.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <limits>

namespace
{
    constexpr std::int64_t kUnsetWindowEndExclusive = std::numeric_limits<std::int64_t>::min();
} // namespace

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline) noexcept
    : PlacedClip(
          id,
          std::move(material),
          startSampleOnTimeline,
          0,
          static_cast<std::int64_t>(-1),
          0,
          kUnsetWindowEndExclusive)
{
}

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline,
                      const std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept
    : PlacedClip(id,
                  std::move(material),
                  startSampleOnTimeline,
                  0,
                  visibleLengthSamplesOnDiskOrDefault,
                  0,
                  kUnsetWindowEndExclusive)
{
}

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline,
                      const std::int64_t leftTrimSamples,
                      const std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept
    : PlacedClip(id,
                  std::move(material),
                  startSampleOnTimeline,
                  leftTrimSamples,
                  visibleLengthSamplesOnDiskOrDefault,
                  0,
                  kUnsetWindowEndExclusive)
{
}

PlacedClip::PlacedClip(const PlacedClipId id,
                       std::shared_ptr<const AudioClip> material,
                       const std::int64_t startSampleOnTimeline,
                       const std::int64_t leftTrimSamplesRequested,
                       const std::int64_t visibleLengthSamplesOnDiskOrDefault,
                       const std::int64_t materialWindowStartSamples,
                       const std::int64_t materialWindowEndExclusiveSamples) noexcept
    : id_(id)
    , material_(std::move(material))
    , startSampleOnTimeline_(startSampleOnTimeline)
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);

    const int matN = material_->getNumSamples();
    if (matN <= 0)
    {
        visibleLengthSamples_ = 0;
        leftTrimSamples_ = 0;
        materialWindowStartSamples_ = 0;
        materialWindowEndExclusiveSamples_ = 0;
        return;
    }

    const std::int64_t m = static_cast<std::int64_t>(matN);
    std::int64_t ws = 0;
    std::int64_t we = m;
    if (materialWindowEndExclusiveSamples != kUnsetWindowEndExclusive)
    {
        ws = juce::jlimit(std::int64_t{ 0 }, m, materialWindowStartSamples);
        we = juce::jlimit(ws, m, materialWindowEndExclusiveSamples);
    }

    materialWindowStartSamples_ = ws;
    materialWindowEndExclusiveSamples_ = we;

    if (we <= ws)
    {
        visibleLengthSamples_ = 0;
        leftTrimSamples_ = ws;
        return;
    }

    const std::int64_t lMaxForAnyAudio = std::min(we - 1, m - 1);
    std::int64_t l = juce::jlimit(ws, lMaxForAnyAudio, leftTrimSamplesRequested);
    const std::int64_t maxFromWindow = we - l;
    const std::int64_t maxFromTail = m - l;
    const std::int64_t maxV = std::min(maxFromWindow, maxFromTail);
    jassert(maxV >= 0);

    if (maxV <= 0)
    {
        leftTrimSamples_ = l;
        visibleLengthSamples_ = 0;
        return;
    }

    if (visibleLengthSamplesOnDiskOrDefault <= 0)
    {
        visibleLengthSamples_ = maxV;
    }
    else
    {
        visibleLengthSamples_
            = juce::jlimit(std::int64_t{ 1 }, maxV, visibleLengthSamplesOnDiskOrDefault);
    }
    leftTrimSamples_ = l;
#if !defined(NDEBUG)
    if (visibleLengthSamples_ > 0 && matN > 0)
    {
        jassert(materialWindowStartSamples_ <= leftTrimSamples_);
        jassert(leftTrimSamples_ + visibleLengthSamples_ <= materialWindowEndExclusiveSamples_);
    }
#endif
}

PlacedClip PlacedClip::replicatedWith(const std::int64_t startSampleOnTimeline,
                                      const std::int64_t leftTrimSamples,
                                      const std::int64_t visibleLengthSamples) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    return PlacedClip(
        id_,
        material_,
        startSampleOnTimeline,
        leftTrimSamples,
        visibleLengthSamples,
        materialWindowStartSamples_,
        materialWindowEndExclusiveSamples_);
}

PlacedClip PlacedClip::withStartSampleOnTimeline(const std::int64_t newStartSampleOnTimeline) const noexcept
{
    return replicatedWith(newStartSampleOnTimeline, leftTrimSamples_, visibleLengthSamples_);
}

PlacedClip PlacedClip::withRightEdgeVisibleLength(const std::int64_t newVisibleLength) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    if (material_ == nullptr)
        return *this;

    const int matN = material_->getNumSamples();
    if (matN <= 0)
        return *this;

    const std::int64_t m = static_cast<std::int64_t>(matN);
    const std::int64_t we = materialWindowEndExclusiveSamples_;
    const std::int64_t capTail = m - leftTrimSamples_;
    const std::int64_t capWin = we - leftTrimSamples_;
    const std::int64_t cap = std::min(capTail, capWin);
    if (cap < 1)
        return *this;

    const std::int64_t clamped = juce::jlimit(std::int64_t{ 1 }, cap, newVisibleLength);
    return replicatedWith(startSampleOnTimeline_, leftTrimSamples_, clamped);
}

PlacedClip PlacedClip::withLeftEdgeTrim(const std::int64_t newLeftTrimSamples) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    if (material_ == nullptr)
        return *this;

    const int matN = material_->getNumSamples();
    if (matN <= 0)
        return *this;

    const std::int64_t m = static_cast<std::int64_t>(matN);
    const std::int64_t l0 = leftTrimSamples_;
    const std::int64_t v0 = visibleLengthSamples_;
    const std::int64_t winStart = materialWindowStartSamples_;
    const std::int64_t winEndEx = materialWindowEndExclusiveSamples_;
    jassert(winEndEx <= m);

    if (v0 < 1)
        return *this;

    const std::int64_t dMinTimeline = juce::jmax(-l0, -startSampleOnTimeline_);
    const std::int64_t dMinWin = winStart - l0;
    const std::int64_t dMin = juce::jmax(dMinTimeline, dMinWin);

    const std::int64_t dMax = v0 - 1;

    if (dMin > dMax)
        return *this;

    const std::int64_t newL = juce::jlimit(l0 + dMin, l0 + dMax, newLeftTrimSamples);
    const std::int64_t d = newL - l0;
    const std::int64_t newS = startSampleOnTimeline_ + d;
    const std::int64_t newV = v0 - d;

    jassert(newS >= 0 && newV >= 1);
    jassert(newL >= winStart && newL <= m - 1);
    jassert(newL + newV <= m);
    jassert(newL + newV <= winEndEx);

    return replicatedWith(newS, newL, newV);
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
