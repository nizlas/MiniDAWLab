#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <juce_core/juce_core.h>

#include "domain/Track.h"

class ExperimentalInstrumentHost;
class InstrumentTrackController;
class Session;
class PlaybackEngine;

namespace mini_daw::experimental_instrument_playback_bridge
{
/// Re-keys host/controller map entries when map key differs from the controller's experimental domain TrackId.
void reconcileRegistryAgainstSessionRows(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>>& controllersByTrackId) noexcept;

/// Runs reconcile, builds snapshot ordered by session instrument rows, fingerprint dedup + optional routing log,
/// then publishes via PlaybackEngine (unchanged semantics vs former TransportControlsContent implementation).
void republishAfterRegistryChange(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>>& controllersByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost,
    const std::unique_ptr<InstrumentTrackController>& stagingController,
    Session& session,
    PlaybackEngine& playbackEngine,
    juce::String& lastPublishFingerprintInOut);

void prepareAllHostsForDevice(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost,
    double sampleRate,
    int blockSamples) noexcept;

void releaseAllHostsDeviceResources(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost) noexcept;

void beginAudioBlockAllHosts(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost,
    std::int64_t numSamples) noexcept;
} // namespace mini_daw::experimental_instrument_playback_bridge
