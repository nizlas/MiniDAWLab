#pragma once

// =============================================================================
// ArrangementClipboard — pure placement plan for Copy/Paste of arrangement events
// =============================================================================
//
// The clipboard stores values (track offset, start offset, MIDI anchor offset). This
// header only decides where a paste lands. It does not touch the session, the
// clipboard, or undo.
//
// The earliest copied start is the time anchor. The topmost source track (lowest
// session index, including tracks that have no copied event) is the track anchor.
// Paste puts the time anchor at the snapped playhead and the track anchor on the
// caller-chosen destination track. Every other event keeps the same time shift and
// its saved track offset. One illegal destination (kind, range, collapsed member,
// or a start that is still before 0 after the existing negative-start shift) rejects
// the whole plan. Nothing is skipped and no track is created.

#include "domain/Track.h"
#include "ui/ArrangementGroupMove.h"

#include <cstdint>
#include <limits>
#include <vector>

struct ArrangementClipboardOffset
{
    ArrangementEventKind kind = ArrangementEventKind::AudioClip;
    /// Session-index distance from the topmost copied track. Zero for that track.
    int trackOffset = 0;
    /// Start distance from the earliest copied start. Zero for that event.
    std::int64_t startOffsetSamples = 0;
    /// MIDI only: `timelineAnchorSamples - startSamples` at copy time. Audio is 0.
    std::int64_t anchorOffsetFromStartSamples = 0;
};

struct ArrangementClipboardSourceSample
{
    ArrangementEventKind kind = ArrangementEventKind::AudioClip;
    int trackIndex = -1;
    std::int64_t startSamples = 0;
    std::int64_t anchorOffsetFromStartSamples = 0;
};

struct ArrangementClipboardPasteDest
{
    int destTrackIndex = -1;
    TrackId destTrackId = kInvalidTrackId;
    std::int64_t startSamples = 0;
    std::int64_t anchorSamples = 0;
};

struct ArrangementClipboardPastePlan
{
    bool valid = false;
    std::vector<ArrangementClipboardPasteDest> dests;
};

/// Fills offsets from the earliest start and the topmost source track. Fails closed
/// when any source track index is unknown.
[[nodiscard]] inline bool buildArrangementClipboardOffsets(
    const std::vector<ArrangementClipboardSourceSample>& sources,
    std::vector<ArrangementClipboardOffset>& out) noexcept
{
    out.clear();
    if (sources.empty())
    {
        return false;
    }
    int top = std::numeric_limits<int>::max();
    std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
    for (const ArrangementClipboardSourceSample& source : sources)
    {
        if (source.trackIndex < 0)
        {
            return false;
        }
        if (source.trackIndex < top)
        {
            top = source.trackIndex;
        }
        if (source.startSamples < earliest)
        {
            earliest = source.startSamples;
        }
    }
    out.reserve(sources.size());
    for (const ArrangementClipboardSourceSample& source : sources)
    {
        ArrangementClipboardOffset offset;
        offset.kind = source.kind;
        offset.trackOffset = source.trackIndex - top;
        offset.startOffsetSamples = source.startSamples - earliest;
        offset.anchorOffsetFromStartSamples = source.anchorOffsetFromStartSamples;
        out.push_back(offset);
    }
    return true;
}

/// `anchorTrackIndex` is where the topmost copied track lands. `timeAnchorSamples` is
/// the snapped playhead. A negative earliest start shifts the whole group so the
/// earliest start is 0 (the existing single MIDI paste rule). Kind, range and
/// collapsed-member checks still reject the whole paste after that shift.
[[nodiscard]] inline ArrangementClipboardPastePlan planArrangementClipboardPaste(
    const std::vector<ArrangementClipboardOffset>& items,
    const int anchorTrackIndex,
    const std::int64_t timeAnchorSamples,
    const std::vector<TrackKind>& kinds,
    const std::vector<TrackId>& ids,
    const std::vector<int>& collapsedHidden) noexcept
{
    ArrangementClipboardPastePlan plan;
    if (items.empty() || anchorTrackIndex < 0 || kinds.size() != ids.size()
        || collapsedHidden.size() != ids.size())
    {
        return plan;
    }
    plan.dests.resize(items.size());
    std::int64_t minStart = std::numeric_limits<std::int64_t>::max();
    for (size_t i = 0; i < items.size(); ++i)
    {
        plan.dests[i].destTrackIndex = anchorTrackIndex + items[i].trackOffset;
        plan.dests[i].startSamples = timeAnchorSamples + items[i].startOffsetSamples;
        plan.dests[i].anchorSamples = plan.dests[i].startSamples + items[i].anchorOffsetFromStartSamples;
        if (plan.dests[i].startSamples < minStart)
        {
            minStart = plan.dests[i].startSamples;
        }
    }
    if (minStart < 0)
    {
        const std::int64_t shift = -minStart;
        for (ArrangementClipboardPasteDest& dest : plan.dests)
        {
            dest.startSamples += shift;
            dest.anchorSamples += shift;
        }
    }
    bool ok = true;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const int destIndex = plan.dests[i].destTrackIndex;
        if (destIndex < 0 || destIndex >= (int)kinds.size())
        {
            ok = false;
            continue;
        }
        plan.dests[i].destTrackId = ids[(size_t)destIndex];
        if (collapsedHidden[(size_t)destIndex] != 0)
        {
            ok = false;
        }
        if (plan.dests[i].startSamples < 0)
        {
            ok = false;
        }
        const bool kindOk = items[i].kind == ArrangementEventKind::AudioClip
                                ? trackKindAcceptsTimelineAudioClips(kinds[(size_t)destIndex])
                                : trackKindOwnsTimelineMidiClips(kinds[(size_t)destIndex]);
        if (!kindOk)
        {
            ok = false;
        }
    }
    plan.valid = ok;
    return plan;
}
