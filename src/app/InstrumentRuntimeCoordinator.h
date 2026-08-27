#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "io/ProjectFile.h"

class ExperimentalInstrumentHost;
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

    [[nodiscard]] bool anyHeldHostShowsHalionSonicLoaded() const noexcept;

    /// First keyed GA host matching legacy scan order; else staging host when it holds GA; otherwise nullptr.
    [[nodiscard]] ExperimentalInstrumentHost* findGrooveAgentTemplateHostPreferKeyed(
        ExperimentalInstrumentHost* avoidSameAs = nullptr) const noexcept;

    /// First keyed HALion Sonic host; else staging when it holds HALion Sonic.
    [[nodiscard]] ExperimentalInstrumentHost* findHalionSonicTemplateHostPreferKeyed(
        ExperimentalInstrumentHost* avoidSameAs = nullptr) const noexcept;

    [[nodiscard]] ExperimentalInstrumentHost* getInstrumentHostForTrack(TrackId tid) const noexcept;
    [[nodiscard]] InstrumentTrackController* getInstrumentControllerForTrack(TrackId tid) const noexcept;

    /// Message thread: keyed instrument controllers only (staging excluded). Null `fn` is ignored.
    void forEachInstrumentController(const std::function<void(TrackId, InstrumentTrackController&)>& fn);

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

    /// [Message thread] Stability C3 introspection: `{trackId, host*, controller*}` for every keyed
    /// runtime (staging excluded). Diagnostics only.
    [[nodiscard]] std::vector<std::tuple<TrackId, const void*, const void*>>
        exportKeyedRuntimePointersForDiagnostics() const;
    [[nodiscard]] ExperimentalInstrumentHost* stagingInstrumentHostUnchecked() const noexcept;
    [[nodiscard]] InstrumentTrackController* stagingInstrumentControllerUnchecked() const noexcept;

    [[nodiscard]] bool moveInstrumentMidiClipsBetweenTracks(TrackId sourceTrackId,
                                                           TrackId destTrackId,
                                                           std::vector<InstrumentMidiClipId> clipIdsInOrder,
                                                           std::int64_t deltaSamples) noexcept;

    void applyTimelineSampleRateToKeyedAndStaging(double sr) noexcept;
    /// Clips always play at the project tempo: re-align every keyed + staging controller's clip bpm
    /// with the session's project BPM (call after project BPM edits and undo/redo snapshot restores).
    void alignAllInstrumentClipTemposToProjectTempo() noexcept;
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

    double lastPreparedDeviceSampleRate_ = 0.0;
    int lastPreparedDeviceBlockSize_ = 0;

    void runSyncInstrumentTimelineRowAttachmentCallback();
    [[nodiscard]] bool invokeDrumNamePhaseAudioProbeShouldSkipPredicate() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentRuntimeCoordinator)
};
