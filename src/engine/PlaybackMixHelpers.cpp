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

const Track* findCanonicalMasterTrack(const SessionSnapshot& snap) noexcept
{
    const int idx = snap.findCanonicalMasterTrackIndex();
    if (idx < 0)
    {
        return nullptr;
    }
    return &snap.getTrack(idx);
}

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
                                            const std::int64_t timelineEnd,
                                            const int onlyTrackIndex) noexcept
{
    if (audibleRun <= 0)
    {
        return;
    }

    for (int ti = 0; ti < sessionSnap.getNumTracks(); ++ti)
    {
        if (onlyTrackIndex >= 0 && ti != onlyTrackIndex)
        {
            continue;
        }
        const Track& tr = sessionSnap.getTrack(ti);
        if (tr.getKind() == TrackKind::Instrument || tr.getKind() == TrackKind::Group
            || tr.getKind() == TrackKind::Master)
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

void processBusChannelStripToOutputs(const Track& busTrack,
                                     float* const* busScratchStereo,
                                     const int destOutFrame0,
                                     const int numSamples,
                                     const int numOutputChannels,
                                     float* const* outputChannelData,
                                     PluginInsertHost* pluginHost) noexcept
{
    if (numSamples <= 0 || busScratchStereo == nullptr || outputChannelData == nullptr)
    {
        return;
    }
    if (busTrack.isTrackOff())
    {
        return;
    }

    const float storedFaderGain = busTrack.getChannelFaderGain();
    const float effectiveGain = busTrack.isMuted() ? 0.0f : storedFaderGain;
    if (!busTrack.isMuted() && storedFaderGain <= 0.0f)
    {
        return;
    }

    const bool useInsert = pluginHost != nullptr
                           && pluginHost->audioThread_hasActivePluginForTrack(busTrack.getId());

    if (useInsert && effectiveGain > 0.0f)
    {
        pluginHost->audioThread_clearScratch(PluginInsertHost::kInsertChannels, numSamples);
        if (float* const* scratch = pluginHost->audioThread_getScratchWritePointers())
        {
            if (scratch[0] != nullptr && scratch[1] != nullptr && busScratchStereo[0] != nullptr
                && busScratchStereo[1] != nullptr)
            {
                juce::FloatVectorOperations::copy(scratch[0] + destOutFrame0,
                                                  busScratchStereo[0] + destOutFrame0,
                                                  numSamples);
                juce::FloatVectorOperations::copy(scratch[1] + destOutFrame0,
                                                  busScratchStereo[1] + destOutFrame0,
                                                  numSamples);
            }
            pluginHost->audioThread_processChainForTrack(busTrack.getId(), InsertStage::Pre, numSamples);
            scaleStereoScratch(scratch, numSamples, effectiveGain);
            pluginHost->audioThread_processChainForTrack(busTrack.getId(), InsertStage::Post, numSamples);
            multiplyStereoScratchLR(scratch,
                                    numSamples,
                                    trackPanLawGainLeft(busTrack.getStereoPan()),
                                    trackPanLawGainRight(busTrack.getStereoPan()));
            addStereoScratchToDeviceOutputs(scratch,
                                            numSamples,
                                            destOutFrame0,
                                            numOutputChannels,
                                            outputChannelData,
                                            1.0f);
        }
        return;
    }

    const float gL = trackPanLawGainLeft(busTrack.getStereoPan());
    const float gR = trackPanLawGainRight(busTrack.getStereoPan());

    if (numOutputChannels == 1)
    {
        float* d = outputChannelData[0];
        if (d != nullptr && busScratchStereo[0] != nullptr && busScratchStereo[1] != nullptr)
        {
            const float* l = busScratchStereo[0] + destOutFrame0;
            const float* r = busScratchStereo[1] + destOutFrame0;
            float* dest = d + destOutFrame0;
            for (int i = 0; i < numSamples; ++i)
            {
                dest[i] += effectiveGain * 0.5f * (gL * l[i] + gR * r[i]);
            }
        }
        return;
    }

    if (numOutputChannels >= 1 && busScratchStereo[0] != nullptr)
    {
        float* d0 = outputChannelData[0];
        if (d0 != nullptr)
        {
            juce::FloatVectorOperations::addWithMultiply(
                d0 + destOutFrame0, busScratchStereo[0] + destOutFrame0, effectiveGain * gL, numSamples);
        }
    }
    if (numOutputChannels >= 2 && busScratchStereo[1] != nullptr)
    {
        float* d1 = outputChannelData[1];
        if (d1 != nullptr)
        {
            juce::FloatVectorOperations::addWithMultiply(
                d1 + destOutFrame0, busScratchStereo[1] + destOutFrame0, effectiveGain * gR, numSamples);
        }
    }
}

void clearStereoScratch(float* scratchL, float* scratchR, const int numSamples) noexcept
{
    if (numSamples <= 0)
    {
        return;
    }
    if (scratchL != nullptr)
    {
        juce::FloatVectorOperations::clear(scratchL, numSamples);
    }
    if (scratchR != nullptr)
    {
        juce::FloatVectorOperations::clear(scratchR, numSamples);
    }
}

void addPostStripStageToDeviceOutputs(float* const* stageStereo,
                                      const int destOutFrame0,
                                      const int numSamples,
                                      const int numOutputChannels,
                                      float* const* outputChannelData) noexcept
{
    addStereoScratchToDeviceOutputs(
        stageStereo, numSamples, destOutFrame0, numOutputChannels, outputChannelData, 1.0f);
}

void addPostStripStageToBus(float* stageL,
                            float* stageR,
                            float* busL,
                            float* busR,
                            const int destOutFrame0,
                            const int numSamples,
                            const float gain) noexcept
{
    if (numSamples <= 0 || gain <= 0.0f)
    {
        return;
    }
    if (busL != nullptr && stageL != nullptr)
    {
        juce::FloatVectorOperations::addWithMultiply(
            busL + destOutFrame0, stageL + destOutFrame0, gain, numSamples);
    }
    if (busR != nullptr && stageR != nullptr)
    {
        juce::FloatVectorOperations::addWithMultiply(
            busR + destOutFrame0, stageR + destOutFrame0, gain, numSamples);
    }
}

namespace
{
    void addClipRunToStereoScratch(const AudioClip& clip,
                                   const int offInMaterial,
                                   const int run,
                                   const int outFrame0,
                                   float* scratchL,
                                   float* scratchR,
                                   const float trackGain,
                                   const float stereoPan) noexcept
    {
        float* const scratchPtrs[2] = { scratchL, scratchR };
        addClipRunToOutputs(clip,
                            offInMaterial,
                            run,
                            outFrame0,
                            2,
                            scratchPtrs,
                            trackGain,
                            stereoPan);
    }

    void addStereoScratchToStereoScratch(float* destL,
                                         float* destR,
                                         const float* srcL,
                                         const float* srcR,
                                         const int outFrame0,
                                         const int run) noexcept
    {
        if (destL != nullptr && srcL != nullptr)
        {
            juce::FloatVectorOperations::add(destL + outFrame0, srcL + outFrame0, run);
        }
        if (destR != nullptr && srcR != nullptr)
        {
            juce::FloatVectorOperations::add(destR + outFrame0, srcR + outFrame0, run);
        }
    }
} // namespace

void renderAudioTrackPostStripToStereoScratch(const SessionSnapshot& sessionSnap,
                                              const std::int64_t timelineStartAudible,
                                              const int audibleRun,
                                              const int destOutFrame0,
                                              float* stageL,
                                              float* stageR,
                                              PluginInsertHost* pluginHost,
                                              const TrackId omitClipPlaybackForTrack,
                                              const std::int64_t timelineEnd,
                                              const int trackIndex) noexcept
{
    if (audibleRun <= 0 || stageL == nullptr || stageR == nullptr || trackIndex < 0
        || trackIndex >= sessionSnap.getNumTracks())
    {
        return;
    }

    const Track& tr = sessionSnap.getTrack(trackIndex);
    if (tr.getKind() != TrackKind::Audio)
    {
        return;
    }
    if (omitClipPlaybackForTrack != kInvalidTrackId && tr.getId() == omitClipPlaybackForTrack)
    {
        return;
    }
    if (tr.isTrackOff())
    {
        return;
    }
    const float storedFaderGain = tr.getChannelFaderGain();
    const float effectiveGain = tr.isMuted() ? 0.0f : storedFaderGain;
    if (!tr.isMuted() && storedFaderGain <= 0.0f)
    {
        return;
    }

    const std::vector<PlacedClip>& lane = tr.getPlacedClips();
    const bool useInsert
        = pluginHost != nullptr && pluginHost->audioThread_hasActivePluginForTrack(tr.getId());
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
                    addStereoScratchToStereoScratch(
                        stageL, stageR, scratch[0], scratch[1], destFrame, run);
                }
            }
            else
            {
                addClipRunToStereoScratch(
                    c, off, run, destFrame, stageL, stageR, effectiveGain, tr.getStereoPan());
            }
        }
        t += run;
        out0 += run;
    }
    jassert(out0 == audibleRun);
}

void renderInstrumentPostStripToStereoScratch(ExperimentalInstrumentHost* host,
                                              const Track& track,
                                              float* stageL,
                                              float* stageR,
                                              const int destOutFrame0,
                                              const int numSamples,
                                              PluginInsertHost* pluginHost) noexcept
{
    if (host == nullptr || numSamples <= 0 || stageL == nullptr || stageR == nullptr)
    {
        return;
    }
    clearStereoScratch(stageL, stageR, numSamples);
    if (track.isTrackOff())
    {
        return;
    }
    const float storedFaderGain = track.getChannelFaderGain();
    const float effectiveGain = track.isMuted() ? 0.0f : storedFaderGain;
    if (!track.isMuted() && storedFaderGain <= 0.0f)
    {
        return;
    }

    const TrackId trackId = track.getId();
    const bool useInsert
        = pluginHost != nullptr && pluginHost->audioThread_hasActivePluginForTrack(trackId);

    if (useInsert && effectiveGain > 0.0f)
    {
        pluginHost->audioThread_clearScratch(PluginInsertHost::kInsertChannels, numSamples);
        if (float* const* scratch = pluginHost->audioThread_getScratchWritePointers())
        {
            mixExperimentalInstrumentAfterTracks(
                host, scratch, 2, numSamples, 1.0f, kTrackStereoPanCenter);
            pluginHost->audioThread_processChainForTrack(trackId, InsertStage::Pre, numSamples);
            scaleStereoScratch(scratch, numSamples, effectiveGain);
            pluginHost->audioThread_processChainForTrack(trackId, InsertStage::Post, numSamples);
            multiplyStereoScratchLR(scratch,
                                    numSamples,
                                    trackPanLawGainLeft(track.getStereoPan()),
                                    trackPanLawGainRight(track.getStereoPan()));
            addStereoScratchToStereoScratch(
                stageL, stageR, scratch[0], scratch[1], destOutFrame0, numSamples);
        }
        return;
    }

    float* const stagePtrs[2] = { stageL, stageR };
    mixExperimentalInstrumentAfterTracks(
        host, stagePtrs, 2, numSamples, effectiveGain, track.getStereoPan());
    juce::ignoreUnused(destOutFrame0);
}

void applyBusPostChannelStripFromInputToStage(const Track& busTrack,
                                              float* const* busInputStereo,
                                              float* stageL,
                                              float* stageR,
                                              const int destOutFrame0,
                                              const int numSamples,
                                              PluginInsertHost* pluginHost) noexcept
{
    if (numSamples <= 0 || busInputStereo == nullptr || stageL == nullptr || stageR == nullptr)
    {
        return;
    }
    if (busTrack.isTrackOff())
    {
        clearStereoScratch(stageL, stageR, numSamples);
        return;
    }

    const float storedFaderGain = busTrack.getChannelFaderGain();
    const float effectiveGain = busTrack.isMuted() ? 0.0f : storedFaderGain;
    if (!busTrack.isMuted() && storedFaderGain <= 0.0f)
    {
        clearStereoScratch(stageL, stageR, numSamples);
        return;
    }

    const bool useInsert = pluginHost != nullptr
                           && pluginHost->audioThread_hasActivePluginForTrack(busTrack.getId());

    if (useInsert && effectiveGain > 0.0f)
    {
        pluginHost->audioThread_clearScratch(PluginInsertHost::kInsertChannels, numSamples);
        if (float* const* scratch = pluginHost->audioThread_getScratchWritePointers())
        {
            if (scratch[0] != nullptr && scratch[1] != nullptr && busInputStereo[0] != nullptr
                && busInputStereo[1] != nullptr)
            {
                juce::FloatVectorOperations::copy(scratch[0] + destOutFrame0,
                                                  busInputStereo[0] + destOutFrame0,
                                                  numSamples);
                juce::FloatVectorOperations::copy(scratch[1] + destOutFrame0,
                                                  busInputStereo[1] + destOutFrame0,
                                                  numSamples);
            }
            pluginHost->audioThread_processChainForTrack(busTrack.getId(), InsertStage::Pre, numSamples);
            scaleStereoScratch(scratch, numSamples, effectiveGain);
            pluginHost->audioThread_processChainForTrack(busTrack.getId(), InsertStage::Post, numSamples);
            multiplyStereoScratchLR(scratch,
                                    numSamples,
                                    trackPanLawGainLeft(busTrack.getStereoPan()),
                                    trackPanLawGainRight(busTrack.getStereoPan()));
            juce::FloatVectorOperations::copy(stageL + destOutFrame0, scratch[0] + destOutFrame0, numSamples);
            juce::FloatVectorOperations::copy(stageR + destOutFrame0, scratch[1] + destOutFrame0, numSamples);
        }
        return;
    }

    const float gL = trackPanLawGainLeft(busTrack.getStereoPan());
    const float gR = trackPanLawGainRight(busTrack.getStereoPan());
    if (busInputStereo[0] != nullptr)
    {
        juce::FloatVectorOperations::copy(stageL + destOutFrame0,
                                          busInputStereo[0] + destOutFrame0,
                                          numSamples);
        juce::FloatVectorOperations::multiply(stageL + destOutFrame0, effectiveGain * gL, numSamples);
    }
    else
    {
        juce::FloatVectorOperations::clear(stageL + destOutFrame0, numSamples);
    }
    if (busInputStereo[1] != nullptr)
    {
        juce::FloatVectorOperations::copy(stageR + destOutFrame0,
                                          busInputStereo[1] + destOutFrame0,
                                          numSamples);
        juce::FloatVectorOperations::multiply(stageR + destOutFrame0, effectiveGain * gR, numSamples);
    }
    else
    {
        juce::FloatVectorOperations::clear(stageR + destOutFrame0, numSamples);
    }
}

void fanPostStripStageToDryAndSends(float* stageL,
                                    float* stageR,
                                    const int destOutFrame0,
                                    const int numSamples,
                                    const int dryBusIndex,
                                    const std::vector<RoutingPlan::SendTap>& sends,
                                    const RoutingPlan& plan) noexcept
{
    if (numSamples <= 0 || stageL == nullptr || stageR == nullptr)
    {
        return;
    }
    if (dryBusIndex >= 0 && dryBusIndex < static_cast<int>(plan.busScratchL.size()))
    {
        addPostStripStageToBus(stageL,
                               stageR,
                               plan.busScratchL[(size_t)dryBusIndex],
                               plan.busScratchR[(size_t)dryBusIndex],
                               destOutFrame0,
                               numSamples,
                               1.0f);
    }
    for (const RoutingPlan::SendTap& tap : sends)
    {
        if (tap.destBusIndex < 0 || tap.amountLinear <= 0.0f
            || tap.destBusIndex >= static_cast<int>(plan.busScratchL.size()))
        {
            continue;
        }
        addPostStripStageToBus(stageL,
                               stageR,
                               plan.busScratchL[(size_t)tap.destBusIndex],
                               plan.busScratchR[(size_t)tap.destBusIndex],
                               destOutFrame0,
                               numSamples,
                               tap.amountLinear);
    }
}

} // namespace playback_mix_helpers
