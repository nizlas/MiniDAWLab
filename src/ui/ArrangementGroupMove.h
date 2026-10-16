#pragma once

// =============================================================================
// ArrangementGroupMove — pure plan for moving several arrangement events together
// =============================================================================
//
// ROLE
//   One shared time delta and one shared track-index delta for every selected arrangement
//   event (audio clip or MIDI clip). The delta comes from the grabbed event: its snapped
//   start, and the session-track index under the pointer. Other events are not snapped on
//   their own and illegal tracks are not skipped.
//
//   The whole plan is invalid when any destination is before sample 0, outside the track
//   list, on a track kind that single-move would refuse, or on a hidden member of a
//   collapsed visual group. Invalid plans still list every event so the arrangement can
//   draw the whole preview; nothing here mutates the session.
//
// Threading: pure function. Callers run it on the message thread.
// =============================================================================

#include "domain/Track.h"

#include <cstdint>
#include <vector>

enum class ArrangementEventKind : std::uint8_t
{
    AudioClip,
    MidiClip
};

struct ArrangementEventId
{
    ArrangementEventKind kind = ArrangementEventKind::AudioClip;
    TrackId trackId = kInvalidTrackId;
    std::uint64_t eventId = 0;

    friend bool operator==(const ArrangementEventId& a, const ArrangementEventId& b) noexcept
    {
        return a.kind == b.kind && a.trackId == b.trackId && a.eventId == b.eventId;
    }
};

struct ArrangementEventOrigin
{
    ArrangementEventId id;
    int trackIndex = -1;
    std::int64_t startSamples = 0;
    std::int64_t lengthSamples = 0;
};

struct ArrangementMoveDest
{
    ArrangementEventId id;
    TrackId destTrackId = kInvalidTrackId;
    int destTrackIndex = -1;
    std::int64_t destStartSamples = 0;
    std::int64_t lengthSamples = 0;
};

struct ArrangementGroupMovePlan
{
    bool valid = false;
    std::int64_t timeDelta = 0;
    int trackDelta = 0;
    std::vector<ArrangementMoveDest> dests;
};

/// `pointerTrackIndex` is the session index of the row under the pointer, or -1 when that
/// point is not a legal destination row (header, ruler, collapsed mini-strip, or outside).
/// `collapsedHiddenMember[i] == 1` when session track `i` is a hidden member of a collapsed
/// visual group. The three session vectors must be the same length.
[[nodiscard]] inline ArrangementGroupMovePlan planArrangementGroupMove(
    const std::vector<ArrangementEventOrigin>& origins,
    const ArrangementEventId& grabbed,
    const std::int64_t snappedGrabbedStart,
    const int pointerTrackIndex,
    const std::vector<TrackKind>& kindsInSessionOrder,
    const std::vector<TrackId>& idsInSessionOrder,
    const std::vector<int>& collapsedHiddenMember) noexcept
{
    ArrangementGroupMovePlan plan;
    const int trackCount = (int)kindsInSessionOrder.size();
    if (origins.empty() || idsInSessionOrder.size() != kindsInSessionOrder.size()
        || collapsedHiddenMember.size() != kindsInSessionOrder.size())
    {
        return plan;
    }

    const ArrangementEventOrigin* grabbedOrigin = nullptr;
    for (const ArrangementEventOrigin& origin : origins)
    {
        if (origin.id == grabbed)
        {
            grabbedOrigin = &origin;
            break;
        }
    }
    if (grabbedOrigin == nullptr)
    {
        return plan;
    }

    plan.timeDelta = snappedGrabbedStart - grabbedOrigin->startSamples;
    plan.trackDelta = pointerTrackIndex < 0 ? 0 : pointerTrackIndex - grabbedOrigin->trackIndex;
    plan.dests.reserve(origins.size());

    bool allLegal = pointerTrackIndex >= 0;
    for (const ArrangementEventOrigin& origin : origins)
    {
        ArrangementMoveDest dest;
        dest.id = origin.id;
        dest.destStartSamples = origin.startSamples + plan.timeDelta;
        dest.lengthSamples = origin.lengthSamples;
        dest.destTrackIndex = origin.trackIndex + plan.trackDelta;
        if (dest.destStartSamples < 0)
        {
            allLegal = false;
        }
        if (dest.destTrackIndex < 0 || dest.destTrackIndex >= trackCount)
        {
            allLegal = false;
            dest.destTrackIndex = -1;
        }
        else
        {
            dest.destTrackId = idsInSessionOrder[(size_t)dest.destTrackIndex];
            if (collapsedHiddenMember[(size_t)dest.destTrackIndex] != 0)
            {
                allLegal = false;
            }
            const TrackKind kind = kindsInSessionOrder[(size_t)dest.destTrackIndex];
            const bool kindOk = origin.id.kind == ArrangementEventKind::AudioClip
                                    ? trackKindAcceptsTimelineAudioClips(kind)
                                    : trackKindOwnsTimelineMidiClips(kind);
            if (!kindOk)
            {
                allLegal = false;
            }
        }
        plan.dests.push_back(dest);
    }
    plan.valid = allLegal;
    return plan;
}
