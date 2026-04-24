// =============================================================================
// PlaybackEngine.cpp  —  drive the speakers from Session + Transport (one file to read for audio)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   JUCE’s audio callback. We fill the device’s float buffers from `PlacedClip` data via the
//   immutable `SessionSnapshot` and advance `Transport`’s playhead. No file decode, no UI.
//
// PHASE 2 COVERAGE (this file’s product story)
//   The session keeps **overlapping** events on a single timeline, ordered **front to back** in
//   the snapshot (index 0 = front / newest). At each **timeline** instant, the **first** row that
//   covers that instant is what you hear: **not** a mix of all overlapping material. A clip behind
//   the front is audible only in **gaps** where no row in front of it covers. Gaps on the line
//   between clips (no row covers) are **silence**; the playhead still advances in real time, up to
//   the **derived** session end (then clamp, same as Phase 1 end intent).
//
// WHERE THIS SITS
//   `Session` publish → acquire-load of `const SessionSnapshot` (refcount) here; `Transport` seek
//   apply, playhead read/advance. See ARCHITECTURE_PRINCIPLES (Phase 2, snapshot handoff).
//
// REALTIME
//   [Audio thread] only in the callback. No allocation on this path beyond the existing `shared_ptr`
//   acquire; all scratch decisions use stack and fixed loops over clip count.
// =============================================================================

#include "engine/PlaybackEngine.h"

#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <limits>

namespace
{
    // The **front-most** (smallest) snapshot index that covers timeline sample `t`, or -1 (gap
    // or before/after all material). This is the approved “topmost event wins” rule for overlaps.
    [[nodiscard]] int findCoveringRowIndex(
        const SessionSnapshot& snap, const std::int64_t t) noexcept
    {
        const int n = snap.getNumPlacedClips();
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& p = snap.getPlacedClip(i);
            const std::int64_t s = p.getStartSample();
            const std::int64_t e = s + static_cast<std::int64_t>(p.getAudioClip().getNumSamples());
            if (t >= s && t < e)
            {
                return i;
            }
        }
        return -1;
    }

    // Smallest timeline position *strictly* after `t` where which clip (if any) is “on top” can
    // change. That is always a clip start, clip end, or the exclusive session end: coverage only
    // flips on those points — so a whole device block is filled in a small number of constant runs
    // without a per-sample search.
    [[nodiscard]] std::int64_t minBoundaryStrictlyAfter(
        const SessionSnapshot& snap, const std::int64_t t, const std::int64_t timelineEnd) noexcept
    {
        std::int64_t m = std::numeric_limits<std::int64_t>::max();
        const int n = snap.getNumPlacedClips();
        for (int i = 0; i < n; ++i)
        {
            const PlacedClip& p = snap.getPlacedClip(i);
            const std::int64_t s = p.getStartSample();
            const std::int64_t e = s + static_cast<std::int64_t>(p.getAudioClip().getNumSamples());
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

    // [Audio thread] Copy `run` frames from a single clip at `offInMaterial` into each device row,
    // starting at `outFrame0` — **only** the run; no `clear` to `numSamples` end (this callback may
    // write more runs to the same buffer later; full buffer is zeroed first).
    void copyClipRunToOutputs(const AudioClip& clip,
                              int offInMaterial,
                              int run,
                              int outFrame0,
                              int numOutChannels,
                              float* const* outputChannelData) noexcept
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
                juce::FloatVectorOperations::copy(
                    dest, buf.getReadPointer(0) + offInMaterial, run);
            }
            else if (outChannel < numSourceChannels)
            {
                juce::FloatVectorOperations::copy(
                    dest, buf.getReadPointer(outChannel) + offInMaterial, run);
            }
            // Else: more device outputs than file channels -> leave silent (pre-cleared).
        }
    }
} // namespace

PlaybackEngine::PlaybackEngine(Transport& transport, Session& session)
    : transport_(transport)
    , session_(session)
{
}

PlaybackEngine::~PlaybackEngine() = default;

// [Message thread]
void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    juce::ignoreUnused(device);
}

// [Message thread]
void PlaybackEngine::audioDeviceStopped() {}

// [Audio thread] Fills the output block: coverage runs + Transport advance. See file header.
void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

    const int deviceBlockSizeInFrames = numSamples;
    transport_.audioThread_beginBlock();

    const std::shared_ptr<const SessionSnapshot> sessionSnap = session_.loadSessionSnapshotForAudioThread();
    const PlaybackIntent playbackIntent = transport_.audioThread_loadIntent();
    std::int64_t t = transport_.audioThread_loadPlayhead();

    // Full silence default; runs below overwrite [0, R) when Playing.
    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        if (float* row = outputChannelData[ch])
        {
            juce::FloatVectorOperations::clear(row, numSamples);
        }
    }

    if (sessionSnap == nullptr || sessionSnap->isEmpty() || deviceBlockSizeInFrames <= 0
        || playbackIntent != PlaybackIntent::Playing)
    {
        // Not playing, or no material: the buffer stays silent; playhead is not moved this block.
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    const std::int64_t timelineEnd = sessionSnap->getDerivedTimelineLengthSamples();
    if (t >= timelineEnd)
    {
        // Past the derived end: silence for the full block, no advance (clamp-at-end, same as
        // one-clip at end of file; user must seek to hear again).
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    // Snapshot where this block *started* on the timeline: advance matches how far we really move
    // in session time, including through gaps (same R samples whether they are silence or clip).
    const std::int64_t t0 = t;

    // How many **timeline** samples this block is allowed to render: cannot extend past
    // `timelineEnd` (exclusive); shorter runs still zero-pad the rest of the device block.
    const std::int64_t canDo = juce::jmin(
        static_cast<std::int64_t>(deviceBlockSizeInFrames), timelineEnd - t0);
    const int R = static_cast<int>(canDo);
    if (R <= 0)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    // --- One or more *runs* [t, t+run) of constant "who wins" at every sample, each filled from
    //     at most one clip (or left silent if the run is a gap on the line). `nextB - t` is
    //     always the maximum length in which the winning row cannot change, because coverage only
    //     flips on clip start/end and session end. This is the phase-2 *stacking* model without
    //     ever summing overlapping material. ---
    int out0 = 0;
    while (out0 < R)
    {
        const int row = findCoveringRowIndex(*sessionSnap, t);
        const std::int64_t nextB = minBoundaryStrictlyAfter(*sessionSnap, t, timelineEnd);
        jassert(nextB > t);
        int run = static_cast<int>(juce::jmin(
            static_cast<std::int64_t>(R - out0), nextB - t));
        jassert(run > 0);

        if (row >= 0)
        {
            const PlacedClip& p = sessionSnap->getPlacedClip(row);
            const AudioClip& c = p.getAudioClip();
            // Offset into this clip’s `AudioBuffer` (material index 0 = first file sample) for the
            // first timeline sample `t` in this run.
            const int off = static_cast<int>(t - p.getStartSample());
            jassert(off >= 0 && off + run <= c.getNumSamples());
            copyClipRunToOutputs(c, off, run, out0, numOutputChannels, outputChannelData);
        }
        // `row < 0`: gap (no `PlacedClip` covers) — that span stays cleared (digital silence here).

        t += run;
        out0 += run;
    }

    jassert(out0 == R);
    jassert(t - t0 == static_cast<std::int64_t>(R));
    transport_.audioThread_advancePlayheadIfPlaying(static_cast<std::int64_t>(R));
}
