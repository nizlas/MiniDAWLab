// =============================================================================
// Session.cpp  —  swap clip atomically; see Session.h for the threading story
// =============================================================================
//
// IMPLEMENTATION NOTE
//   std::atomic_store / atomic_load on shared_ptr (C++20-style free functions) with release /
//   acquire is how we publish a fully constructed AudioClip to the audio thread without a mutex.
//   The old clip lives until the last shared_ptr to it is dropped. Product: the UI and the audio
//   callback each see a single coherent clip pointer; a failed file load does not “half replace” the
//   session (we only store after a successful decode).
// =============================================================================

#include "domain/Session.h"

#include "io/AudioFileLoader.h"

Session::Session()
    : clip_(std::shared_ptr<const AudioClip>{})
{
}

Session::~Session() = default;

juce::Result Session::replaceClipFromFile(const juce::File& file, double deviceSampleRate)
{
    // Decode off-thread (message thread) into a *temporary* owner. We only touch `clip_` if the
    // file fully loads — a bad file leaves the user’s *previous* clip in place, so we never
    // publish a half-built buffer.
    std::unique_ptr<AudioClip> loaded;
    const juce::Result loadResult = AudioFileLoader::loadFromFile(file, deviceSampleRate, loaded);

    if (!loadResult.wasOk())
    {
        // Failure is non-destructive: the old clip, if any, is still the active session for playback.
        return loadResult;
    }

    std::shared_ptr<const AudioClip> shared(std::move(loaded));
    // Release: audio thread and UI acquire-load this pointer and then read only the clip’s
    // const data — a full hand-off of the new blob with no further locking.
    std::atomic_store_explicit(&clip_, shared, std::memory_order_release);
    return juce::Result::ok();
}

void Session::clearClip() noexcept
{
    // Product: there is *nothing* to play or show as a clip until the user loads another file.
    std::atomic_store_explicit(&clip_, {}, std::memory_order_release);
}

const AudioClip* Session::getCurrentClip() const noexcept
{
    // Same clip snapshot the engine would use, but for UI code that only needs a raw pointer in
    // a narrow scope. Still goes through the atomic for one coherent view of “current”.
    return loadClipForAudioThread().get();
}

// [Audio thread] and [Message thread] — see header
std::shared_ptr<const AudioClip> Session::loadClipForAudioThread() const noexcept
{
    // Acquire: see the latest successful replace/clear on the message thread; refcount bump is
    // the only per-block cost for sharing the const clip in the callback.
    return std::atomic_load_explicit(&clip_, std::memory_order_acquire);
}
