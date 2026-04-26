// =============================================================================
// PlacedClip.cpp  —  construction of a single placement (message-thread / snapshot build path)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   `PlacedClip` is built only when `Session` (or future session editors) assembles a new
//   `SessionSnapshot` on the **message** thread. The audio thread does not call this constructor;
//   it only reads `const PlacedClip` data through an already-published snapshot pointer.
//
// IN-BODY STORY
//   The constructor and accessor guard against null `shared_ptr` in debug (jassert). Product path
//   always supplies material from a successful decode; a null pointer would mean a bug in the
//   publisher, not a recoverable user error here.
//
// PEDAGOGICAL NOTE
//   This file is intentionally **small**: all “what time is this clip on?” meaning lives in
//   `startSampleOnTimeline` once; the heavy work is snapshot assembly (`SessionSnapshot`) and
//   reading (`PlaybackEngine`, `ClipWaveformView`). Reading this .cpp should confirm object **shape**,
//   not re-derive timeline rules.
// =============================================================================

#include "domain/PlacedClip.h"

#include "domain/AudioClip.h"

#include <juce_core/juce_core.h>

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline) noexcept
    : PlacedClip(id, std::move(material), startSampleOnTimeline, static_cast<std::int64_t>(-1))
{
}

PlacedClip::PlacedClip(const PlacedClipId id,
                      std::shared_ptr<const AudioClip> material,
                      const std::int64_t startSampleOnTimeline,
                      const std::int64_t visibleLengthSamplesOnDiskOrDefault) noexcept
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
        return;
    }
    const std::int64_t cap = static_cast<std::int64_t>(matN);
    if (visibleLengthSamplesOnDiskOrDefault <= 0)
    {
        visibleLengthSamples_ = cap;
    }
    else
    {
        visibleLengthSamples_
            = juce::jlimit(std::int64_t{1}, cap, visibleLengthSamplesOnDiskOrDefault);
    }
}

PlacedClip PlacedClip::withStartSampleOnTimeline(const std::int64_t newStartSampleOnTimeline) const noexcept
{
    jassert(material_ != nullptr);
    jassert(id_ != kInvalidPlacedClipId);
    return PlacedClip(id_, material_, newStartSampleOnTimeline, visibleLengthSamples_);
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
    const std::int64_t cap = static_cast<std::int64_t>(matN);
    const std::int64_t clamped = juce::jlimit(std::int64_t{1}, cap, newVisibleLength);
    return PlacedClip(id_, material_, startSampleOnTimeline_, clamped);
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
