#pragma once

#include "domain/Track.h"
#include "engine/RoutingPlan.h"

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

/// Copy/add post-channel-strip `stage` into `outputChannelData` (mono fold or stereo L/R).
void addPostStripStageToDeviceOutputs(float* const* stageStereo,
                                      int destOutFrame0,
                                      int numSamples,
                                      int numOutputChannels,
                                      float* const* outputChannelData) noexcept;

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
                                            std::int64_t timelineEnd,
                                            int onlyTrackIndex = -1) noexcept;

/// [Audio thread] Apply one bus row's channel strip (Pre → fader/mute/off → Post → pan) from stereo
/// `busScratchStereo` (`[0]`/ `[1]` = L/R) into `outputChannelData` at `destOutFrame0` for `numSamples`.
void processBusChannelStripToOutputs(const Track& busTrack,
                                     float* const* busScratchStereo,
                                     int destOutFrame0,
                                     int numSamples,
                                     int numOutputChannels,
                                     float* const* outputChannelData,
                                     PluginInsertHost* pluginHost) noexcept;

void clearStereoScratch(float* scratchL, float* scratchR, int numSamples) noexcept;

/// Add post-channel-strip stereo (`stage`) into bus scratch at `destOutFrame0` for `numSamples`.
void addPostStripStageToBus(float* stageL,
                            float* stageR,
                            float* busL,
                            float* busR,
                            int destOutFrame0,
                            int numSamples,
                            float gain) noexcept;

/// One audio lane: clips → Pre → fader/mute/off → Post → pan → `stageL`/`stageR` (accumulated).
void renderAudioTrackPostStripToStereoScratch(const SessionSnapshot& sessionSnap,
                                              std::int64_t timelineStartAudible,
                                              int audibleRun,
                                              int destOutFrame0,
                                              float* stageL,
                                              float* stageR,
                                              PluginInsertHost* pluginHost,
                                              TrackId omitClipPlaybackForTrack,
                                              std::int64_t timelineEnd,
                                              int trackIndex) noexcept;

/// Instrument synth → Pre → fader/mute/off → Post → pan into `stageL`/`stageR` (replaces stage segment).
void renderInstrumentPostStripToStereoScratch(ExperimentalInstrumentHost* host,
                                              const Track& track,
                                              float* stageL,
                                              float* stageR,
                                              int destOutFrame0,
                                              int numSamples,
                                              PluginInsertHost* pluginHost) noexcept;

/// Group bus input scratch → post-channel-strip in `stageL`/`stageR` (replaces stage for segment).
void applyBusPostChannelStripFromInputToStage(const Track& busTrack,
                                              float* const* busInputStereo,
                                              float* stageL,
                                              float* stageR,
                                              int destOutFrame0,
                                              int numSamples,
                                              PluginInsertHost* pluginHost) noexcept;

/// Dry bus += stage; each send bus += stage * amount (post-channel-strip fan-out).
void fanPostStripStageToDryAndSends(float* stageL,
                                    float* stageR,
                                    int destOutFrame0,
                                    int numSamples,
                                    int dryBusIndex,
                                    const std::vector<RoutingPlan::SendTap>& sends,
                                    const RoutingPlan& plan) noexcept;

} // namespace playback_mix_helpers
