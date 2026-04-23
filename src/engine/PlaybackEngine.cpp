// =============================================================================
// PlaybackEngine.cpp  —  drive the speakers from Session + Transport (one file to read for audio)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   This is the only class registered with JUCE as the audio "callback". The OS / sound card
//   calls it repeatedly to ask: "here is an output buffer of N samples per channel; fill it."
//   We copy samples from the decoded clip in Session and move the playhead in Transport. We
//   do not open files, decode, draw the UI, or own clip memory — that is other layers' jobs.
//
// WHERE THIS FILE SITS
//   App (Main)  →  AudioDeviceManager.addAudioCallback(PlaybackEngine*)
//                     →  each block:  this file runs on the realtime audio thread
//   This file  →  Session::loadClipForAudioThread()  (read-only snapshot of the clip)
//   This file  →  Transport::audioThread_*         (seek apply, read playhead, advance playhead)
//
// WHAT PLAYBACKENGINE IS NOT ALLOWED TO DO (by project rules + realtime safety)
//   • Load or decode files (would block; belongs on the message thread).
//   • Touch GUI (wrong thread; would stall audio).
//   • Act as the source of truth for the playhead — Transport is; we only *advance* it to match
//     how many source samples we actually output this block.
//
// WHAT IT IS RESPONSIBLE FOR
//   • Converting the current Transport state (intent, playhead after seek) + current clip
//     into interleaved JUCE output buffers: silence, clip PCM, or a mix of copy+silence per rule
//     below. Advancing the playhead by the number of *source* samples consumed when Playing.
//
// THREADING
//   • audioDeviceIOCallbackWithContext  →  [Audio thread] only. Hot path. No locks, no allocs
//     of unbounded size; JUCE’s FloatVectorOperations is a fixed-size float buffer op.
//   • audioDeviceAboutToStart / Stopped  →  [Message thread] (JUCE calls them around stream life).
//   • Constructor / destructor  →  [Message thread] (owned by the app, not the callback).
//
// -----------------------------------------------------------------------------
// END-TO-END FLOW OF THE CALLBACK (see the callback body: plain-language notes at each phase)
// -----------------------------------------------------------------------------
//   Step 1 — Apply a pending user seek: Transport applies message-thread seek requests here at
//            block start so playhead and audio stay aligned.
//   Step 2 — Read clip snapshot, playback intent, and playhead. If no clip or not Playing, we
//            will only output silence, but we still do Step 1 for consistency.
//   Step 3 — Compute framesToPlay (see next section). This is how many *clip* sample frames
//            we will copy this block, before padding with silence in each channel.
//   Step 4 — For each hardware output channel: copy or clear. Mono special case: duplicate one
//            clip channel to left and right when the device is stereo.
//   Step 5 — Tell Transport to advance the playhead by framesToPlay when intent is Playing.
//
// -----------------------------------------------------------------------------
// WHY framesToPlay OFTEN DIFFERS FROM numSamples
//   numSamples is how many sample *frames* the device wants for *each* channel this block
//   (e.g. 256). That is the block size of the *device*, not the remaining length of the file.
//   The playhead is a position in the clip (0 .. clip length). If 80 samples of clip are left
//   until the end, we can only *play* 80 frames of audio from the buffer; we set
//   framesToPlay = min(numSamples, remaining). The playhead then advances by 80, not 256.
//   If we are not Playing, or there is no clip, framesToPlay stays 0.
//
// -----------------------------------------------------------------------------
// WHY WE CLEAR THE TAIL OF EACH OUTPUT CHANNEL (dest + framesToPlay .. + numSamples)
//   The device still expects a full numSamples frame count per channel. We only *wrote* audio
//   in the first framesToPlay positions (e.g. end of clip, or not playing). The rest of the
//   buffer is uninitialized garbage unless we fill it. So we zero the remainder, or the whole
//   buffer when we output no audio. FloatVectorOperations::clear does that in one go per span.
//
// -----------------------------------------------------------------------------
// MONO FILE → STEREO OUTPUT (Phase 1 product rule, not generic mixing)
//   Many mono files should be heard on both speakers. If the clip has 1 channel and the device
//   has at least 2 output channels, we copy the same mono channel to output 0 and 1. Other
//   output indices stay silent unless we add more rules later. If clip has more channels than
//   the device, extra clip channels are ignored; if device has more outputs than clip channels,
//   those outputs are silenced. (See the loop branches in Step 4.)
//
// JUCE TYPES used here (if you are new to JUCE)
//   • AudioIODeviceCallbackContext — per-block metadata; we ignore it in Phase 1 (output only).
//   • FloatVectorOperations::copy / clear — treat like memset/memcpy for float arrays, often
//     optimized. Not magic: we still size them explicitly (framesToPlay, numSamples).
//   • jmin — two-argument minimum, here to bound advance by (block size, remaining clip).
// =============================================================================

#include "engine/PlaybackEngine.h"

#include "domain/Session.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
// Product meaning: the sound card still expects a *full* block per channel every callback. We
// may have fewer *clip* samples to play (end of file or partial block) — we copy the real audio
// first, then zero the rest so the driver never receives uninitialized memory. Realtime: no heap;
// JUCE::FloatVectorOperations is a fixed-size bulk float op (see file header for JUCE note).
void copyClipRunThenSilenceTail(float* outputDest,
                                 const float* sourceAtOffset,
                                 int frameCount,
                                 int deviceBlockSize)
{
    juce::FloatVectorOperations::copy(outputDest, sourceAtOffset, frameCount);
    if (frameCount < deviceBlockSize)
    {
        juce::FloatVectorOperations::clear(outputDest + frameCount,
                                           deviceBlockSize - frameCount);
    }
}
} // namespace

PlaybackEngine::PlaybackEngine(Transport& transport, Session& session)
    : transport_(transport)
    , session_(session)
{
}

PlaybackEngine::~PlaybackEngine() = default;

// [Message thread] Invoked when the device is about to start streaming. Phase 1: nothing to
// cache (we could read sample rate here in a later phase). Kept as a no-op for wiring clarity.
void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    juce::ignoreUnused(device);
}

// [Message thread] Invoked when streaming stops. Phase 1: no-op.
void PlaybackEngine::audioDeviceStopped() {}

// [Audio thread] Called once per audio block by JUCE. Must not: decode, show UI, take locks, or
// allocate heap in a way that would violate realtime expectations. session_.loadClipForAudioThread()
// does an atomic load of a shared_ptr (refcount) — the agreed cross-thread handoff for the clip.
void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                      int numInputChannels,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

    // Seeks are queued on the message thread; we apply them at the *start* of the first block
    // that runs after that, so the playhead and the samples we read below stay in agreement.
    transport_.audioThread_beginBlock();

    const auto clip = session_.loadClipForAudioThread();
    const auto intent = transport_.audioThread_loadIntent();
    const std::int64_t playhead = transport_.audioThread_loadPlayhead();

    const int clipLengthInSamples = clip != nullptr ? clip->getNumSamples() : 0;

    // How many *source* frames we can emit this block, and (when Playing) how far the playhead
    // will move: bounded by the device block size and by how many samples are left in the clip.
    // If we are not Playing, or there is no clip, or the playhead is already past the end, this
    // stays zero and we only output silence.
    int framesToPlay = 0;
    if (clip != nullptr && intent == PlaybackIntent::Playing && playhead < clipLengthInSamples
        && numSamples > 0)
    {
        const std::int64_t remainingInClip =
            static_cast<std::int64_t>(clipLengthInSamples) - playhead;
        framesToPlay =
            static_cast<int>(juce::jmin(static_cast<std::int64_t>(numSamples), remainingInClip));
    }

    const int playheadOffsetInSamples = static_cast<int>(playhead);

    for (int outChannel = 0; outChannel < numOutputChannels; ++outChannel)
    {
        float* const dest = outputChannelData[outChannel];
        if (dest == nullptr)
            continue;

        if (clip == nullptr || framesToPlay <= 0)
        {
            // Nothing to play for this block (no clip, wrong transport mode, or already at end):
            // the product rule is a full block of digital silence, not a shorter buffer.
            juce::FloatVectorOperations::clear(dest, numSamples);
            continue;
        }

        const int numSourceChannels = clip->getNumChannels();
        const bool duplicateMonoToStereo = (numSourceChannels == 1 && numOutputChannels >= 2
                                            && (outChannel == 0 || outChannel == 1));

        if (duplicateMonoToStereo)
        {
            // Phase 1 mono-to-stereo product rule: the file decoded to one channel, but a
            // typical sound card has separate left and right outputs. We duplicate that single
            // channel to the first *two* device outputs so the user hears the clip in the
            // center, not panned to one side. This is an explicit upmix, not a general mixer;
            // other outputs on multi-output devices are still silenced in the branch below.
            const float* const mono = clip->getAudio().getReadPointer(0);
            copyClipRunThenSilenceTail(dest, mono + playheadOffsetInSamples, framesToPlay,
                                       numSamples);
        }
        else if (outChannel < numSourceChannels)
        {
            // One clip channel maps one-to-one to a device output (stereo or multichannel file).
            const float* const channelSource = clip->getAudio().getReadPointer(outChannel);
            copyClipRunThenSilenceTail(dest, channelSource + playheadOffsetInSamples, framesToPlay,
                                       numSamples);
        }
        else
        {
            // The device has more output channels than this clip: Phase 1 does not downmix,
            // duplicate, or route the clip to “extra” speakers — they stay silent.
            juce::FloatVectorOperations::clear(dest, numSamples);
        }
    }

    // Move the timeline playhead by exactly how many *source* samples we just played, but only
    // while the user has left transport in Playing (Paused/Stopped: leave the playhead where it is).
    transport_.audioThread_advancePlayheadIfPlaying(static_cast<std::int64_t>(framesToPlay));
}
