// =============================================================================
// PlaybackEngine.cpp  —  drive the speakers from Session + Transport (one file to read for audio)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   JUCE’s audio callback. We fill the device’s float buffers from `PlacedClip` data via the
//   immutable `SessionSnapshot` and advance `Transport`’s playhead. No file decode, no UI.
//
// PHASE 3 (minimal multi-track): **Within each track**, Phase 2 still applies: overlapping clips
//   are ordered; the **smallest** index in that **lane** that covers a timeline instant wins for
//   *that* lane. **Across** tracks, the audible samples for each lane for the same time window are
//   **added** into the device buffer — a minimal sum (no mixer UI). Each track contributes after
//   multiplying by its `Track::channelFaderGain` (mixer channel volume at the fader point; not
//   clip/pre-gain — see `Track`). Future post-fader taps or inserts may need per-track staging buffers;
//   not implemented — gain is applied at track-output merge into the device buffer today.
//
// WHERE THIS SITS
//   `Session` publish → acquire-load of `const SessionSnapshot` (refcount) here; `Transport` seek
//   apply, playhead read/advance. See ARCHITECTURE_PRINCIPLES (Phase 2/3, snapshot handoff).
//
// REALTIME
//   [Audio thread] only in the callback. No allocation on this path beyond the existing `shared_ptr`
//   acquire; all scratch decisions use stack and fixed loops over clip and track count.
// =============================================================================

#include "engine/PlaybackEngine.h"

#include "engine/CountInClickOutput.h"
#include "engine/RecorderService.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    [[nodiscard]] int findCoveringRowIndexInLane(
        const std::vector<PlacedClip>& lane, const std::int64_t t) noexcept
    {
        for (int i = 0; i < (int)lane.size(); ++i)
        {
            const PlacedClip& p = lane[(size_t)i];
            const std::int64_t s = p.getStartSample();
            const std::int64_t e = s + p.getEffectiveLengthSamples();
            if (t >= s && t < e)
            {
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] std::int64_t minBoundaryStrictlyAfterInLane(
        const std::vector<PlacedClip>& lane,
        const std::int64_t t,
        const std::int64_t timelineEnd) noexcept
    {
        std::int64_t m = std::numeric_limits<std::int64_t>::max();
        for (int i = 0; i < (int)lane.size(); ++i)
        {
            const PlacedClip& p = lane[(size_t)i];
            const std::int64_t s = p.getStartSample();
            const std::int64_t e = s + p.getEffectiveLengthSamples();
            if (s > t)
            {
                m = juce::jmin(m, s);
            }
            if (e > t)
            {
                m = juce::jmin(m, e);
            }
        }
        if (timelineEnd > t)
        {
            m = juce::jmin(m, timelineEnd);
        }
        if (m == std::numeric_limits<std::int64_t>::max())
        {
            m = timelineEnd;
        }
        return m;
    }

    void addClipRunToOutputs(const AudioClip& clip,
                             int offInMaterial,
                             int run,
                             int outFrame0,
                             int numOutChannels,
                             float* const* outputChannelData,
                             float trackGain) noexcept
    {
        const int numSourceChannels = clip.getNumChannels();
        const juce::AudioBuffer<float>& buf = clip.getAudio();

        for (int outChannel = 0; outChannel < numOutChannels; ++outChannel)
        {
            float* d = outputChannelData[outChannel];
            if (d == nullptr)
            {
                continue;
            }
            float* const dest = d + outFrame0;
            const bool duplicateMono = (numSourceChannels == 1 && numOutChannels >= 2
                                        && (outChannel == 0 || outChannel == 1));
            if (duplicateMono)
            {
                juce::FloatVectorOperations::addWithMultiply(
                    dest, buf.getReadPointer(0) + offInMaterial, trackGain, run);
            }
            else if (outChannel < numSourceChannels)
            {
                juce::FloatVectorOperations::addWithMultiply(
                    dest, buf.getReadPointer(outChannel) + offInMaterial, trackGain, run);
            }
        }
    }
} // namespace

PlaybackEngine::PlaybackEngine(Transport& transport, Session& session, RecorderService* recorder,
                                 CountInClickOutput* countIn)
    : transport_(transport)
    , session_(session)
    , recorder_(recorder)
    , countIn_(countIn)
{
}

PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    juce::ignoreUnused(device);
}

void PlaybackEngine::audioDeviceStopped() {}

void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context);

    // [Audio thread] Phase 4: route mono input[0] to the recorder SPSC path only while `isRecording()`
    // and valid input pointers; does not access Session. `pushInputBlock` still no-ops if not recording
    // — this call site avoids touching the recorder SPSC at all when idle.
    if (recorder_ != nullptr
        && recorder_->isRecording()
        && numInputChannels > 0
        && numSamples > 0
        && inputChannelData != nullptr
        && inputChannelData[0] != nullptr)
    {
        recorder_->pushInputBlock(inputChannelData[0], numSamples);
    }

    const int deviceBlockSizeInFrames = numSamples;
    transport_.audioThread_beginBlock();

    const std::shared_ptr<const SessionSnapshot> sessionSnap = session_.loadSessionSnapshotForAudioThread();
    const PlaybackIntent playbackIntent = transport_.audioThread_loadIntent();
    const std::int64_t t0 = transport_.audioThread_loadPlayhead();

    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        if (float* row = outputChannelData[ch])
        {
            juce::FloatVectorOperations::clear(row, numSamples);
        }
    }
    if (countIn_ != nullptr)
    {
        countIn_->audioThread_mixInto(outputChannelData, numOutputChannels, numSamples);
    }

    if (sessionSnap == nullptr || deviceBlockSizeInFrames <= 0
        || playbackIntent != PlaybackIntent::Playing)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    const std::int64_t timelineEnd = sessionSnap->getArrangementExtentSamples();
    if (timelineEnd <= 0 || t0 >= timelineEnd)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    const std::int64_t canDo = juce::jmin(
        static_cast<std::int64_t>(deviceBlockSizeInFrames), timelineEnd - t0);
    const int R = static_cast<int>(canDo);
    if (R <= 0)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    for (int ti = 0; ti < sessionSnap->getNumTracks(); ++ti)
    {
        const Track& tr = sessionSnap->getTrack(ti);
        if (recorder_ != nullptr && recorder_->isRecording() && tr.getId() == recorder_->getRecordingTrackId())
        {
            // Transient: do not play existing clips on the track being recorded; other tracks mix as usual.
            continue;
        }
        const float trackGain = tr.getChannelFaderGain();
        if (trackGain <= 0.0f)
        {
            continue;
        }
        const std::vector<PlacedClip>& lane = tr.getPlacedClips();
        std::int64_t t = t0;
        int out0 = 0;
        while (out0 < R)
        {
            const int row = findCoveringRowIndexInLane(lane, t);
            const std::int64_t nextB = minBoundaryStrictlyAfterInLane(lane, t, timelineEnd);
            jassert(nextB > t);
            int run = static_cast<int>(juce::jmin(
                static_cast<std::int64_t>(R - out0), nextB - t));
            jassert(run > 0);

            if (row >= 0)
            {
                const PlacedClip& p = lane[(size_t)row];
                const AudioClip& c = p.getAudioClip();
                const std::int64_t rel = t - p.getStartSample();
                jassert(rel >= 0);
                jassert(rel + static_cast<std::int64_t>(run) <= p.getEffectiveLengthSamples());
                const int off = static_cast<int>(rel + p.getLeftTrimSamples());
                jassert(off >= 0);
                jassert(off + run <= c.getNumSamples());
                addClipRunToOutputs(c, off, run, out0, numOutputChannels, outputChannelData, trackGain);
            }
            t += run;
            out0 += run;
        }
        jassert(out0 == R);
        jassert(t - t0 == static_cast<std::int64_t>(R));
    }

    transport_.audioThread_advancePlayheadIfPlaying(static_cast<std::int64_t>(R));
}
