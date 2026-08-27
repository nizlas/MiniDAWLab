#pragma once

#include "domain/Track.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class SessionSnapshot;

/// Immutable routing topology for one audio block. Built on the message thread; read on the audio thread.
struct RoutingPlan
{
    struct SendTap
    {
        int destBusIndex = -1;
        float amountLinear = 0.0f;
    };

    struct SourceStep
    {
        int trackIndex = -1;
        int destBusIndex = -1;
        std::vector<SendTap> sends;
        /// Stability C3: identity of the track this step was built from. Lets invariant checks
        /// (and diagnostics) verify that `trackIndex` still refers to the same session row after
        /// structural edits. Not used by the realtime render path.
        TrackId builtFromTrackId = kInvalidTrackId;
        TrackKind builtFromTrackKind = TrackKind::Audio;
    };

    struct BusStep
    {
        int trackIndex = -1;
        int sourceBusIndex = -1;
        int destBusIndex = -1;
        std::vector<SendTap> sends;
        /// Stability C3: see SourceStep.
        TrackId builtFromTrackId = kInvalidTrackId;
        TrackKind builtFromTrackKind = TrackKind::Master;
    };

    std::vector<SourceStep> sourceSteps;
    std::vector<BusStep> busSteps;
    std::size_t masterBusIndex = 0;

    /// Stereo scratch pointers `[busIndex][0/1]`; valid for the plan's lifetime (kept alive by
    /// `busScratchOwners` below).
    std::vector<float*> busScratchL;
    std::vector<float*> busScratchR;

    /// Stability C4B: shared ownership of the scratch slots behind `busScratchL/R`. The
    /// `PlaybackEngine` pool replaces slots (never mutates/frees them in place), so a plan that is
    /// still retained by the audio thread keeps its buffers alive even after the pool moved on.
    /// Never dereferenced on the audio thread — lifetime anchor only.
    std::vector<std::shared_ptr<void>> busScratchOwners;
};
