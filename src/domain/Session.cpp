// =============================================================================
// Session.cpp  —  release/acquire of `const SessionSnapshot` (same contract as old single-clip)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   This file is the **publish** side of the message-thread → audio-thread snapshot handoff. We
//   only call `std::atomic_store` / `std::atomic_load` on `std::shared_ptr<const SessionSnapshot>`
//   with the memory orders spelled out in comments — the mechanical pattern matches the former
//   `std::shared_ptr<const AudioClip>` in `Session`, but the pointed-to object now carries
//   `PlacedClip` rows instead of a bare clip, so Step 4+ is behavior-preserving with a Phase-2-
//   ready **shape** (see `SessionSnapshot` header for why that matters).
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

juce::Result Session::addClipFromFileAtPlayhead(const juce::File& file,
                                                const double deviceSampleRate,
                                                const std::int64_t startSampleOnTimeline)
{
    // Decode on the message thread. Until we have a *complete* new `AudioClip`, we do not
    // publish: a corrupt file must not become a half-finished new snapshot.
    std::unique_ptr<AudioClip> loaded;
    const juce::Result loadResult = AudioFileLoader::loadFromFile(file, deviceSampleRate, loaded);

    if (!loadResult.wasOk())
    {
        // Failure: return the error and leave `sessionSnapshot_` untouched (acquire in the
        // callback still sees the previous pointer).
        return loadResult;
    }

    const std::shared_ptr<const AudioClip> material(std::move(loaded));
    // `startSampleOnTimeline` was read on this thread (typically from `Transport` once, at add
    // time) — the clip is spliced in as the new front (newest) row; the prior snapshot, if any, is
    // copied in order behind it.
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    const std::shared_ptr<const SessionSnapshot> next =
        SessionSnapshot::withClipAddedAsNewest(*current, material, startSampleOnTimeline);
    jassert(next != nullptr);
    // Release: make this snapshot the one future acquires see; old snapshot is kept alive by any
    // in-flight callback/UI read until their shared_ptrs drop.
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
    // Bridge: return the **front** row’s material (index 0). The main waveform view uses the
    // full snapshot in `ClipWaveformView` (Step 7).
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->isEmpty())
    {
        return nullptr;
    }
    return &snap->getPlacedClip(0).getAudioClip();
}

std::int64_t Session::getTimelineLengthSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->isEmpty())
    {
        return 0;
    }
    return snap->getDerivedTimelineLengthSamples();
}

std::shared_ptr<const SessionSnapshot> Session::loadSessionSnapshotForAudioThread() const noexcept
{
    // Acquire: pair with the release stores in replace/clear so this read happens-after the last
    // full snapshot publish; the cost on the hot path is the atomic + shared_ptr retain.
    return std::atomic_load_explicit(&sessionSnapshot_, std::memory_order_acquire);
}
