#pragma once

// Groups finished Volume/Pan strokes by gesture id. A seek split stays in the
// same group. The window makes one undo step per group, after the pointer is up
// and the runtime has no further stroke for that id.

#include "engine/TrackAutomationRuntime.h"

#include <vector>

struct TrackAutomationStroke
{
    TrackId trackId = kInvalidTrackId;
    int parameterId = 0;
    TrackAutomationCommit commit{};
    std::vector<TrackAutomationPoint> samples;
};

struct TrackAutomationUndoGroup
{
    uint32_t gestureId = 0;
    bool userReleased = false;
    std::vector<TrackAutomationStroke> strokes;
};

inline void trackAutomationCollectStroke(std::vector<TrackAutomationUndoGroup>& groups,
                                         TrackAutomationStroke stroke)
{
    for (TrackAutomationUndoGroup& group : groups)
    {
        if (group.gestureId == stroke.commit.gestureId)
        {
            group.strokes.push_back(std::move(stroke));
            return;
        }
    }
    TrackAutomationUndoGroup created;
    created.gestureId = stroke.commit.gestureId;
    created.strokes.push_back(std::move(stroke));
    groups.push_back(std::move(created));
}

/// True when the user has released and the runtime is not still capturing or
/// holding an untaken stroke of this gesture.
[[nodiscard]] inline bool trackAutomationGroupReady(const TrackAutomationUndoGroup& group,
                                                    const bool gestureBusy) noexcept
{
    return group.userReleased && !group.strokes.empty() && !gestureBusy;
}
