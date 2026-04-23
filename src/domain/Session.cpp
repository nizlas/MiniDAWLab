// =============================================================================
// Session.cpp  —  release/acquire of `const SessionSnapshot` (same contract as old single-clip)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   This file is the **publish** side of the message-thread → audio-thread snapshot handoff. We
//   only call `std::atomic_store` / `std::atomic_load` on `std::shared_ptr<const SessionSnapshot>`
//   with the memory orders spelled out in comments — the mechanical pattern matches the former
//   `std::shared_ptr<const AudioClip>` in `Session`, but the pointed-to object now carries
//   `PlacedClip` rows instead of a bare clip, so Step 4 is behavior-preserving with a
//   Phase-2-ready **shape** (see `SessionSnapshot` header for why that matters).
//
// IN-BODY COMMENTS
//   Where we touch `sessionSnapshot_`, the comments name **user-visible meaning** (keep old
//   file on error, what clear implies) in plain language, not a narration of the C++ calls.
// =============================================================================

#include "domain/Session.h"

#include "domain/AudioClip.h"
#include "io/AudioFileLoader.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <utility>

Session::Session()
    : sessionSnapshot_(SessionSnapshot::createEmpty())
{
    // Initial world: the shared empty snapshot (see `createEmpty` — no new allocation of an empty
    // `vector` here beyond what that helper does once at first use).
}

Session::~Session() = default;

juce::Result Session::replaceClipFromFile(const juce::File& file, const double deviceSampleRate)
{
    // Decode on the message thread. Until we have a *complete* new `AudioClip`, we do not
    // publish: a corrupt file must not half-replace a good session — the last known-good snapshot
    // stays what the user hears and what the UI draws.
    std::unique_ptr<AudioClip> loaded;
    const juce::Result loadResult = AudioFileLoader::loadFromFile(file, deviceSampleRate, loaded);

    if (!loadResult.wasOk())
    {
        // Failure: return the error and leave `sessionSnapshot_` untouched (acquire in the
        // callback still sees the previous pointer).
        return loadResult;
    }

    const std::shared_ptr<const AudioClip> material(std::move(loaded));
    // Success: one `PlacedClip` at session start 0 — clip-relative playhead 0..length-1 in
    // `Transport` still lines up; we have only re-wrapped the same idea in a snapshot object.
    const std::shared_ptr<const SessionSnapshot> next =
        SessionSnapshot::withSinglePlacedClip(material, 0);
    jassert(next != nullptr);
    // Release: make this snapshot the one future acquires see; prior snapshot remains alive until
    // the last audio/UI reader drops it (refcount).
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return juce::Result::ok();
}

void Session::clearClip() noexcept
{
    // User-facing “no file”: publish the *shared* empty snapshot — same as having zero clips, no
    // extra allocation per clear beyond storing the well-known empty pointer.
    std::atomic_store_explicit(
        &sessionSnapshot_, SessionSnapshot::createEmpty(), std::memory_order_release);
}

const AudioClip* Session::getCurrentClip() const noexcept
{
    // Walk the same pointer the engine uses, but only expose the one `AudioClip` the old
    // single-waveform UI is built for. If the snapshot is empty or is not a single clip, there is
    // nothing to show.
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->isEmpty() || snap->getNumPlacedClips() != 1)
    {
        return nullptr;
    }
    return &snap->getPlacedClip(0).getAudioClip();
}

std::shared_ptr<const SessionSnapshot> Session::loadSessionSnapshotForAudioThread() const noexcept
{
    // Acquire: pair with the release stores in replace/clear so this read happens-after the last
    // full snapshot publish; the cost on the hot path is the atomic + shared_ptr retain.
    return std::atomic_load_explicit(&sessionSnapshot_, std::memory_order_acquire);
}
