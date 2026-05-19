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
    };

    struct BusStep
    {
        int trackIndex = -1;
        int sourceBusIndex = -1;
        int destBusIndex = -1;
        std::vector<SendTap> sends;
    };

    std::vector<SourceStep> sourceSteps;
    std::vector<BusStep> busSteps;
    std::size_t masterBusIndex = 0;

    /// Stereo scratch pointers `[busIndex][0/1]`; valid for the plan's lifetime (owned by `PlaybackEngine` pool).
    std::vector<float*> busScratchL;
    std::vector<float*> busScratchR;
};
