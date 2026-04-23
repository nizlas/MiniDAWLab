#pragma once

// =============================================================================
// Session — which clip is loaded for playback (domain); no transport, no UI
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Holds “the current clip” as an immutable AudioClip in memory, or nothing. PlaybackEngine
//   reads it on the audio thread; the UI reads it on the message thread for the waveform. File
//   loading and decode are *not* here — AudioFileLoader produces an AudioClip; this class only
//   swaps in the result atomically.
//
// THREAD MODEL
//   • replaceClipFromFile / clearClip: [Message thread] only in Phase 1. They may block (decode).
//   • loadClipForAudioThread: [Audio thread] safe — atomic load of std::shared_ptr<const AudioClip>
//     (acquire), refcount bump, no decode. Also legal from the message thread for UI snapshots.
//   • getCurrentClip: convenience for UI; not a different truth than loadClipForAudioThread.
//
// OWNERSHIP
//   Session owns the atomic<shared_ptr<const AudioClip>>. It does not own Transport. Lifetimes:
//   application creates Session and keeps it alive for the app lifetime; clips are replaced, not
//   owned from outside.
//
// NOT RESPONSIBLE FOR
//   Playhead position, play/pause, or opening files (UI does open; loader decodes; we store).
//
// In-body comments in Session.cpp state product meaning at the atomic publish path (failed load
// vs successful swap, what “clear” means for playback) next to the mechanics.
//
// See also: AudioClip, AudioFileLoader, PlaybackEngine (reader on audio thread).
// =============================================================================

#include "domain/AudioClip.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

class Session
{
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // [Message thread] Decode and replace the current clip, or return error and keep old clip.
    juce::Result replaceClipFromFile(const juce::File& file, double deviceSampleRate);

    // [Message thread] Remove the current clip (release store to empty).
    void clearClip() noexcept;

    // [Message thread] Raw pointer to current clip; same data as a snapshot from load, shorter
    // lifetime — do not use across threads for logic that needs atomicity; for UI it matches
    // what you will paint after a message-thread load.
    [[nodiscard]] const AudioClip* getCurrentClip() const noexcept;

    // [Audio thread] and [Message thread] Lock-free share of the current clip; refcount only on
    // the hot path. No decode.
    [[nodiscard]] std::shared_ptr<const AudioClip> loadClipForAudioThread() const noexcept;

private:
    mutable std::atomic<std::shared_ptr<const AudioClip>> clip_;
};
