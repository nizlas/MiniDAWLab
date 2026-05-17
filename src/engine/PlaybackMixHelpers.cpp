#include "engine/PlaybackMixHelpers.h"

#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/TrackStereoPan.h"
#include "engine/PlaybackEngine.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <limits>

namespace playback_mix_helpers
{

int findCoveringRowIndexInLane(const std::vector<PlacedClip>& lane, const std::int64_t t) noexcept
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

std::int64_t minBoundaryStrictlyAfterInLane(const std::vector<PlacedClip>& lane,
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
                         const int offInMaterial,
                         const int run,
                         const int outFrame0,
                         const int numOutChannels,
                         float* const* outputChannelData,
                         const float trackGain,
                         const float stereoPan) noexcept
{
    const int numSourceChannels = clip.getNumChannels();
    const juce::AudioBuffer<float>& buf = clip.getAudio();
    const float gL = trackPanLawGainLeft(stereoPan);
    const float gR = trackPanLawGainRight(stereoPan);

    if (numOutChannels == 1 && numSourceChannels == 1)
    {
        float* d = outputChannelData[0];
        if (d != nullptr)
        {
            const float fold = 0.5f * (gL + gR) * trackGain;
            juce::FloatVectorOperations::addWithMultiply(
                d + outFrame0, buf.getReadPointer(0) + offInMaterial, fold, run);
        }
        return;
    }

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
            const float g = (outChannel == 0) ? gL : gR;
            juce::FloatVectorOperations::addWithMultiply(
                dest, buf.getReadPointer(0) + offInMaterial, trackGain * g, run);
        }
        else if (outChannel < numSourceChannels)
        {
            const float bal = (outChannel == 0) ? gL : gR;
            juce::FloatVectorOperations::addWithMultiply(
                dest, buf.getReadPointer(outChannel) + offInMaterial, trackGain * bal, run);
        }
    }
}

void copyClipRunToStereoScratch(const AudioClip& clip,
                                const int offInMaterial,
                                const int run,
                                float* scratchL,
                                float* scratchR) noexcept
{
    const int numSourceChannels = clip.getNumChannels();
    const juce::AudioBuffer<float>& buf = clip.getAudio();
    const float* src0 = buf.getReadPointer(0) + offInMaterial;
    if (numSourceChannels == 1)
    {
        juce::FloatVectorOperations::copy(scratchL, src0, run);
        juce::FloatVectorOperations::copy(scratchR, src0, run);
    }
    else
    {
        juce::FloatVectorOperations::copy(scratchL, src0, run);
        if (numSourceChannels >= 2)
        {
            juce::FloatVectorOperations::copy(scratchR, buf.getReadPointer(1) + offInMaterial, run);
        }
        else
        {
            juce::FloatVectorOperations::clear(scratchR, run);
        }
    }
}

void multiplyStereoScratchLR(float* const* scratchPtrs, const int run, const float gainL, const float gainR) noexcept
{
    if (scratchPtrs == nullptr || run <= 0)
    {
        return;
    }
    if (float* L = scratchPtrs[0])
    {
        juce::FloatVectorOperations::multiply(L, gainL, run);
    }
    if (float* R = scratchPtrs[1])
    {
        juce::FloatVectorOperations::multiply(R, gainR, run);
    }
}

void addStereoScratchToDeviceOutputs(float* const* scratchPtrs,
                                     const int run,
                                     const int outFrame0,
                                     const int numOutputChannels,
                                     float* const* outputChannelData,
                                     const float trackGain) noexcept
{
    if (scratchPtrs == nullptr || run <= 0)
    {
        return;
    }
    const float* L = scratchPtrs[0];
    const float* R = scratchPtrs[1];
    if (L == nullptr || R == nullptr)
    {
        return;
    }
    if (numOutputChannels <= 0)
    {
        return;
    }
    if (numOutputChannels == 1)
    {
        float* d = outputChannelData[0];
        if (d != nullptr)
        {
            float* const dest = d + outFrame0;
            const float halfGain = 0.5f * trackGain;
            juce::FloatVectorOperations::addWithMultiply(dest, L, halfGain, run);
            juce::FloatVectorOperations::addWithMultiply(dest, R, halfGain, run);
        }
    }
    else
    {
        if (float* d0 = outputChannelData[0])
        {
            juce::FloatVectorOperations::addWithMultiply(d0 + outFrame0, L, trackGain, run);
        }
        if (numOutputChannels >= 2)
        {
            if (float* d1 = outputChannelData[1])
            {
                juce::FloatVectorOperations::addWithMultiply(d1 + outFrame0, R, trackGain, run);
            }
        }
    }
}

void scaleStereoScratch(float* const* scratchPtrs, const int run, const float gain) noexcept
{
    if (scratchPtrs == nullptr || run <= 0)
    {
        return;
    }
    if (float* L = scratchPtrs[0])
    {
        juce::FloatVectorOperations::multiply(L, gain, run);
    }
    if (float* R = scratchPtrs[1])
    {
        juce::FloatVectorOperations::multiply(R, gain, run);
    }
}

void mixExperimentalInstrumentAfterTracks(ExperimentalInstrumentHost* host,
                                          float* const* outputChannelData,
                                          const int numOutputChannels,
                                          const int numSamples,
                                          const float instrumentGain,
                                          const float stereoPan) noexcept
{
    if (host == nullptr || numSamples <= 0)
    {
        return;
    }
    host->audioThread_processBlockAndAddToOutputs(
        outputChannelData, numOutputChannels, numSamples, instrumentGain, stereoPan);
}

const ExperimentalInstrumentPlaybackEntry* findExperimentalInstrumentPlaybackEntry(
    const ExperimentalInstrumentPlaybackSnapshot& snap,
    const TrackId trackId) noexcept
{
    for (const auto& e : snap.entries)
    {
        if (e.trackId != trackId)
        {
            continue;
        }
        if (e.host == nullptr || e.midiController == nullptr)
        {
            continue;
        }
        return &e;
    }
    return nullptr;
}

void renderAudioTracksClipSummingForSegment(const SessionSnapshot& sessionSnap,
                                            const std::int64_t timelineStartAudible,
                                            const int audibleRun,
                                            const int destOutFrame0,
                                            const int numOutputChannels,
                                            float* const* outputChannelData,
                                            PluginInsertHost* pluginHost,
                                            const TrackId omitClipPlaybackForTrack,
                                            const std::int64_t timelineEnd) noexcept
{
    if (audibleRun <= 0)
    {
        return;
    }

    for (int ti = 0; ti < sessionSnap.getNumTracks(); ++ti)
    {
        const Track& tr = sessionSnap.getTrack(ti);
        if (tr.getKind() == TrackKind::Instrument)
        {
            continue;
        }
        if (omitClipPlaybackForTrack != kInvalidTrackId && tr.getId() == omitClipPlaybackForTrack)
        {
            continue;
        }
        if (tr.isTrackOff())
        {
            continue;
        }
        const float storedFaderGain = tr.getChannelFaderGain();
        const float effectiveGain = tr.isMuted() ? 0.0f : storedFaderGain;
        if (!tr.isMuted() && storedFaderGain <= 0.0f)
        {
            continue;
        }
        const std::vector<PlacedClip>& lane = tr.getPlacedClips();
        const bool useInsert = pluginHost != nullptr
                               && pluginHost->audioThread_hasActivePluginForTrack(tr.getId());
        std::int64_t t = timelineStartAudible;
        int out0 = 0;
        while (out0 < audibleRun)
        {
            const int row = findCoveringRowIndexInLane(lane, t);
            const std::int64_t nextB = minBoundaryStrictlyAfterInLane(lane, t, timelineEnd);
            jassert(nextB > t);
            int run = static_cast<int>(juce::jmin(
                static_cast<std::int64_t>(audibleRun - out0), nextB - t));
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
                const int destFrame = destOutFrame0 + out0;

                if (useInsert && effectiveGain > 0.0f)
                {
                    pluginHost->audioThread_clearScratch(PluginInsertHost::kInsertChannels, run);
                    if (float* const* scratch = pluginHost->audioThread_getScratchWritePointers())
                    {
                        copyClipRunToStereoScratch(c, off, run, scratch[0], scratch[1]);
                        pluginHost->audioThread_processChainForTrack(tr.getId(), InsertStage::Pre, run);
                        scaleStereoScratch(scratch, run, effectiveGain);
                        pluginHost->audioThread_processChainForTrack(tr.getId(), InsertStage::Post, run);
                        multiplyStereoScratchLR(scratch,
                                                run,
                                                trackPanLawGainLeft(tr.getStereoPan()),
                                                trackPanLawGainRight(tr.getStereoPan()));
                        addStereoScratchToDeviceOutputs(scratch,
                                                        run,
                                                        destFrame,
                                                        numOutputChannels,
                                                        outputChannelData,
                                                        1.0f);
                    }
                }
                else
                {
                    addClipRunToOutputs(
                        c, off, run, destFrame, numOutputChannels, outputChannelData, effectiveGain, tr.getStereoPan());
                }
            }
            t += run;
            out0 += run;
        }
        jassert(out0 == audibleRun);
        jassert(t - timelineStartAudible == static_cast<std::int64_t>(audibleRun));
    }
}

} // namespace playback_mix_helpers
