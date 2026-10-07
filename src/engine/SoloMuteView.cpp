#include "engine/SoloMuteView.h"

#include "domain/SessionSnapshot.h"

#include <algorithm>

namespace solo_mute_view
{

namespace
{
    /// Forward audio edges exactly as the routing plan wires them (`RoutingPlanBuilder`): the
    /// main output (a Group or Master row) plus every ENABLED send whose destination is a Group
    /// row. Appends destination snapshot indices to `out` (no dedup — callers guard with visited).
    void appendForwardAudioEdges(const SessionSnapshot& snap, const int fromIdx, std::vector<int>& out)
    {
        const Track& tr = snap.getTrack(fromIdx);
        if (tr.getKind() == TrackKind::Master)
        {
            return;
        }
        const int mainOutIdx = snap.findTrackIndexById(tr.getRoutedOutputTrackId());
        if (mainOutIdx >= 0 && mainOutIdx != fromIdx)
        {
            const TrackKind k = snap.getTrack(mainOutIdx).getKind();
            if (k == TrackKind::Group || k == TrackKind::Master)
            {
                out.push_back(mainOutIdx);
            }
        }
        for (int si = 0; si < tr.getNumSends(); ++si)
        {
            const TrackSend& send = tr.getSend(si);
            if (!send.enabled || send.destTrackId == kInvalidTrackId)
            {
                continue;
            }
            const int destIdx = snap.findTrackIndexById(send.destTrackId);
            if (destIdx >= 0 && destIdx != fromIdx && snap.getTrack(destIdx).getKind() == TrackKind::Group)
            {
                out.push_back(destIdx);
            }
        }
    }
} // namespace

std::shared_ptr<const SoloMuteView> deriveSoloMuteView(const SessionSnapshot& snap,
                                                       const std::vector<TrackId>& currentSoloSet)
{
    auto view = std::make_shared<SoloMuteView>();
    const int n = snap.getNumTracks();
    if (n <= 0)
    {
        return view;
    }

    // Effective explicit set: ids that exist in this snapshot, deduplicated, never the Master row.
    // Stale ids of deleted tracks fall out here — they can never produce a false-active solo.
    std::vector<char> isExplicit(static_cast<std::size_t>(n), 0);
    bool anyExplicit = false;
    for (const TrackId id : currentSoloSet)
    {
        if (id == kInvalidTrackId)
        {
            continue;
        }
        const int idx = snap.findTrackIndexById(id);
        if (idx < 0 || snap.getTrack(idx).getKind() == TrackKind::Master)
        {
            continue;
        }
        isExplicit[static_cast<std::size_t>(idx)] = 1;
        anyExplicit = true;
    }
    if (!anyExplicit)
    {
        return view; // soloActive stays false: an empty/fully-stale set never restricts playback.
    }
    view->soloActive = true;

    std::vector<SoloTrackAudioDecision> decision(static_cast<std::size_t>(n),
                                                 SoloTrackAudioDecision::ForcedSilent);
    // "Content open" = this track's OWN generated content belongs to the audible solo picture:
    // explicitly soloed sources and upstream feeders of a soloed Group. Controls MIDI gating.
    std::vector<char> contentOpen(static_cast<std::size_t>(n), 0);

    // Downstream closure: mark `startIdx` and everything the signal needs on its way to Stereo
    // Out (main-out chain + enabled send destinations and their chains) ForcedAudible.
    std::vector<int> stack;
    std::vector<char> visited(static_cast<std::size_t>(n), 0);
    const auto markDownstreamAudible = [&](const int startIdx) {
        std::fill(visited.begin(), visited.end(), char{0});
        stack.clear();
        stack.push_back(startIdx);
        visited[static_cast<std::size_t>(startIdx)] = 1;
        while (!stack.empty())
        {
            const int cur = stack.back();
            stack.pop_back();
            decision[static_cast<std::size_t>(cur)] = SoloTrackAudioDecision::ForcedAudible;
            std::vector<int> next;
            appendForwardAudioEdges(snap, cur, next);
            for (const int nx : next)
            {
                if (!visited[static_cast<std::size_t>(nx)])
                {
                    visited[static_cast<std::size_t>(nx)] = 1;
                    stack.push_back(nx);
                }
            }
        }
    };

    // Reverse adjacency (feeder → bus), built once, used for Group upstream closures.
    std::vector<std::vector<int>> reverseEdges(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        std::vector<int> next;
        appendForwardAudioEdges(snap, i, next);
        for (const int nx : next)
        {
            reverseEdges[static_cast<std::size_t>(nx)].push_back(i);
        }
    }

    // Pass 1: explicit solos.
    for (int i = 0; i < n; ++i)
    {
        if (!isExplicit[static_cast<std::size_t>(i)])
        {
            continue;
        }
        const Track& tr = snap.getTrack(i);
        const TrackKind kind = tr.getKind();

        if (kind == TrackKind::Midi)
        {
            // A pure MIDI lane produces no audio of its own; soloing it opens its DESTINATION
            // Instrument strip as a carrier (ForcedAudible, content stays gated) plus that
            // strip's downstream path. Off is always respected — solo never turns on an Off row.
            const int destIdx = snap.findTrackIndexById(tr.getMidiDestinationTrackId());
            if (destIdx >= 0 && snap.getTrack(destIdx).getKind() == TrackKind::Instrument
                && !snap.getTrack(destIdx).isTrackOff())
            {
                markDownstreamAudible(destIdx);
            }
            continue;
        }

        // Audio / Instrument / Group: the row's own content is part of the solo picture.
        contentOpen[static_cast<std::size_t>(i)] = 1;
        if (!tr.isTrackOff())
        {
            markDownstreamAudible(i);
        }

        if (kind == TrackKind::Group && !tr.isTrackOff())
        {
            // Upstream closure: everything that feeds this Group (main out or enabled sends,
            // transitively, stopping at Off rows) passes WITH ITS STORED MUTE STATE — solo on a
            // Group means "hear what the group currently plays", so base-muted feeders stay
            // silent and are never force-opened.
            std::fill(visited.begin(), visited.end(), char{0});
            stack.clear();
            stack.push_back(i);
            visited[static_cast<std::size_t>(i)] = 1;
            while (!stack.empty())
            {
                const int cur = stack.back();
                stack.pop_back();
                for (const int feeder : reverseEdges[static_cast<std::size_t>(cur)])
                {
                    if (visited[static_cast<std::size_t>(feeder)])
                    {
                        continue;
                    }
                    visited[static_cast<std::size_t>(feeder)] = 1;
                    if (snap.getTrack(feeder).isTrackOff())
                    {
                        continue; // Off blocks the path; rows beyond it do not reach this Group.
                    }
                    if (decision[static_cast<std::size_t>(feeder)] == SoloTrackAudioDecision::ForcedSilent)
                    {
                        decision[static_cast<std::size_t>(feeder)] = SoloTrackAudioDecision::PassStoredMute;
                    }
                    contentOpen[static_cast<std::size_t>(feeder)] = 1;
                    stack.push_back(feeder);
                }
            }
        }
    }

    // Pass 2: MIDI gating (see header). Only OPEN destination strips need event-level gating —
    // a ForcedSilent instrument keeps consuming its MIDI with output zeroed (mute semantics).
    for (int i = 0; i < n; ++i)
    {
        const Track& tr = snap.getTrack(i);
        if (tr.getKind() == TrackKind::Instrument)
        {
            const bool stripOpen = decision[static_cast<std::size_t>(i)] != SoloTrackAudioDecision::ForcedSilent
                                   && !tr.isTrackOff();
            if (stripOpen && !contentOpen[static_cast<std::size_t>(i)])
            {
                view->suppressOwnTransportClips.push_back(tr.getId());
            }
            continue;
        }
        if (tr.getKind() == TrackKind::Midi)
        {
            const int destIdx = snap.findTrackIndexById(tr.getMidiDestinationTrackId());
            if (destIdx < 0 || snap.getTrack(destIdx).getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const bool destStripOpen =
                decision[static_cast<std::size_t>(destIdx)] != SoloTrackAudioDecision::ForcedSilent
                && !snap.getTrack(destIdx).isTrackOff();
            const bool routeAllowed = isExplicit[static_cast<std::size_t>(i)] != 0
                                      || contentOpen[static_cast<std::size_t>(destIdx)] != 0;
            if (destStripOpen && !routeAllowed)
            {
                view->suppressRoutedMidiSources.push_back(tr.getId());
            }
        }
    }

    view->audioDecisions.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        view->audioDecisions.emplace_back(snap.getTrack(i).getId(), decision[static_cast<std::size_t>(i)]);
    }
    std::sort(view->audioDecisions.begin(), view->audioDecisions.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(view->suppressOwnTransportClips.begin(), view->suppressOwnTransportClips.end());
    std::sort(view->suppressRoutedMidiSources.begin(), view->suppressRoutedMidiSources.end());
    return view;
}

} // namespace solo_mute_view
