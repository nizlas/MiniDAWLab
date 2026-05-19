#include "domain/SessionRouting.h"

#include "domain/SessionSnapshot.h"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <vector>

namespace session_routing
{

bool isOutputBusKind(const TrackKind kind) noexcept
{
    return kind == TrackKind::Group || kind == TrackKind::Master;
}

namespace
{
    [[nodiscard]] int findTrackIndexById(const std::vector<Track>& tracks, const TrackId id) noexcept
    {
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            if (tracks[(size_t)i].getId() == id)
            {
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] bool trackIdIsGroupOrMaster(const std::vector<Track>& tracks, const TrackId id) noexcept
    {
        const int ix = findTrackIndexById(tracks, id);
        if (ix < 0)
        {
            return false;
        }
        return isOutputBusKind(tracks[(size_t)ix].getKind());
    }

    /// Walk `startGroupId`'s output chain (first hop = its `routedOutputTrackId`). Returns false when the
    /// chain hits `masterId`, any `TrackKind::Master` row, a cycle, a missing/invalid hop, or a non-bus row.
    [[nodiscard]] bool groupOutputChainReachesMaster(const std::vector<Track>& tracks,
                                                     const TrackId startGroupId,
                                                     const TrackId masterId) noexcept
    {
        const int startIx = findTrackIndexById(tracks, startGroupId);
        if (startIx < 0 || tracks[(size_t)startIx].getKind() != TrackKind::Group)
        {
            return false;
        }

        std::unordered_set<TrackId> seen;
        TrackId cur = tracks[(size_t)startIx].getRoutedOutputTrackId();
        for (int guard = 0; guard < static_cast<int>(tracks.size()) + 2; ++guard)
        {
            if (cur == kInvalidTrackId)
            {
                return false;
            }
            if (cur == masterId)
            {
                return true;
            }
            if (!seen.insert(cur).second)
            {
                return false;
            }
            const int ix = findTrackIndexById(tracks, cur);
            if (ix < 0)
            {
                return false;
            }
            const Track& t = tracks[(size_t)ix];
            if (t.getKind() == TrackKind::Master)
            {
                return true;
            }
            if (t.getKind() != TrackKind::Group)
            {
                return false;
            }
            cur = t.getRoutedOutputTrackId();
        }
        return false;
    }

    [[nodiscard]] bool trackIdIsGroup(const std::vector<Track>& tracks, const TrackId id) noexcept
    {
        const int ix = findTrackIndexById(tracks, id);
        return ix >= 0 && tracks[(size_t)ix].getKind() == TrackKind::Group;
    }

    [[nodiscard]] Track rebuildTrackPreservingFields(const Track& t, std::vector<TrackSend> sends) noexcept
    {
        return Track(t.getId(),
                     t.getName(),
                     t.getPlacedClips(),
                     t.getChannelFaderGain(),
                     t.isTrackOff(),
                     t.isMuted(),
                     t.getKind(),
                     t.getStereoPan(),
                     t.getRoutedOutputTrackId(),
                     std::move(sends));
    }

    void appendCombinedOutgoing(const std::vector<Track>& tracks,
                                const TrackId from,
                                std::vector<TrackId>& out) noexcept
    {
        const int ix = findTrackIndexById(tracks, from);
        if (ix < 0)
        {
            return;
        }
        const Track& t = tracks[(size_t)ix];
        if (t.getKind() == TrackKind::Group)
        {
            const TrackId routeOut = t.getRoutedOutputTrackId();
            if (routeOut != kInvalidTrackId)
            {
                out.push_back(routeOut);
            }
        }
        for (int si = 0; si < t.getNumSends(); ++si)
        {
            const TrackSend& send = t.getSend(si);
            if (send.enabled && send.destTrackId != kInvalidTrackId)
            {
                out.push_back(send.destTrackId);
            }
        }
    }

    [[nodiscard]] bool combinedGraphHasCycle(const std::vector<Track>& tracks) noexcept
    {
        std::unordered_set<TrackId> visited;
        std::unordered_set<TrackId> stack;
        const std::function<bool(TrackId)> dfs = [&](const TrackId u) -> bool {
            if (stack.count(u) > 0)
            {
                return true;
            }
            if (visited.count(u) > 0)
            {
                return false;
            }
            stack.insert(u);
            std::vector<TrackId> next;
            appendCombinedOutgoing(tracks, u, next);
            for (const TrackId v : next)
            {
                if (dfs(v))
                {
                    return true;
                }
            }
            stack.erase(u);
            visited.insert(u);
            return false;
        };
        for (const Track& t : tracks)
        {
            if (dfs(t.getId()))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool combinedGraphReachable(const std::vector<Track>& tracks,
                                              const TrackId start,
                                              const TrackId target) noexcept
    {
        if (start == target)
        {
            return true;
        }
        std::unordered_set<TrackId> seen;
        std::vector<TrackId> stack;
        stack.push_back(start);
        while (!stack.empty())
        {
            const TrackId cur = stack.back();
            stack.pop_back();
            if (cur == target)
            {
                return true;
            }
            if (!seen.insert(cur).second)
            {
                continue;
            }
            std::vector<TrackId> next;
            appendCombinedOutgoing(tracks, cur, next);
            for (const TrackId v : next)
            {
                if (!seen.count(v))
                {
                    stack.push_back(v);
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool canSendFromKind(const TrackKind kind) noexcept
    {
        return kind == TrackKind::Audio || kind == TrackKind::Instrument || kind == TrackKind::Group;
    }

    [[nodiscard]] bool isLegalSendDestinationOnTracks(const std::vector<Track>& tracks,
                                                      const TrackId fromTrackId,
                                                      const TrackId destTrackId) noexcept
    {
        if (fromTrackId == kInvalidTrackId || destTrackId == kInvalidTrackId)
        {
            return false;
        }
        if (fromTrackId == destTrackId)
        {
            return false;
        }
        const int fromIx = findTrackIndexById(tracks, fromTrackId);
        const int destIx = findTrackIndexById(tracks, destTrackId);
        if (fromIx < 0 || destIx < 0)
        {
            return false;
        }
        const Track& fromTr = tracks[(size_t)fromIx];
        const Track& destTr = tracks[(size_t)destIx];
        if (fromTr.getKind() == TrackKind::Master || destTr.getKind() == TrackKind::Master)
        {
            return false;
        }
        if (!canSendFromKind(fromTr.getKind()) || destTr.getKind() != TrackKind::Group)
        {
            return false;
        }
        if (combinedGraphReachable(tracks, destTrackId, fromTrackId))
        {
            return false;
        }
        return true;
    }
} // namespace

void repairRoutingInPlace(std::vector<Track>& tracks, const TrackId masterTrackId) noexcept
{
    if (masterTrackId == kInvalidTrackId)
    {
        return;
    }
    for (Track& t : tracks)
    {
        if (t.getKind() == TrackKind::Master)
        {
            continue;
        }
        TrackId dest = t.getRoutedOutputTrackId();
        if (dest == kInvalidTrackId || dest == t.getId() || !trackIdIsGroupOrMaster(tracks, dest))
        {
            dest = masterTrackId;
        }
        else if (t.getKind() == TrackKind::Group
                 && !groupOutputChainReachesMaster(tracks, t.getId(), masterTrackId))
        {
            dest = masterTrackId;
        }
        if (dest != t.getRoutedOutputTrackId())
        {
            t = Track(t.getId(),
                      t.getName(),
                      t.getPlacedClips(),
                      t.getChannelFaderGain(),
                      t.isTrackOff(),
                      t.isMuted(),
                      t.getKind(),
                      t.getStereoPan(),
                      dest,
                      t.getSends());
        }
    }
}

void repairTrackSendUiSlotsInPlace(std::vector<TrackSend>& sends) noexcept
{
    bool used[kTrackSendInspectorUiSlotCount] {};
    std::vector<TrackSend> out;
    out.reserve(sends.size());
    for (TrackSend s : sends)
    {
        int slot = s.uiSlotIndex;
        if (slot < 0 || slot >= kTrackSendInspectorUiSlotCount || used[slot])
        {
            slot = -1;
            for (int cand = 0; cand < kTrackSendInspectorUiSlotCount; ++cand)
            {
                if (!used[cand])
                {
                    slot = cand;
                    break;
                }
            }
            if (slot < 0)
            {
                continue;
            }
        }
        s.uiSlotIndex = slot;
        used[slot] = true;
        out.push_back(s);
    }
    sends.swap(out);
}

void repairSendsInPlace(std::vector<Track>& tracks, const TrackId masterTrackId) noexcept
{
    juce::ignoreUnused(masterTrackId);
    for (Track& t : tracks)
    {
        if (t.getKind() == TrackKind::Master)
        {
            if (t.getNumSends() > 0)
            {
                t = rebuildTrackPreservingFields(t, {});
            }
            continue;
        }
        std::vector<TrackSend> sends = t.getSends();
        sends.erase(std::remove_if(sends.begin(),
                                   sends.end(),
                                   [&](const TrackSend& s) {
                                       if (s.destTrackId == kInvalidTrackId || s.destTrackId == t.getId())
                                       {
                                           return true;
                                       }
                                       return !trackIdIsGroup(tracks, s.destTrackId);
                                   }),
                    sends.end());
        for (TrackSend& s : sends)
        {
            s.amountLinear = clampTrackSendAmountLinear(s.amountLinear);
        }
        repairTrackSendUiSlotsInPlace(sends);
        t = rebuildTrackPreservingFields(t, std::move(sends));
    }

    for (;;)
    {
        if (!combinedGraphHasCycle(tracks))
        {
            break;
        }
        bool disabledOne = false;
        for (Track& t : tracks)
        {
            std::vector<TrackSend> sends = t.getSends();
            for (size_t si = 0; si < sends.size(); ++si)
            {
                if (!sends[si].enabled)
                {
                    continue;
                }
                sends[si].enabled = false;
                std::vector<Track> trial = tracks;
                for (Track& tt : trial)
                {
                    if (tt.getId() == t.getId())
                    {
                        tt = rebuildTrackPreservingFields(tt, sends);
                        break;
                    }
                }
                if (!combinedGraphHasCycle(trial))
                {
                    t = rebuildTrackPreservingFields(t, sends);
                    disabledOne = true;
                    break;
                }
                sends[si].enabled = true;
            }
            if (disabledOne)
            {
                break;
            }
        }
        if (!disabledOne)
        {
            break;
        }
    }
}

std::vector<TrackId> legalOutputDestinations(const SessionSnapshot& snap, const TrackId fromTrackId) noexcept
{
    std::vector<TrackId> out;
    const TrackId masterId = snap.findCanonicalMasterTrackId();
    if (masterId == kInvalidTrackId || fromTrackId == kInvalidTrackId)
    {
        return out;
    }
    const int fromIx = snap.findTrackIndexById(fromTrackId);
    if (fromIx < 0)
    {
        return out;
    }
    if (snap.getTrack(fromIx).getKind() == TrackKind::Master)
    {
        return out;
    }

    if (isLegalRoutedOutputTarget(snap, fromTrackId, masterId))
    {
        out.push_back(masterId);
    }
    for (int i = 0; i < snap.getNumTracks(); ++i)
    {
        const Track& t = snap.getTrack(i);
        if (t.getKind() != TrackKind::Group)
        {
            continue;
        }
        if (t.getId() == fromTrackId)
        {
            continue;
        }
        if (isLegalRoutedOutputTarget(snap, fromTrackId, t.getId()))
        {
            out.push_back(t.getId());
        }
    }
    return out;
}

bool wouldCreateRoutingCycle(const SessionSnapshot& snap,
                             const TrackId fromTrackId,
                             const TrackId proposedDestId) noexcept
{
    if (fromTrackId == kInvalidTrackId || proposedDestId == kInvalidTrackId)
    {
        return true;
    }
    if (fromTrackId == proposedDestId)
    {
        return true;
    }
    const int fromIx = snap.findTrackIndexById(fromTrackId);
    const int destIx = snap.findTrackIndexById(proposedDestId);
    if (fromIx < 0 || destIx < 0)
    {
        return true;
    }
    const Track& fromTr = snap.getTrack(fromIx);
    const Track& destTr = snap.getTrack(destIx);
    if (fromTr.getKind() == TrackKind::Master)
    {
        return true;
    }
    if (!isOutputBusKind(destTr.getKind()))
    {
        return true;
    }
    if (fromTr.getKind() != TrackKind::Group)
    {
        return false;
    }
    const TrackId masterId = snap.findCanonicalMasterTrackId();
    std::vector<Track> tracks;
    tracks.reserve((size_t)snap.getNumTracks());
    for (int i = 0; i < snap.getNumTracks(); ++i)
    {
        tracks.push_back(snap.getTrack(i));
    }
    // Temporarily model F -> proposedDest and walk from proposedDest; cycle if we reach F before Master.
    for (Track& t : tracks)
    {
        if (t.getId() == fromTrackId)
        {
            t = Track(t.getId(),
                      t.getName(),
                      t.getPlacedClips(),
                      t.getChannelFaderGain(),
                      t.isTrackOff(),
                      t.isMuted(),
                      t.getKind(),
                      t.getStereoPan(),
                      proposedDestId,
                      t.getSends());
            break;
        }
    }
    std::unordered_set<TrackId> seen;
    TrackId cur = proposedDestId;
    for (int guard = 0; guard < static_cast<int>(tracks.size()) + 2; ++guard)
    {
        if (cur == masterId)
        {
            return false;
        }
        if (cur == fromTrackId)
        {
            return true;
        }
        if (cur == kInvalidTrackId)
        {
            return true;
        }
        if (!seen.insert(cur).second)
        {
            return true;
        }
        const int ix = findTrackIndexById(tracks, cur);
        if (ix < 0)
        {
            return true;
        }
        const Track& t = tracks[(size_t)ix];
        if (t.getKind() == TrackKind::Master)
        {
            return false;
        }
        if (t.getKind() != TrackKind::Group)
        {
            return true;
        }
        cur = t.getRoutedOutputTrackId();
    }
    return true;
}

bool isLegalRoutedOutputTarget(const SessionSnapshot& snap,
                               const TrackId fromTrackId,
                               const TrackId destTrackId) noexcept
{
    return !wouldCreateRoutingCycle(snap, fromTrackId, destTrackId);
}

bool isLegalSendDestinationForTrackList(const std::vector<Track>& tracks,
                                        const TrackId fromTrackId,
                                        const TrackId destTrackId) noexcept
{
    return isLegalSendDestinationOnTracks(tracks, fromTrackId, destTrackId);
}

bool isLegalSendDestination(const SessionSnapshot& snap,
                            const TrackId fromTrackId,
                            const TrackId destTrackId) noexcept
{
    std::vector<Track> tracks;
    tracks.reserve((size_t)snap.getNumTracks());
    for (int i = 0; i < snap.getNumTracks(); ++i)
    {
        tracks.push_back(snap.getTrack(i));
    }
    return isLegalSendDestinationOnTracks(tracks, fromTrackId, destTrackId);
}

std::vector<TrackId> legalSendDestinations(const SessionSnapshot& snap, const TrackId fromTrackId) noexcept
{
    std::vector<TrackId> out;
    const int fromIx = snap.findTrackIndexById(fromTrackId);
    if (fromIx < 0)
    {
        return out;
    }
    const Track& fromTr = snap.getTrack(fromIx);
    if (fromTr.getKind() == TrackKind::Master)
    {
        return out;
    }
    if (fromTr.getKind() != TrackKind::Audio && fromTr.getKind() != TrackKind::Instrument
        && fromTr.getKind() != TrackKind::Group)
    {
        return out;
    }
    for (int i = 0; i < snap.getNumTracks(); ++i)
    {
        const Track& t = snap.getTrack(i);
        if (t.getKind() != TrackKind::Group || t.getId() == fromTrackId)
        {
            continue;
        }
        if (isLegalSendDestination(snap, fromTrackId, t.getId()))
        {
            out.push_back(t.getId());
        }
    }
    return out;
}

} // namespace session_routing
