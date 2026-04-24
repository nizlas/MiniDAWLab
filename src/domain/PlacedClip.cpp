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

PlacedClip::PlacedClip(std::shared_ptr<const AudioClip> material,
                      std::int64_t startSampleOnTimeline) noexcept
    : material_(std::move(material))
    , startSampleOnTimeline_(startSampleOnTimeline)
{
    // Failing this means someone tried to build a `PlacedClip` without real PCM — the snapshot
    // should not be published in that state (see `SessionSnapshot::withSinglePlacedClip`).
    jassert(material_ != nullptr);
}

const AudioClip& PlacedClip::getAudioClip() const noexcept
{
    jassert(material_ != nullptr);
    // Same const buffer the audio thread reads through the snapshot — no per-call load, no copy.
    return *material_;
}
