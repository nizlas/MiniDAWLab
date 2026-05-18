#include "domain/SessionRouting.h"

#include "domain/SessionSnapshot.h"

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

    [[nodiscard]] bool groupChainRevisitsBeforeMaster(const std::vector<Track>& tracks,
                                                      const TrackId startGroupId,
                                                      const TrackId masterId) noexcept
    {
        std::unordered_set<TrackId> seen;
        TrackId cur = startGroupId;
        for (int guard = 0; guard < static_cast<int>(tracks.size()) + 2; ++guard)
        {
            if (cur == kInvalidTrackId)
            {
                return true;
            }
            if (cur == masterId)
            {
                return false;
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
            if (t.getKind() != TrackKind::Group)
            {
                return t.getKind() != TrackKind::Master;
            }
            cur = t.getRoutedOutputTrackId();
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
                 && groupChainRevisitsBeforeMaster(tracks, t.getId(), masterTrackId))
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
                      dest);
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
                      proposedDestId);
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

} // namespace session_routing
