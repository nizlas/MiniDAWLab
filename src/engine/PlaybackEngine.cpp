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
//   This file  →  Session::loadSessionSnapshotForAudioThread()  (read-only session snapshot)
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

#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
// Copy `frameCount` consecutive clip samples into the start of a device output buffer, then
// make the *rest* of that buffer well-defined. JUCE’s contract: each `outputChannelData` row has
// length `deviceBlockSize` floats; the host always consumes the full width. We only *have* real
// audio for the first `frameCount` of those (e.g. stop mid-block at end of file) — the tail
// would otherwise be whatever memory was in the array, so we zero it. Realtime: no heap;
// `FloatVectorOperations` is a fixed-size bulk op (JUCE-usage: same idea as `memcpy`+`memset` on
// the float spans).
void copyClipRunThenSilenceTail(float* outputDest,
                                 const float* firstSampleToPlay,
                                 int frameCount,
                                 int deviceBlockSize)
{
    juce::FloatVectorOperations::copy(outputDest, firstSampleToPlay, frameCount);
    if (frameCount < deviceBlockSize)
    {
        juce::FloatVectorOperations::clear(
            outputDest + frameCount,
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
// allocate in a way that would violate realtime expectations. The session snapshot is an
// acquire-load of a `shared_ptr<const SessionSnapshot>` (refcount) — the agreed cross-thread
// handoff. Step 4 still uses one PlacedClip at start 0 and the same clip-relative playhead range
// as Phase 1.
void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                      int numInputChannels,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

    // Same value as JUCE’s `numSamples` for this callback; name stresses “how many float *frames* per
    // output row” as opposed to “how many clip samples we can still read” (`framesToPlay` below).
    const int deviceBlockSizeInFrames = numSamples;

    // --- 1) Seek contract (Transport) — UI queues seeks on the message thread; we make them
    //     visible here *before* we read the playhead or read PCM, so the buffer copy and the
    //     stored “where we are in the file” line up for this block. ---
    transport_.audioThread_beginBlock();

    // --- 2) What audio material is in play? (Session) — still one decoded buffer for Step 4;
    //     later phases use the snapshot’s list for coverage and timeline-absolute time. ---
    const std::shared_ptr<const SessionSnapshot> sessionSnap =
        session_.loadSessionSnapshotForAudioThread();
    const AudioClip* clip = nullptr;
    if (sessionSnap != nullptr && !sessionSnap->isEmpty() && sessionSnap->getNumPlacedClips() == 1)
    {
        // Only case we drive speakers for: a single clip placed at the session start, which for
        // now is the same as “the file” the UI opened (multi-clip rules come in a later step).
        const PlacedClip& placed = sessionSnap->getPlacedClip(0);
        jassert(placed.getStartSample() == 0);
        clip = &placed.getAudioClip();
    }

    // --- 3) Transport intent and read position. The playhead is still an index *into the clip
    //     buffer* (0 = first sample of the file); the waveform and click-to-seek use the same
    //     index space, so the user’s line and what we play match. ---
    const PlaybackIntent playbackIntent = transport_.audioThread_loadIntent();
    const std::int64_t playhead = transport_.audioThread_loadPlayhead();
    const int clipLengthInSamples = clip != nullptr ? clip->getNumSamples() : 0;
    // Where `copy` starts in each channel of `clip` for this block: the playhead, possibly mid-file.
    const int readOffsetInClip = static_cast<int>(playhead);

    // --- 4) How many *clip* frames to output this *device* block? Often the full block, but at
    //     end-of-file the remainder can be *smaller* than `deviceBlockSizeInFrames` — that is
    //     the main reason `framesToPlay` and the device block size differ (not a JUCE quirk;
    //     product: stop playback when the file runs out, not after padded garbage). We only
    //     count frames toward playhead advance when we are actually Playing and have material. ---
    int framesToPlay = 0;
    if (clip != nullptr && playbackIntent == PlaybackIntent::Playing
        && playhead < static_cast<std::int64_t>(clipLengthInSamples) && deviceBlockSizeInFrames > 0)
    {
        const std::int64_t remainingInClip =
            static_cast<std::int64_t>(clipLengthInSamples) - playhead;
        framesToPlay = static_cast<int>(
            juce::jmin(static_cast<std::int64_t>(deviceBlockSizeInFrames), remainingInClip));
    }

    // --- 5) One device output at a time: each `outChannel` is one speaker row in JUCE’s
    //     `outputChannelData` (float array of length `numSamples` per row). Filling rules *differ
    //     by channel* when the file is mono or when the device has more speakers than the file. ---
    for (int outChannel = 0; outChannel < numOutputChannels; ++outChannel)
    {
        float* const deviceOutputBuffer = outputChannelData[outChannel];
        if (deviceOutputBuffer == nullptr)
        {
            // JUCE: an unused output slot can be null; nothing to fill.
            continue;
        }

        if (clip == nullptr || framesToPlay <= 0)
        {
            // No audio this block: either no file, user not Playing, or read cursor at/past the
            // end. The driver still expects a full `deviceBlockSizeInFrames` buffer — digital silence.
            juce::FloatVectorOperations::clear(deviceOutputBuffer, numSamples);
            continue;
        }

        const int numSourceChannels = clip->getNumChannels();
        const bool duplicateMonoToStereo = (numSourceChannels == 1 && numOutputChannels >= 2
                                            && (outChannel == 0 || outChannel == 1));

        if (duplicateMonoToStereo)
        {
            // Phase 1 *product* rule (not a general mixer): a single-channel file should be
            // *audible on both* left and right on a normal stereo output. The hardware gives
            // separate buffers for “speaker 0” and “speaker 1”; if we only wrote one, the user
            // would hear the file panned to one side. We therefore feed the *same* mono source
            // channel to those first two rows; remaining outputs of an oversized device still get
            // the “no routing” path below.
            const float* const monoSource = clip->getAudio().getReadPointer(0);
            copyClipRunThenSilenceTail(
                deviceOutputBuffer,
                monoSource + readOffsetInClip,
                framesToPlay,
                numSamples);
        }
        else if (outChannel < numSourceChannels)
        {
            // Multichannel (or stereo) file with enough clip channels: JUCE’s buffer stores each
            // *clip* channel in its own float array; take the one for this channel index and map it
            // to the device output of the *same* index — a straight path (file ch 0 → out 0, etc.)
            // up to the number of file channels. `readOffsetInClip` is the offset into *that* row.
            const float* const clipChannelPcm = clip->getAudio().getReadPointer(outChannel);
            copyClipRunThenSilenceTail(
                deviceOutputBuffer,
                clipChannelPcm + readOffsetInClip,
                framesToPlay,
                numSamples);
        }
        else
        {
            // More physical outputs than the file has channels (e.g. 7.1 device, stereo file):
            // Phase 1 does *not* invent a surround downmix or duplicate into extra speakers. Those
            // outputs are explicitly silent so behavior stays predictable and we do not imply a
            // routing graph we have not designed yet.
            juce::FloatVectorOperations::clear(deviceOutputBuffer, numSamples);
        }
    }

    // --- 6) Transport advance: the playhead only moves by `framesToPlay` when the user is
    //     Playing; Paused/Stopped or zero frames leaves the read cursor where Transport says. ---
    transport_.audioThread_advancePlayheadIfPlaying(static_cast<std::int64_t>(framesToPlay));
}
