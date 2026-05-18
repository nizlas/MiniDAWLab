#pragma once

#include "domain/Track.h"

#include <vector>

class SessionSnapshot;
class Track;

namespace session_routing
{

[[nodiscard]] bool isOutputBusKind(TrackKind kind) noexcept;

/// Repair every non-Master row's `routedOutputTrackId_` to a legal Group/Master target (Master id required).
void repairRoutingInPlace(std::vector<Track>& tracks, TrackId masterTrackId) noexcept;

/// Legal output destinations for Inspector (Master + Groups that do not create a cycle with `fromTrackId`).
[[nodiscard]] std::vector<TrackId> legalOutputDestinations(const SessionSnapshot& snap,
                                                             TrackId fromTrackId) noexcept;

[[nodiscard]] bool wouldCreateRoutingCycle(const SessionSnapshot& snap,
                                           TrackId fromTrackId,
                                           TrackId proposedDestId) noexcept;

[[nodiscard]] bool isLegalRoutedOutputTarget(const SessionSnapshot& snap,
                                             TrackId fromTrackId,
                                             TrackId destTrackId) noexcept;

} // namespace session_routing
