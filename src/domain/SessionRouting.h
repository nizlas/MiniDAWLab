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

/// True when `fromTrackId` may send to `destTrackId` (Group destination, combined DAG acyclic).
[[nodiscard]] bool isLegalSendDestination(const SessionSnapshot& snap,
                                          TrackId fromTrackId,
                                          TrackId destTrackId) noexcept;

/// Same as `isLegalSendDestination` but evaluates the proposed `tracks` list (e.g. after a local edit).
[[nodiscard]] bool isLegalSendDestinationForTrackList(const std::vector<Track>& tracks,
                                                      TrackId fromTrackId,
                                                      TrackId destTrackId) noexcept;

/// Group rows that are valid send targets for `fromTrackId` (excludes self).
[[nodiscard]] std::vector<TrackId> legalSendDestinations(const SessionSnapshot& snap,
                                                         TrackId fromTrackId) noexcept;

/// Sanitize sends (Master cleared, invalid destinations dropped, amounts clamped, cycles broken by disable).
void repairSendsInPlace(std::vector<Track>& tracks, TrackId masterTrackId) noexcept;

/// Repair every `Midi` row's **MIDI To**: destinations that are missing or not `Instrument` rows
/// become `kInvalidTrackId` (None). Non-`Midi` rows are forced to None. Identity-based only —
/// never retargets by list position or name.
void repairMidiDestinationsInPlace(std::vector<Track>& tracks) noexcept;

/// Assign/repair `TrackSend::uiSlotIndex` (unique 0..3; drops overflow when no free slot).
void repairTrackSendUiSlotsInPlace(std::vector<TrackSend>& sends) noexcept;

} // namespace session_routing
