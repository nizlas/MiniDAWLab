#pragma once

#include "domain/Track.h"

#include <cstdint>
#include <vector>

class AudioClip;
class ExperimentalInstrumentHost;
class PlacedClip;
class PluginInsertHost;
class SessionSnapshot;

struct ExperimentalInstrumentPlaybackEntry;
struct ExperimentalInstrumentPlaybackSnapshot;

namespace playback_mix_helpers
{

/// Last `TrackKind::Master` row in timeline order (canonical Stereo Out bus).
[[nodiscard]] const Track* findCanonicalMasterTrack(const SessionSnapshot& snap) noexcept;

[[nodiscard]] int findCoveringRowIndexInLane(const std::vector<PlacedClip>& lane,
                                             std::int64_t t) noexcept;

[[nodiscard]] std::int64_t minBoundaryStrictlyAfterInLane(const std::vector<PlacedClip>& lane,
                                                          std::int64_t t,
                                                          std::int64_t timelineEnd) noexcept;

void addClipRunToOutputs(const AudioClip& clip,
                         int offInMaterial,
                         int run,
                         int outFrame0,
                         int numOutChannels,
                         float* const* outputChannelData,
                         float trackGain,
                         float stereoPan) noexcept;

void copyClipRunToStereoScratch(const AudioClip& clip,
                                int offInMaterial,
                                int run,
                                float* scratchL,
                                float* scratchR) noexcept;

void multiplyStereoScratchLR(float* const* scratchPtrs, int run, float gainL, float gainR) noexcept;

void addStereoScratchToDeviceOutputs(float* const* scratchPtrs,
                                     int run,
                                     int outFrame0,
                                     int numOutputChannels,
                                     float* const* outputChannelData,
                                     float trackGain) noexcept;

void scaleStereoScratch(float* const* scratchPtrs, int run, float gain) noexcept;

void mixExperimentalInstrumentAfterTracks(ExperimentalInstrumentHost* host,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          float instrumentGain,
                                          float stereoPan) noexcept;

[[nodiscard]] const ExperimentalInstrumentPlaybackEntry* findExperimentalInstrumentPlaybackEntry(
    const ExperimentalInstrumentPlaybackSnapshot& snap,
    TrackId trackId) noexcept;

/// Same clip summing / insert path as `PlaybackEngine::audioDeviceIOCallbackWithContext` `renderRun`
/// (timeline slice → device frames starting at `destOutFrame0`).
void renderAudioTracksClipSummingForSegment(const SessionSnapshot& sessionSnap,
                                            std::int64_t timelineStartAudible,
                                            int audibleRun,
                                            int destOutFrame0,
                                            int numOutputChannels,
                                            float* const* outputChannelData,
                                            PluginInsertHost* pluginHost,
                                            TrackId omitClipPlaybackForTrack,
                                            std::int64_t timelineEnd) noexcept;

/// [Audio thread] Apply one bus row's channel strip (Pre → fader/mute/off → Post → pan) from stereo
/// `busScratchStereo` (`[0]`/ `[1]` = L/R) into `outputChannelData` at `destOutFrame0` for `numSamples`.
void processBusChannelStripToOutputs(const Track& busTrack,
                                     float* const* busScratchStereo,
                                     int destOutFrame0,
                                     int numSamples,
                                     int numOutputChannels,
                                     float* const* outputChannelData,
                                     PluginInsertHost* pluginHost) noexcept;

} // namespace playback_mix_helpers
