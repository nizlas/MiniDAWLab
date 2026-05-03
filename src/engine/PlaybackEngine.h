#pragma once

// =============================================================================
// PlaybackEngine.h / PlaybackEngine.cpp — JUCE audio I/O callback: device ← Session + Transport
// =============================================================================
//
// File role: declare the one object registered with juce::AudioDeviceManager to receive block
// callbacks. Implementation narrates the fill loop, mono→stereo rule, and playhead advance in
// PlaybackEngine.cpp.
//
// CLASS RESPONSIBILITY
//   Implements juce::AudioIODeviceCallback. The audio device invokes this object on a high-
//   priority thread to fill output buffers. This class is the *only* bridge from our domain
//   (which sample to play) to the hardware (float arrays per channel). It advances Transport’s
//   playhead (timeline-absolute) to match *timeline* samples advanced while Playing, including
//   silence in gaps. Phase 3: per-track coverage (Phase 2 rule in each lane) plus **sum** across lanes.
//
// OWNERSHIP AND LIFETIME
//   Does not own Transport, Session, or `RecorderService`. The application (Main) owns all of
//   them and outlives the engine. Tear order: removeAudioCallback → destroy `PlaybackEngine` →
//   destroy `RecorderService` so the non-owning `RecorderService*` is never used after the engine
//   dies. The optional recorder pointer is valid for the whole callback lifetime when non-null.
//
// DELIBERATELY NOT RESPONSIBLE FOR
//   File I/O, decoding, waveform UI, or deciding user intent (Play/Pause) beyond *reading* it
//   from Transport. Does not set seek targets — user code queues seeks on Transport; we only
//   run audioThread_beginBlock to apply them at block boundaries.
//
// THREADING (which methods on which thread)
//   See comments on each public override: two are message-thread (device lifecycle) and one is
//   audio-thread (per block). The callback’s body is the central realtime path: it holds to the
//   body-readability tier (in-body plain-language at branches) so JUCE buffer layout and channel
//   indexing are not the only place “what the user hears” is defined.
//
// JUCE: AudioIODeviceCallback is the interface the audio device uses; see .cpp for the
//      implementation body and a plain-language walkthrough of the buffer fill.
//
// Optional `RecorderService` (Phase 4): non-owning pointer for **input** `pushInputBlock` from the
// audio thread only. Does **not** own the recorder, does not call `Transport` / `Session`. May be
// null if recording is not composed in.

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>

class CountInClickOutput;
class PluginInsertHost;
class RecorderService;
class Session;
class Transport;

class PlaybackEngine : public juce::AudioIODeviceCallback
{
public:
    // Contract: retain non-owning references; Main must outlive the engine and unregister the
    // callback before destroy. Thread: Main / message thread.
    // `recorder` may be null; if non-null, it must outlive this engine (destroy engine before recorder).
    // `countIn` is optional: short count-in metronome clicks to device outputs only (no session/recorder).
    // `pluginHost` optional Phase 8: per-track VST3 insert; must outlive this engine until after
    // `removeAudioCallback` (same tear order as `recorder`).
    PlaybackEngine(Transport& transport, Session& session, RecorderService* recorder = nullptr,
                  CountInClickOutput* countIn = nullptr, PluginInsertHost* pluginHost = nullptr);
    ~PlaybackEngine() override;

    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    PlaybackEngine(PlaybackEngine&&) = delete;
    PlaybackEngine& operator=(PlaybackEngine&&) = delete;

    // [Audio thread] Realtime: fill `outputChannelData` using **per-track** coverage (front-most
    // `PlacedClip` in each lane that covers each timeline position; gaps = silence **in that lane**).
    // **Across** tracks, samples are **added** into the same output (minimal sum, not a mixer).
    // Optional: forward mono **input[0]** to `RecorderService::pushInputBlock` when a recorder is
    // composed in (independent of `Session`; no-op if not recording or no input channels).
    // No decode, I/O, locks, or UI; no new heap use on the hot path beyond the snapshot pointer copy.
    // See .cpp for coverage runs, mono→stereo, and transport advance.
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    // [Message thread] JUCE: stream starting; reserved for a later phase (e.g. sample rate).
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    // [Message thread] JUCE: stream ended; nothing to release in Phase 1.
    void audioDeviceStopped() override;

    // [Message thread] Audible read position shifts by adding this to the transport timeline sample each block.
    // Positive reads later material; negative reads earlier. Wrap decisions still use unshifted playhead.
    void setPlaybackOffsetSamples(std::int64_t samples) noexcept;

private:
    Transport& transport_;
    Session& session_;
    RecorderService* const recorder_;
    CountInClickOutput* const countIn_;
    PluginInsertHost* const pluginHost_;

    std::atomic<std::int64_t> playbackOffsetSamples_{ 0 };
};
