#include "engine/RoutingPlanBuilder.h"

#include "domain/SessionSnapshot.h"

#include <juce_core/juce_core.h>

#include <queue>
#include <unordered_map>
#include <vector>

namespace routing_plan_builder
{

std::shared_ptr<const RoutingPlan> build(
    const SessionSnapshot& snap,
    const std::vector<std::pair<float*, float*>>& busScratchPairs) noexcept
{
    auto plan = std::make_shared<RoutingPlan>();

    const int n = snap.getNumTracks();
    if (n <= 0 || busScratchPairs.empty())
    {
        return plan;
    }

    std::unordered_map<TrackId, int> busTrackIndexToBusScratch;
    std::vector<int> busTrackIndices;
    busTrackIndices.reserve((size_t)n);

    for (int ti = 0; ti < n; ++ti)
    {
        const Track& tr = snap.getTrack(ti);
        if (tr.getKind() == TrackKind::Group || tr.getKind() == TrackKind::Master)
        {
            const int busIdx = static_cast<int>(busTrackIndices.size());
            busTrackIndexToBusScratch[tr.getId()] = busIdx;
            busTrackIndices.push_back(ti);
        }
    }

    if (busTrackIndices.empty())
    {
        return plan;
    }

    plan->busScratchL.resize(busScratchPairs.size());
    plan->busScratchR.resize(busScratchPairs.size());
    for (size_t bi = 0; bi < busScratchPairs.size(); ++bi)
    {
        plan->busScratchL[bi] = busScratchPairs[bi].first;
        plan->busScratchR[bi] = busScratchPairs[bi].second;
    }

    const TrackId masterId = snap.findCanonicalMasterTrackId();
    const auto masterBusIt = busTrackIndexToBusScratch.find(masterId);
    if (masterBusIt != busTrackIndexToBusScratch.end())
    {
        plan->masterBusIndex = static_cast<size_t>(masterBusIt->second);
    }

    for (int ti = 0; ti < n; ++ti)
    {
        const Track& tr = snap.getTrack(ti);
        if (tr.getKind() != TrackKind::Audio && tr.getKind() != TrackKind::Instrument)
        {
            continue;
        }
        const auto destIt = busTrackIndexToBusScratch.find(tr.getRoutedOutputTrackId());
        if (destIt == busTrackIndexToBusScratch.end())
        {
            continue;
        }
        RoutingPlan::SourceStep step;
        step.trackIndex = ti;
        step.destBusIndex = destIt->second;
        plan->sourceSteps.push_back(step);
    }

    // Topological order for Group buses (leaves first), Master last.
    const int numBuses = static_cast<int>(busTrackIndices.size());
    std::vector<int> inDegree((size_t)numBuses, 0);
    std::vector<std::vector<int>> groupEdges((size_t)numBuses);

    for (int bi = 0; bi < numBuses; ++bi)
    {
        const Track& busTr = snap.getTrack(busTrackIndices[(size_t)bi]);
        if (busTr.getKind() != TrackKind::Group)
        {
            continue;
        }
        const auto destIt = busTrackIndexToBusScratch.find(busTr.getRoutedOutputTrackId());
        if (destIt == busTrackIndexToBusScratch.end())
        {
            continue;
        }
        const int destBi = destIt->second;
        if (destBi == bi)
        {
            continue;
        }
        groupEdges[(size_t)bi].push_back(destBi);
        inDegree[(size_t)destBi]++;
    }

    std::queue<int> q;
    for (int bi = 0; bi < numBuses; ++bi)
    {
        const Track& busTr = snap.getTrack(busTrackIndices[(size_t)bi]);
        if (busTr.getKind() == TrackKind::Group && inDegree[(size_t)bi] == 0)
        {
            q.push(bi);
        }
    }

    std::vector<int> topoBusOrder;
    topoBusOrder.reserve((size_t)numBuses);
    while (!q.empty())
    {
        const int bi = q.front();
        q.pop();
        topoBusOrder.push_back(bi);
        for (const int succ : groupEdges[(size_t)bi])
        {
            if (--inDegree[(size_t)succ] == 0)
            {
                q.push(succ);
            }
        }
    }

    // Append Master (and any stray buses) at end.
    for (int bi = 0; bi < numBuses; ++bi)
    {
        if (std::find(topoBusOrder.begin(), topoBusOrder.end(), bi) == topoBusOrder.end())
        {
            topoBusOrder.push_back(bi);
        }
    }

    for (const int bi : topoBusOrder)
    {
        const int trackIndex = busTrackIndices[(size_t)bi];
        const Track& busTr = snap.getTrack(trackIndex);
        RoutingPlan::BusStep step;
        step.trackIndex = trackIndex;
        step.sourceBusIndex = bi;
        if (busTr.getKind() == TrackKind::Master)
        {
            step.destBusIndex = -1;
        }
        else
        {
            const auto destIt = busTrackIndexToBusScratch.find(busTr.getRoutedOutputTrackId());
            step.destBusIndex = (destIt != busTrackIndexToBusScratch.end()) ? destIt->second : -1;
        }
        plan->busSteps.push_back(step);
    }

    return plan;
}

} // namespace routing_plan_builder
