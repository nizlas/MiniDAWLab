// =============================================================================
// SessionSnapshot.cpp  —  immutable session state published to the audio thread
// =============================================================================
//
// ROLE
//   Build **new** const snapshots on the message thread; the audio path acquire-loads and reads
//   tracks and placements only. Multi-track: each factory leaves `Track` as the unit of
//   *local* front-to-back order; the committed-move rule never compares clips on different tracks.
//   `withClipMovedToTrack` is the one-shot structural move: one row, source ≠ target, insert at 0
//   on the destination track (identity preserved). `withTrackReordered` reorders whole `Track` rows
//   (ids, names, per-track clip lists unchanged).
// =============================================================================

#include "domain/SessionSnapshot.h"

#include "domain/AudioClip.h"
#include "domain/ProjectMusicalTime.h"
#include "domain/SessionRouting.h"
#include "domain/TrackStereoPan.h"

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>
#include <limits>
#include <unordered_set>

namespace
{
    std::int64_t derivedTimelineEndFromTracks(const std::vector<Track>& tracks) noexcept
    {
        std::int64_t maxEnd = 0;
        for (const Track& t : tracks)
        {
            for (int i = 0; i < t.getNumPlacedClips(); ++i)
            {
                const PlacedClip& p = t.getPlacedClip(i);
                const std::int64_t end = p.getStartSample() + p.getEffectiveLengthSamples();
                if (end > maxEnd)
                    maxEnd = end;
            }
        }
        return maxEnd;
    }

    bool rangesOverlapHalfOpen(
        const std::int64_t a0, const std::int64_t a1, const std::int64_t b0, const std::int64_t b1)
    {
        return a0 < b1 && b0 < a1;
    }

    /// One slice rule: WAV timeline clips attach only to `TrackKind::Audio` lanes. Experimental
    /// instrument shells live in-session for ordering/until real multi-instrument work; they carry
    /// no `PlacedClip` audio material in this architecture. `Master` is the final output bus only.
    [[nodiscard]] bool trackAcceptsTimelineAudioClipMaterial(const Track& t) noexcept
    {
        return t.getKind() == TrackKind::Audio;
    }

    [[nodiscard]] TrackId maxTrackIdInList(const std::vector<Track>& tracks) noexcept
    {
        TrackId maxId = 0;
        for (const Track& t : tracks)
        {
            maxId = juce::jmax(maxId, t.getId());
        }
        return maxId;
    }

    [[nodiscard]] Track makeDefaultMasterTrack(const TrackId masterId) noexcept
    {
        return Track(masterId,
                     juce::String(kMasterTrackDisplayName),
                     std::vector<PlacedClip>{},
                     kTrackChannelVolumeUnityGain,
                     false,
                     false,
                     TrackKind::Master);
    }

    [[nodiscard]] Track normalizedCanonicalMasterRow(const Track& source) noexcept
    {
        return Track(source.getId(),
                     juce::String(kMasterTrackDisplayName),
                     std::vector<PlacedClip>{},
                     source.getChannelFaderGain(),
                     false,
                     source.isMuted(),
                     TrackKind::Master,
                     source.getStereoPan());
    }

    [[nodiscard]] bool trackNameIsCanonicalMaster(const juce::String& name) noexcept
    {
        return name.equalsIgnoreCase(kMasterTrackDisplayName);
    }

    [[nodiscard]] TrackKind demotedKindForFormerMaster(
        const TrackId id, const std::unordered_set<TrackId>* instrumentLaneIds) noexcept
    {
        if (instrumentLaneIds != nullptr && instrumentLaneIds->count(id) > 0)
        {
            return TrackKind::Instrument;
        }
        return TrackKind::Audio;
    }

    [[nodiscard]] int chooseCanonicalMasterTrackIndex(
        const std::vector<Track>& tracks, const std::vector<int>& masterIndices) noexcept
    {
        for (const int idx : masterIndices)
        {
            if (trackNameIsCanonicalMaster(tracks[(size_t)idx].getName()))
            {
                return idx;
            }
        }
        return masterIndices.back();
    }

    // Same end-state policy as the old single-lane `withClipMoved`, applied to **one** lane’s clip
    // vector. Unknown id → jassert, return a copy of `tracks` (caller passes a deep copy to mutate).
    std::vector<PlacedClip> moveOneClipInLane(
        const std::vector<PlacedClip>& laneClips,
        const PlacedClipId movedId,
        const std::int64_t newStartSampleOnTimeline,
        bool* outFound)
    {
        *outFound = false;
        std::vector<PlacedClip> clips = laneClips;
        if (movedId == kInvalidPlacedClipId)
        {
            jassert(false);
            return clips;
        }
        int movedIndex = -1;
        for (int i = 0; i < (int)clips.size(); ++i)
        {
            if (clips[(size_t)i].getId() == movedId)
            {
                movedIndex = i;
                break;
            }
        }
        if (movedIndex < 0)
        {
            // `movedId` lives in a **different** track; this lane is unchanged.
            *outFound = false;
            return clips;
        }
        *outFound = true;
        const std::int64_t clampedStart = juce::jmax(std::int64_t{0}, newStartSampleOnTimeline);
        {
            const PlacedClip& oldM = clips[(size_t)movedIndex];
            clips[(size_t)movedIndex] = oldM.withStartSampleOnTimeline(clampedStart);
        }
        const PlacedClip& m = clips[(size_t)movedIndex];
        const std::int64_t m0 = m.getStartSample();
        const std::int64_t m1 = m0 + m.getEffectiveLengthSamples();
        bool overlapWithAnyOther = false;
        for (int j = 0; j < (int)clips.size(); ++j)
        {
            if (j == movedIndex)
            {
                continue;
            }
            const PlacedClip& o = clips[(size_t)j];
            const std::int64_t o0 = o.getStartSample();
            const std::int64_t o1 = o0 + o.getEffectiveLengthSamples();
            if (rangesOverlapHalfOpen(m0, m1, o0, o1))
            {
                overlapWithAnyOther = true;
                break;
            }
        }
        if (!overlapWithAnyOther)
        {
            PlacedClip promoted = std::move(clips[(size_t)movedIndex]);
            clips.erase(clips.begin() + movedIndex);
            clips.insert(clips.begin(), std::move(promoted));
        }
        return clips;
    }

    [[nodiscard]] Track duplicateTrackSameClips(const Track& t)
    {
        return Track(t.getId(),
                     t.getName(),
                     t.getPlacedClips(),
                     t.getChannelFaderGain(),
                     t.isTrackOff(),
                     t.isMuted(),
                     t.getKind(),
                     t.getStereoPan(),
                     t.getRoutedOutputTrackId());
    }

    [[nodiscard]] Track duplicateTrackWithMovedClips(const Track& t, std::vector<PlacedClip>&& clips)
    {
        return Track(t.getId(),
                     t.getName(),
                     std::move(clips),
                     t.getChannelFaderGain(),
                     t.isTrackOff(),
                     t.isMuted(),
                     t.getKind(),
                     t.getStereoPan(),
                     t.getRoutedOutputTrackId());
    }

    [[nodiscard]] Track duplicateTrackSameClipsWithGain(const Track& t, const float linearGain)
    {
        const float g = juce::jlimit(0.0f, kTrackChannelFaderGainMax, linearGain);
        return Track(t.getId(),
                     t.getName(),
                     t.getPlacedClips(),
                     g,
                     t.isTrackOff(),
                     t.isMuted(),
                     t.getKind(),
                     t.getStereoPan(),
                     t.getRoutedOutputTrackId());
    }

    [[nodiscard]] TrackId findLastMasterTrackId(const std::vector<Track>& tracks) noexcept
    {
        for (int i = static_cast<int>(tracks.size()) - 1; i >= 0; --i)
        {
            if (tracks[(size_t)i].getKind() == TrackKind::Master)
            {
                return tracks[(size_t)i].getId();
            }
        }
        return kInvalidTrackId;
    }
} // namespace

SessionSnapshot::SessionSnapshot(std::vector<Track> tracks,
                                 const std::int64_t arrangementExtentSamples,
                                 const std::int64_t leftLocatorSamples,
                                 const std::int64_t rightLocatorSamples,
                                 ProjectMusicalTime projectMusicalTime) noexcept
    : tracks_(std::move(tracks))
    , arrangementExtentSamples_(juce::jmax(std::int64_t{0}, arrangementExtentSamples))
    , leftLocatorSamples_(juce::jmax(std::int64_t{0}, leftLocatorSamples))
    , rightLocatorSamples_(juce::jmax(std::int64_t{0}, rightLocatorSamples))
    , projectMusicalTime_(sanitizeProjectMusicalTime(projectMusicalTime))
{
}

void SessionSnapshot::ensureMasterTrackInvariant(
    std::vector<Track>& tracks,
    const TrackId allocateMasterId,
    const std::unordered_set<TrackId>* instrumentLaneIds) noexcept
{
    std::vector<int> masterIndices;
    masterIndices.reserve(2U);
    for (int i = 0; i < (int)tracks.size(); ++i)
    {
        if (tracks[(size_t)i].getKind() == TrackKind::Master)
        {
            masterIndices.push_back(i);
        }
    }

    if (masterIndices.empty())
    {
        const TrackId mid = (allocateMasterId != kInvalidTrackId)
                                ? allocateMasterId
                                : (maxTrackIdInList(tracks) + 1);
        tracks.push_back(makeDefaultMasterTrack(mid));
        return;
    }

    if (masterIndices.size() == 1U)
    {
        const int onlyIdx = masterIndices[0];
        const Track& only = tracks[(size_t)onlyIdx];
        if (instrumentLaneIds != nullptr && instrumentLaneIds->count(only.getId()) > 0)
        {
            const Track& o = only;
            tracks[(size_t)onlyIdx]
                = Track(o.getId(),
                        o.getName(),
                        o.getPlacedClips(),
                        o.getChannelFaderGain(),
                        o.isTrackOff(),
                        o.isMuted(),
                        TrackKind::Instrument,
                        o.getStereoPan(),
                        o.getRoutedOutputTrackId());
            const TrackId mid = (allocateMasterId != kInvalidTrackId)
                                    ? allocateMasterId
                                    : (maxTrackIdInList(tracks) + 1);
            tracks.push_back(makeDefaultMasterTrack(mid));
            return;
        }
    }

    const int keepIdx = chooseCanonicalMasterTrackIndex(tracks, masterIndices);
    const TrackId keptMasterId = tracks[(size_t)keepIdx].getId();

    for (int i = (int)masterIndices.size() - 1; i >= 0; --i)
    {
        const int idx = masterIndices[(size_t)i];
        if (idx == keepIdx)
        {
            continue;
        }
        const Track& dropped = tracks[(size_t)idx];
        const TrackKind demoted = demotedKindForFormerMaster(dropped.getId(), instrumentLaneIds);
        tracks[(size_t)idx] = Track(dropped.getId(),
                                    dropped.getName(),
                                    dropped.getPlacedClips(),
                                    dropped.getChannelFaderGain(),
                                    dropped.isTrackOff(),
                                    dropped.isMuted(),
                                    demoted,
                                    dropped.getStereoPan(),
                                    keptMasterId);
    }

    Track master = normalizedCanonicalMasterRow(tracks[(size_t)keepIdx]);
    tracks.erase(tracks.begin() + keepIdx);
    tracks.push_back(std::move(master));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::ensuringMasterInvariant(
    const SessionSnapshot& previous) noexcept
{
    if (previous.isEmpty())
    {
        return withSingleEmptyTrack(TrackId{1}, juce::String("Track 1"));
    }
    std::vector<Track> tracks;
    tracks.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        tracks.push_back(duplicateTrackSameClips(previous.getTrack(i)));
    }
    return withTracks(std::move(tracks),
                      previous.getStoredArrangementExtentSamples(),
                      previous.getLeftLocatorSamples(),
                      previous.getRightLocatorSamples(),
                      previous.getProjectMusicalTime());
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::createEmpty() noexcept
{
    static const auto empty
        = std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::vector<Track>{}, 0, 0, 0});
    return empty;
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withSingleEmptyTrack(
    const TrackId trackId, juce::String trackName) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        jassert(false);
        return createEmpty();
    }
    std::vector<Track> v;
    const TrackId masterId = trackId + 1;
    v.emplace_back(trackId,
                   std::move(trackName),
                   std::vector<PlacedClip>{},
                   kTrackChannelVolumeUnityGain,
                   false,
                   false,
                   TrackKind::Audio,
                   kTrackStereoPanCenter,
                   masterId);
    v.push_back(makeDefaultMasterTrack(masterId));
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v), 0, 0, 0});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTracks(std::vector<Track> tracks) noexcept
{
    return withTracks(std::move(tracks), 0);
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTracks(
    std::vector<Track> tracks, const std::int64_t arrangementExtentSamples) noexcept
{
    return withTracks(std::move(tracks), arrangementExtentSamples, 0, 0);
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTracks(
    std::vector<Track> tracks,
    const std::int64_t arrangementExtentSamples,
    const std::int64_t leftLocatorSamples,
    const std::int64_t rightLocatorSamples,
    ProjectMusicalTime projectMusicalTime,
    const std::unordered_set<TrackId>* instrumentLaneIdsForMasterRepair) noexcept
{
    if (tracks.empty())
    {
        jassert(false);
        return withSingleEmptyTrack(TrackId{1}, juce::String("Track 1"));
    }
    ensureMasterTrackInvariant(tracks, kInvalidTrackId, instrumentLaneIdsForMasterRepair);
    session_routing::repairRoutingInPlace(tracks, findLastMasterTrackId(tracks));
    const std::int64_t derived = derivedTimelineEndFromTracks(tracks);
    const std::int64_t extentEffective
        = juce::jmax(arrangementExtentSamples, derived);
    const std::int64_t lClamp = juce::jlimit(std::int64_t{0}, extentEffective, leftLocatorSamples);
    const std::int64_t rClamp = juce::jlimit(std::int64_t{0}, extentEffective, rightLocatorSamples);
    return std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::move(tracks),
                            arrangementExtentSamples,
                            lClamp,
                            rClamp,
                            projectMusicalTime));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withSinglePlacedClip(
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const PlacedClipId newClipId) noexcept
{
    if (material == nullptr)
    {
        return createEmpty();
    }
    if (newClipId == kInvalidPlacedClipId)
    {
        jassert(false);
        return createEmpty();
    }
    std::vector<PlacedClip> lane;
    lane.emplace_back(newClipId, std::move(material), startSampleOnTimeline);
    std::vector<Track> v;
    v.emplace_back(TrackId{1}, juce::String("Track 1"), std::move(lane));
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v), 0, 0, 0});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withArrangementExtent(
    const SessionSnapshot& previous, const std::int64_t newArrangementExtentSamples) noexcept
{
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot(
        previous.tracks_,
        newArrangementExtentSamples,
        previous.getLeftLocatorSamples(),
        previous.getRightLocatorSamples(),
        previous.getProjectMusicalTime()));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withLocators(
    const SessionSnapshot& previous,
    const std::int64_t leftLocatorSamples,
    const std::int64_t rightLocatorSamples) noexcept
{
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot(
        previous.tracks_,
        previous.getStoredArrangementExtentSamples(),
        leftLocatorSamples,
        rightLocatorSamples,
        previous.getProjectMusicalTime()));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withMusicalTime(
    const SessionSnapshot& previous,
    ProjectMusicalTime musicalTime) noexcept
{
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot(
        previous.tracks_,
        previous.getStoredArrangementExtentSamples(),
        previous.getLeftLocatorSamples(),
        previous.getRightLocatorSamples(),
        musicalTime));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
    const SessionSnapshot& previous,
    const PlacedClipId newClipId,
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const TrackId targetTrackId) noexcept
{
    if (material == nullptr)
    {
        return createEmpty();
    }
    if (newClipId == kInvalidPlacedClipId || targetTrackId == kInvalidTrackId)
    {
        jassert(false);
        return createEmpty();
    }
    // No tracks yet: one lane = target id with the new row only.
    if (previous.getNumTracks() == 0)
    {
        std::vector<PlacedClip> lane;
        lane.emplace_back(newClipId, std::move(material), startSampleOnTimeline);
        std::vector<Track> v;
        v.emplace_back(
            targetTrackId,
            juce::String("Track ") + juce::String(targetTrackId),
            std::move(lane));
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v), 0, 0, 0});
    }
    const int tIdx = previous.findTrackIndexById(targetTrackId);
    if (tIdx < 0)
    {
        jassert(false);
        // Defensive: copy as no-op
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (!trackAcceptsTimelineAudioClipMaterial(previous.getTrack(tIdx)))
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        if (i != tIdx)
        {
            const Track& t = previous.getTrack(i);
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            const Track& t = previous.getTrack(i);
            const std::vector<PlacedClip>& oldC = t.getPlacedClips();
            std::vector<PlacedClip> v;
            v.reserve(oldC.size() + 1U);
            v.emplace_back(newClipId, std::move(material), startSampleOnTimeline);
            for (const PlacedClip& p : oldC)
            {
                v.push_back(p);
            }
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

namespace
{
constexpr std::int64_t kUnsetMaterialWindowEndExclusiveSend()
{
    return std::numeric_limits<std::int64_t>::min();
}
} // namespace

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
    const SessionSnapshot& previous,
    const PlacedClipId newClipId,
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const TrackId targetTrackId,
    const std::int64_t leftTrimSamples,
    const std::int64_t visibleLengthSamples) noexcept
{
    return withClipAddedAsNewestOnTargetTrack(previous,
                                                  newClipId,
                                                  std::move(material),
                                                  startSampleOnTimeline,
                                                  targetTrackId,
                                                  leftTrimSamples,
                                                  visibleLengthSamples,
                                                  std::int64_t{ 0 },
                                                  kUnsetMaterialWindowEndExclusiveSend());
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
    const SessionSnapshot& previous,
    const PlacedClipId newClipId,
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const TrackId targetTrackId,
    const std::int64_t leftTrimSamples,
    const std::int64_t visibleLengthSamples,
    const std::int64_t materialWindowStartSamples,
    const std::int64_t materialWindowEndExclusiveSamples) noexcept
{
    if (material == nullptr)
    {
        return createEmpty();
    }
    if (newClipId == kInvalidPlacedClipId || targetTrackId == kInvalidTrackId)
    {
        jassert(false);
        return createEmpty();
    }
    if (previous.getNumTracks() == 0)
    {
        std::vector<PlacedClip> lane;
        lane.emplace_back(newClipId,
                            std::move(material),
                            startSampleOnTimeline,
                            leftTrimSamples,
                            visibleLengthSamples,
                            materialWindowStartSamples,
                            materialWindowEndExclusiveSamples);
        std::vector<Track> v;
        v.emplace_back(
            targetTrackId,
            juce::String("Track ") + juce::String(targetTrackId),
            std::move(lane));
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{std::move(v), 0, 0, 0});
    }
    const int tIdx = previous.findTrackIndexById(targetTrackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (!trackAcceptsTimelineAudioClipMaterial(previous.getTrack(tIdx)))
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        if (i != tIdx)
        {
            const Track& t = previous.getTrack(i);
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            const Track& t = previous.getTrack(i);
            const std::vector<PlacedClip>& oldC = t.getPlacedClips();
            std::vector<PlacedClip> v;
            v.reserve(oldC.size() + 1U);
            v.emplace_back(newClipId,
                             std::move(material),
                             startSampleOnTimeline,
                             leftTrimSamples,
                             visibleLengthSamples,
                             materialWindowStartSamples,
                             materialWindowEndExclusiveSamples);
            for (const PlacedClip& p : oldC)
            {
                v.push_back(p);
            }
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackAdded(
    const SessionSnapshot& previous,
    const TrackId newTrackId,
    juce::String newTrackName) noexcept
{
    return withTrackAdded(previous, newTrackId, std::move(newTrackName), TrackKind::Audio);
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackAdded(
    const SessionSnapshot& previous,
    const TrackId newTrackId,
    juce::String newTrackName,
    TrackKind kind) noexcept
{
    if (newTrackId == kInvalidTrackId || kind == TrackKind::Master)
    {
        jassert(false);
        return createEmpty();
    }
    if (previous.findTrackIndexById(newTrackId) >= 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks() + 1U);
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (t.getKind() == TrackKind::Master)
        {
            continue;
        }
        out.push_back(duplicateTrackSameClips(t));
    }
    const TrackId masterId = previous.findCanonicalMasterTrackId();
    const Track newRow(newTrackId,
                       std::move(newTrackName),
                       std::vector<PlacedClip>{},
                       kTrackChannelVolumeUnityGain,
                       false,
                       false,
                       kind,
                       kTrackStereoPanCenter,
                       masterId);
    out.push_back(newRow);
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (t.getKind() == TrackKind::Master)
        {
            out.push_back(duplicateTrackSameClips(t));
            break;
        }
    }
    ensureMasterTrackInvariant(out, kInvalidTrackId, nullptr);
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackRemoved(
    const SessionSnapshot& previous,
    const TrackId removedTrackId) noexcept
{
    if (removedTrackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const TrackId masterId = previous.findCanonicalMasterTrackId();
    bool found = false;
    bool removedWasGroup = false;
    std::vector<Track> out;
    out.reserve(static_cast<size_t>(juce::jmax(0, previous.getNumTracks() - 1)));
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (t.getId() == removedTrackId)
        {
            if (t.getKind() == TrackKind::Master)
            {
                return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
                    previous.tracks_, previous.arrangementExtentSamples_,
                    previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(),
                    previous.getProjectMusicalTime()});
            }
            removedWasGroup = (t.getKind() == TrackKind::Group);
            found = true;
            continue;
        }
        if (removedWasGroup && masterId != kInvalidTrackId && t.getRoutedOutputTrackId() == removedTrackId)
        {
            out.push_back(Track(t.getId(),
                                t.getName(),
                                t.getPlacedClips(),
                                t.getChannelFaderGain(),
                                t.isTrackOff(),
                                t.isMuted(),
                                t.getKind(),
                                t.getStereoPan(),
                                masterId));
        }
        else
        {
            out.push_back(duplicateTrackSameClips(t));
        }
    }
    if (!found)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (out.empty())
    {
        return withSingleEmptyTrack(TrackId{1}, juce::String("Track 1"));
    }
    ensureMasterTrackInvariant(out, kInvalidTrackId, nullptr);
    session_routing::repairRoutingInPlace(out, findLastMasterTrackId(out));
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out),
        previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(),
            previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withPlacedClipRemoved(
    const SessionSnapshot& previous,
    const TrackId trackId,
    const PlacedClipId placedClipId) noexcept
{
    if (trackId == kInvalidTrackId || placedClipId == kInvalidPlacedClipId)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const Track& touched = previous.getTrack(tIdx);
    std::vector<PlacedClip> newClips;
    newClips.reserve(touched.getPlacedClips().size());
    bool removedAny = false;
    for (const PlacedClip& p : touched.getPlacedClips())
    {
        if (p.getId() == placedClipId)
        {
            removedAny = true;
            continue;
        }
        newClips.push_back(p);
    }
    if (!removedAny)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
            continue;
        }
        out.push_back(duplicateTrackWithMovedClips(t, std::move(newClips)));
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipMoved(
    const SessionSnapshot& previous,
    const PlacedClipId movedId,
    const std::int64_t newStartSampleOnTimeline) noexcept
{
    if (movedId == kInvalidPlacedClipId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    bool anyFound = false;
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        bool found = false;
        std::vector<PlacedClip> u
            = moveOneClipInLane(t.getPlacedClips(), movedId, newStartSampleOnTimeline, &found);
        if (found)
        {
            anyFound = true;
        }
        out.push_back(duplicateTrackWithMovedClips(t, std::move(u)));
    }
    if (!anyFound)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipMovedToTrack(
    const SessionSnapshot& previous,
    const PlacedClipId movedId,
    const std::int64_t newStartSampleOnTimeline,
    const TrackId targetTrackId) noexcept
{
    if (movedId == kInvalidPlacedClipId || targetTrackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int targetIdx = previous.findTrackIndexById(targetTrackId);
    if (targetIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (!trackAcceptsTimelineAudioClipMaterial(previous.getTrack(targetIdx)))
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    int sourceIdx = -1;
    int sourceRow = -1;
    for (int t = 0; t < previous.getNumTracks(); ++t)
    {
        const Track& tr = previous.getTrack(t);
        for (int i = 0; i < tr.getNumPlacedClips(); ++i)
        {
            if (tr.getPlacedClip(i).getId() == movedId)
            {
                sourceIdx = t;
                sourceRow = i;
                break;
            }
        }
        if (sourceIdx >= 0)
        {
            break;
        }
    }
    if (sourceIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (sourceIdx == targetIdx)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const PlacedClip& raw = previous.getTrack(sourceIdx).getPlacedClip(sourceRow);
    const std::int64_t clamped
        = juce::jmax(std::int64_t{0}, newStartSampleOnTimeline);
    PlacedClip moved = raw.withStartSampleOnTimeline(clamped);

    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        if (i == sourceIdx)
        {
            const Track& t = previous.getTrack(i);
            std::vector<PlacedClip> v = t.getPlacedClips();
            v.erase(v.begin() + sourceRow);
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
        else if (i == targetIdx)
        {
            const Track& t = previous.getTrack(i);
            const std::vector<PlacedClip>& oldC = t.getPlacedClips();
            std::vector<PlacedClip> v;
            v.reserve(oldC.size() + 1U);
            v.push_back(std::move(moved));
            for (const PlacedClip& p : oldC)
            {
                v.push_back(p);
            }
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
        else
        {
            const Track& t = previous.getTrack(i);
            out.push_back(duplicateTrackSameClips(t));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackReordered(
    const SessionSnapshot& previous,
    const TrackId movedTrackId,
    const int destIndex) noexcept
{
    const int n = previous.getNumTracks();
    if (movedTrackId == kInvalidTrackId || n <= 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (destIndex < 0 || destIndex >= n)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int s = previous.findTrackIndexById(movedTrackId);
    if (s < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (s == destIndex)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> v;
    v.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        const Track& t = previous.getTrack(i);
        v.push_back(duplicateTrackSameClips(t));
    }
    const Track moved = v[(size_t)s];
    v.erase(v.begin() + s);
    v.insert(v.begin() + destIndex, moved);
    ensureMasterTrackInvariant(v, kInvalidTrackId, nullptr);
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(v), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipRightEdgeTrimmed(
    const SessionSnapshot& previous,
    const PlacedClipId id,
    const std::int64_t newVisibleLengthSamples) noexcept
{
    if (id == kInvalidPlacedClipId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{previous.tracks_,
                                                                         previous.arrangementExtentSamples_,
                                                                         previous.getLeftLocatorSamples(),
                                                                         previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    bool any = false;
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int ti = 0; ti < previous.getNumTracks(); ++ti)
    {
        const Track& t = previous.getTrack(ti);
        const std::vector<PlacedClip>& oldC = t.getPlacedClips();
        std::vector<PlacedClip> v;
        v.reserve(oldC.size());
        bool rowChanged = false;
        for (const PlacedClip& p : oldC)
        {
            if (p.getId() == id)
            {
                v.push_back(p.withRightEdgeVisibleLength(newVisibleLengthSamples));
                any = true;
                rowChanged = true;
            }
            else
            {
                v.push_back(p);
            }
        }
        if (rowChanged)
        {
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
        else
        {
            out.push_back(duplicateTrackSameClips(t));
        }
    }
    if (!any)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{previous.tracks_,
                                                                         previous.arrangementExtentSamples_,
                                                                         previous.getLeftLocatorSamples(),
                                                                         previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipLeftEdgeTrimmed(
    const SessionSnapshot& previous,
    const PlacedClipId id,
    const std::int64_t newLeftTrimSamples) noexcept
{
    if (id == kInvalidPlacedClipId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{previous.tracks_,
                                                                         previous.arrangementExtentSamples_,
                                                                         previous.getLeftLocatorSamples(),
                                                                         previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    bool any = false;
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int ti = 0; ti < previous.getNumTracks(); ++ti)
    {
        const Track& t = previous.getTrack(ti);
        const std::vector<PlacedClip>& oldC = t.getPlacedClips();
        std::vector<PlacedClip> v;
        v.reserve(oldC.size());
        bool rowChanged = false;
        for (const PlacedClip& p : oldC)
        {
            if (p.getId() == id)
            {
                v.push_back(p.withLeftEdgeTrim(newLeftTrimSamples));
                any = true;
                rowChanged = true;
            }
            else
            {
                v.push_back(p);
            }
        }
        if (rowChanged)
        {
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
        else
        {
            out.push_back(duplicateTrackSameClips(t));
        }
    }
    if (!any)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{previous.tracks_,
                                                                         previous.arrangementExtentSamples_,
                                                                         previous.getLeftLocatorSamples(),
                                                                         previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withClipSplit(
    const SessionSnapshot& previous,
    const PlacedClipId targetId,
    const std::int64_t splitSampleOnTimeline,
    const PlacedClipId leftNewId,
    const PlacedClipId rightNewId) noexcept
{
    if (targetId == kInvalidPlacedClipId || leftNewId == kInvalidPlacedClipId
        || rightNewId == kInvalidPlacedClipId || leftNewId == rightNewId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    bool any = false;
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int ti = 0; ti < previous.getNumTracks(); ++ti)
    {
        const Track& t = previous.getTrack(ti);
        const std::vector<PlacedClip>& oldC = t.getPlacedClips();
        std::vector<PlacedClip> v;
        v.reserve(oldC.size() + 1);
        bool rowChanged = false;
        for (const PlacedClip& orig : oldC)
        {
            if (orig.getId() != targetId)
            {
                v.push_back(orig);
                continue;
            }
            const std::int64_t S = orig.getStartSample();
            const std::int64_t V = orig.getEffectiveLengthSamples();
            const std::int64_t L = orig.getLeftTrimSamples();
            const std::int64_t splitT = splitSampleOnTimeline;
            if (!(splitT > S && splitT < S + V))
            {
                jassert(false);
                v.push_back(orig);
                continue;
            }
            const std::int64_t vLeft = splitT - S;
            const std::int64_t vRight = (S + V) - splitT;
            std::shared_ptr<const AudioClip> mat = orig.getMaterial();
            const std::int64_t ws = orig.getMaterialWindowStartSamples();
            const std::int64_t we = orig.getMaterialWindowEndExclusiveSamples();
            v.emplace_back(leftNewId, mat, S, L, vLeft, ws, we);
            v.emplace_back(rightNewId, mat, splitT, L + vLeft, vRight, ws, we);
            any = true;
            rowChanged = true;
        }
        if (rowChanged)
        {
            out.push_back(duplicateTrackWithMovedClips(t, std::move(v)));
        }
        else
        {
            out.push_back(duplicateTrackSameClips(t));
        }
    }
    if (!any)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
        std::move(out), previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackChannelFaderGain(
    const SessionSnapshot& previous,
    const TrackId trackId,
    const float channelFaderGainLinear) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const float g = juce::jlimit(0.0f, kTrackChannelFaderGainMax, channelFaderGainLinear);
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            out.push_back(duplicateTrackSameClipsWithGain(t, g));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::move(out),
                            previous.arrangementExtentSamples_,
                            previous.getLeftLocatorSamples(),
                            previous.getRightLocatorSamples(),
                            previous.getProjectMusicalTime()));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackStereoPan(
    const SessionSnapshot& previous,
    const TrackId trackId,
    const float stereoPan) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const float p = sanitizeTrackStereoPan(stereoPan);
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            out.push_back(Track(t.getId(),
                                t.getName(),
                                t.getPlacedClips(),
                                t.getChannelFaderGain(),
                                t.isTrackOff(),
                                t.isMuted(),
                                t.getKind(),
                                p,
                                t.getRoutedOutputTrackId()));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::move(out),
                            previous.arrangementExtentSamples_,
                            previous.getLeftLocatorSamples(),
                            previous.getRightLocatorSamples(),
                            previous.getProjectMusicalTime()));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackRoutedOutputTo(
    const SessionSnapshot& previous,
    const TrackId trackId,
    const TrackId destTrackId) noexcept
{
    if (trackId == kInvalidTrackId || destTrackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (previous.getTrack(tIdx).getKind() == TrackKind::Master)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (!session_routing::isLegalRoutedOutputTarget(previous, trackId, destTrackId))
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (previous.getTrack(tIdx).getRoutedOutputTrackId() == destTrackId)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            out.push_back(Track(t.getId(),
                                t.getName(),
                                t.getPlacedClips(),
                                t.getChannelFaderGain(),
                                t.isTrackOff(),
                                t.isMuted(),
                                t.getKind(),
                                t.getStereoPan(),
                                destTrackId));
        }
    }
    return withTracks(std::move(out),
                      previous.getStoredArrangementExtentSamples(),
                      previous.getLeftLocatorSamples(),
                      previous.getRightLocatorSamples(),
                      previous.getProjectMusicalTime());
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackOff(
    const SessionSnapshot& previous,
    const TrackId trackId,
    const bool trackOff) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const Track& touched = previous.getTrack(tIdx);
    if (touched.getKind() == TrackKind::Master && trackOff)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const bool effectiveOff = (touched.getKind() == TrackKind::Master) ? false : trackOff;
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            out.push_back(Track(t.getId(),
                                t.getName(),
                                t.getPlacedClips(),
                                t.getChannelFaderGain(),
                                effectiveOff,
                                t.isMuted(),
                                t.getKind(),
                                t.getStereoPan(),
                                t.getRoutedOutputTrackId()));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::move(out),
                            previous.arrangementExtentSamples_,
                            previous.getLeftLocatorSamples(),
                            previous.getRightLocatorSamples(),
                            previous.getProjectMusicalTime()));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackMuted(
    const SessionSnapshot& previous,
    const TrackId trackId,
    const bool trackMuted) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_,
            previous.getLeftLocatorSamples(), previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            out.push_back(Track(t.getId(),
                                t.getName(),
                                t.getPlacedClips(),
                                t.getChannelFaderGain(),
                                t.isTrackOff(),
                                trackMuted,
                                t.getKind(),
                                t.getStereoPan(),
                                t.getRoutedOutputTrackId()));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::move(out),
                            previous.arrangementExtentSamples_,
                            previous.getLeftLocatorSamples(),
                            previous.getRightLocatorSamples(),
                            previous.getProjectMusicalTime()));
}

std::shared_ptr<const SessionSnapshot> SessionSnapshot::withTrackRenamed(const SessionSnapshot& previous,
                                                                         const TrackId trackId,
                                                                         juce::String newName) noexcept
{
    const juce::String trimmed = newName.trim();
    if (trackId == kInvalidTrackId || trimmed.isEmpty())
    {
        jassert(trackId != kInvalidTrackId);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_, previous.getLeftLocatorSamples(),
            previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const int tIdx = previous.findTrackIndexById(trackId);
    if (tIdx < 0)
    {
        jassert(false);
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_, previous.getLeftLocatorSamples(),
            previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    const Track& current = previous.getTrack(tIdx);
    if (current.getKind() == TrackKind::Master)
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_, previous.getLeftLocatorSamples(),
            previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    if (trimmed == current.getName())
    {
        return std::shared_ptr<const SessionSnapshot>(new SessionSnapshot{
            previous.tracks_, previous.arrangementExtentSamples_, previous.getLeftLocatorSamples(),
            previous.getRightLocatorSamples(), previous.getProjectMusicalTime()});
    }
    std::vector<Track> out;
    out.reserve((size_t)previous.getNumTracks());
    for (int i = 0; i < previous.getNumTracks(); ++i)
    {
        const Track& t = previous.getTrack(i);
        if (i != tIdx)
        {
            out.push_back(duplicateTrackSameClips(t));
        }
        else
        {
            out.push_back(t.renamed(trimmed));
        }
    }
    return std::shared_ptr<const SessionSnapshot>(
        new SessionSnapshot(std::move(out),
                            previous.arrangementExtentSamples_,
                            previous.getLeftLocatorSamples(),
                            previous.getRightLocatorSamples(),
                            previous.getProjectMusicalTime()));
}

bool SessionSnapshot::isEmpty() const noexcept
{
    for (const Track& t : tracks_)
    {
        if (t.getNumPlacedClips() > 0)
        {
            return false;
        }
    }
    return true;
}

const Track& SessionSnapshot::getTrack(const int index) const
{
    jassert(index >= 0 && index < getNumTracks());
    return tracks_.at((size_t)index);
}

int SessionSnapshot::findTrackIndexById(const TrackId id) const noexcept
{
    if (id == kInvalidTrackId)
    {
        return -1;
    }
    for (int i = 0; i < getNumTracks(); ++i)
    {
        if (tracks_[(size_t)i].getId() == id)
        {
            return i;
        }
    }
    return -1;
}

int SessionSnapshot::findCanonicalMasterTrackIndex() const noexcept
{
    for (int i = getNumTracks() - 1; i >= 0; --i)
    {
        if (tracks_[(size_t)i].getKind() == TrackKind::Master)
        {
            return i;
        }
    }
    return -1;
}

TrackId SessionSnapshot::findCanonicalMasterTrackId() const noexcept
{
    const int idx = findCanonicalMasterTrackIndex();
    if (idx < 0)
    {
        return kInvalidTrackId;
    }
    return tracks_[(size_t)idx].getId();
}

std::int64_t SessionSnapshot::getDerivedTimelineLengthSamples() const noexcept
{
    std::int64_t maxEnd = 0;
    for (const Track& t : tracks_)
    {
        for (int i = 0; i < t.getNumPlacedClips(); ++i)
        {
            const PlacedClip& p = t.getPlacedClip(i);
            const std::int64_t end = p.getStartSample() + p.getEffectiveLengthSamples();
            if (end > maxEnd)
            {
                maxEnd = end;
            }
        }
    }
    return maxEnd;
}

std::int64_t SessionSnapshot::getArrangementExtentSamples() const noexcept
{
    return juce::jmax(arrangementExtentSamples_, getDerivedTimelineLengthSamples());
}