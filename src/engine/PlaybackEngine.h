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
//   playhead to match samples actually delivered.
//
// OWNERSHIP AND LIFETIME
//   Does not own Transport or Session. The application (Main) constructs all three and
//   outlives them in this order: register callback → run → removeAudioCallback → destroy
//   PlaybackEngine → then Session/Transport, so references remain valid for the callback’s
//   whole lifetime. PlaybackEngine is unique_ptr-owned by the app.
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

#include <juce_audio_devices/juce_audio_devices.h>

class Session;
class Transport;

class PlaybackEngine : public juce::AudioIODeviceCallback
{
public:
    // Contract: retain non-owning references; Main must outlive the engine and unregister the
    // callback before destroy. Thread: Main / message thread.
    PlaybackEngine(Transport& transport, Session& session);
    ~PlaybackEngine() override;

    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    PlaybackEngine(PlaybackEngine&&) = delete;
    PlaybackEngine& operator=(PlaybackEngine&&) = delete;

    // [Audio thread] Realtime: fill `outputChannelData` with at most one clip pass; no decode,
    // I/O, locks, or UI. PlaybackEngine.cpp walks the flow and adds in-body plain-language notes
    // at branches (mono-to-stereo, end-of-clip, extra outputs) so product meaning is visible
    // beside the buffer logic.
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

private:
    Transport& transport_;
    Session& session_;
};
