#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "domain/Track.h"
#include "io/ProjectFile.h"

class ExperimentalInstrumentHost;
class InstrumentTrackController;
class Session;
class PlaybackEngine;

/// Owns keyed + staging experimental instrument hosts/controllers, registry reconcile, host wiring, and playback-bridge publishes.
/// Per-track timeline headers/MIDI lanes live in `InstrumentTimelineRowCoordinator`; `TransportControlsContent` owns coordinators and attaches playback-engine lifecycle hooks here.
class InstrumentRuntimeCoordinator final
{
public:
    struct Callbacks
    {
        /// Same predicate as legacy `TransportControlsContent::wireExperimentalInstrumentHost` (playback/recording/count-in).
        std::function<bool()> drumNamePhaseCAudioProbeShouldSkip;
        /// Rows + `trackLanesView` attachment refresh after keyed/staging mutations (ordering preserved).
        std::function<void()> syncInstrumentTimelineRowAttachmentToSession;
    };

    InstrumentRuntimeCoordinator(Session& session, PlaybackEngine& playbackEngine, Callbacks callbacks);

    void wireExperimentalInstrumentHost(ExperimentalInstrumentHost& host, InstrumentTrackController& ctrl) noexcept;

    [[nodiscard]] TrackId canonicalInstrumentLaneTrackIdFromSession() const noexcept;

    [[nodiscard]] bool anyHeldHostShowsGrooveAgentLoaded() const noexcept;

    /// First keyed GA host matching legacy scan order; else staging host when it holds GA; otherwise nullptr.
    [[nodiscard]] ExperimentalInstrumentHost* findGrooveAgentTemplateHostPreferKeyed(
        ExperimentalInstrumentHost* avoidSameAs = nullptr) const noexcept;

    [[nodiscard]] ExperimentalInstrumentHost* getInstrumentHostForTrack(TrackId tid) const noexcept;
    [[nodiscard]] InstrumentTrackController* getInstrumentControllerForTrack(TrackId tid) const noexcept;

    [[nodiscard]] std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
        getOrCreateInstrumentRuntimeForTrack(TrackId tid);

    [[nodiscard]] std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
        getExperimentRuntimePairForGrooveAdds();

    void promoteInstrumentStagingIntoRegistryBoundTo(TrackId tid);

    void removeInstrumentRuntimeForTrack(TrackId tid) noexcept;

    void clearRuntimesPreserveBridgeOnly() noexcept;

    /// Audio thread hooks from `PlaybackEngine`.
    void experimentalBeginAudioBlockAllHosts(std::int64_t numSamples) noexcept;

    /// Device lifecycle from `PlaybackEngine`.
    void prepareExperimentalInstrumentHostsForDevice(double sampleRate, int blockSamples) noexcept;

    void releaseExperimentalInstrumentHostsDeviceResources() noexcept;

    void updateExperimentalPlaybackBridgeAfterRegistryChange();

    [[nodiscard]] bool isKeyedRuntimeRegistryEmpty() const noexcept;
    [[nodiscard]] ExperimentalInstrumentHost* stagingInstrumentHostUnchecked() const noexcept;
    [[nodiscard]] InstrumentTrackController* stagingInstrumentControllerUnchecked() const noexcept;

    void applyTimelineSampleRateToKeyedAndStaging(double sr) noexcept;
    void syncAllKeyedAndStagingShellWithHostState() noexcept;
    void deactivateAllKeyedAndStagingControllers() noexcept;

    void applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept;

    [[nodiscard]] bool hasAnyKeyedInstrumentControllerActive() const noexcept;
    /// Only keyed controllers (`setOnAudioHeaderActivated` clears instrument-strip active without touching staging).
    void deactivateKeyedInstrumentControllersOnly() noexcept;

    /// Keyed controllers: active only when `tid` matches map key; staging always deactivated (instrument header semantics).
    void setKeyedInstrumentControllersActiveExclusive(TrackId tid) noexcept;

private:
    void reconcileInstrumentRegistryAgainstSessionRows() noexcept;

    Session& session_;
    PlaybackEngine& playbackEngine_;
    Callbacks callbacks_;

    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>> instrumentHostsByTrackId_;
    std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>> instrumentControllersByTrackId_;
    std::unique_ptr<ExperimentalInstrumentHost> instrumentStagingHost_;
    std::unique_ptr<InstrumentTrackController> instrumentStagingController_;
    juce::String lastExperimentalPlaybackRoutingPublishFingerprint_;

    void runSyncInstrumentTimelineRowAttachmentCallback();
    [[nodiscard]] bool invokeDrumNamePhaseAudioProbeShouldSkipPredicate() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentRuntimeCoordinator)
};
