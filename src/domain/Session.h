#pragma once

#include "domain/AudioClip.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

// Owns session/timeline state. Phase 1: at most one loaded clip, replaced synchronously from the message thread.
// Audio thread sees the clip via atomic shared_ptr (no I/O, no Session mutex in the callback).
class Session
{
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Decodes via AudioFileLoader on the calling thread; on failure the previous clip (if any) is kept.
    juce::Result replaceClipFromFile(const juce::File& file, double deviceSampleRate);

    void clearClip() noexcept;

    [[nodiscard]] const AudioClip* getCurrentClip() const noexcept;

    // Realtime-safe snapshot for the audio callback (refcount bump only, no decode).
    [[nodiscard]] std::shared_ptr<const AudioClip> loadClipForAudioThread() const noexcept;

private:
    mutable std::atomic<std::shared_ptr<const AudioClip>> clip_;
};
