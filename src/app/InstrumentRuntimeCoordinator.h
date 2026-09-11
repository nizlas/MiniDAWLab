#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
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

    // --- Plugin-less MIDI content controllers (`TrackKind::Midi` rows, Phase B) ---
    // Keyed separately from instrument runtimes: no host, never a playback-bridge instrument entry,
    // published to the engine as `midiSources` instead.
    [[nodiscard]] InstrumentTrackController* getMidiContentControllerForTrack(TrackId tid) const noexcept;
    [[nodiscard]] InstrumentTrackController* getOrCreateMidiContentControllerForTrack(TrackId tid);
    void removeMidiContentControllerForTrack(TrackId tid) noexcept;
    /// The MIDI-clip-owning controller for `tid`, whichever kind of row it is: the keyed instrument
    /// controller, or the plugin-less MIDI content controller. Null when neither exists.
    [[nodiscard]] InstrumentTrackController* getMidiClipControllerForTrack(TrackId tid) const noexcept;

    /// Message thread: keyed instrument controllers plus MIDI content controllers (staging
    /// excluded). Null `fn` is ignored.
    void forEachInstrumentController(const std::function<void(TrackId, InstrumentTrackController&)>& fn);

    [[nodiscard]] std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
        getOrCreateInstrumentRuntimeForTrack(TrackId tid);

    [[nodiscard]] std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
        getExperimentRuntimePairForGrooveAdds();

    void promoteInstrumentStagingIntoRegistryBoundTo(TrackId tid);

    void removeInstrumentRuntimeForTrack(TrackId tid) noexcept;

    void clearRuntimesPreserveBridgeOnly() noexcept;

    // --- P2 Secondary instrument runtime (steering §17, PID-008/PID-009) --------------------
    // A second, independent host per destination for the OPTIONAL Secondary working instrument.
    // Instantiated only when needed (playback, audition, explicit configuration); never wired
    // into the Primary drum-name/template machinery; never renders the proxy. When the Secondary
    // is the transport source, the playback snapshot carries the Secondary host for that track —
    // one entry per track, so Primary/Proxy/Secondary can never feed the transport simultaneously.
    [[nodiscard]] ExperimentalInstrumentHost*
        getSecondaryInstrumentHostForTrack(TrackId tid) const noexcept;
    /// [Message thread] Instantiate/load on demand from the controller's persisted Secondary
    /// configuration (catalog resolution + saved state restore). Idempotent when already loaded
    /// with the same identity; a failed load latches per descriptor identity and is retried only
    /// after reconfiguration. Never touches the Primary host.
    [[nodiscard]] bool ensureSecondaryInstrumentLoadedForTrack(TrackId tid);
    /// [Message thread] Proxy-coordinator seam: the Secondary becomes / stops being the
    /// transport source of `tid`. Idempotent; republishes the playback snapshot so the switch
    /// takes effect at the next audio-block boundary. Deactivation queues an all-notes-off so
    /// held notes can never replay on reactivation.
    void setSecondaryTransportActive(TrackId tid, bool active);
    [[nodiscard]] bool isSecondaryTransportActive(TrackId tid) const noexcept
    {
        return secondaryTransportActive_.count(tid) != 0;
    }
    /// [Message thread] Retire the Secondary runtime (publish-before-destroy). The persisted
    /// Secondary CONFIGURATION on the controller is untouched — this only drops the instance.
    void removeSecondaryRuntimeForTrack(TrackId tid) noexcept;
    /// [Message thread] After UI select/replace/remove/mapping edits: clears the failure latch,
    /// applies the channel mapping to a live instance, retires the instance when the Secondary
    /// was removed. Reload-on-identity-change happens on the next ensure call.
    void noteSecondaryConfigurationChanged(TrackId tid);
    /// [Message thread] PID-008 audition gate wired from Main: true when Secondary audition may
    /// sound for `tid` (transport stopped, or the Secondary IS the transport source).
    void setSecondaryAuditionGate(std::function<bool(TrackId)> gate) noexcept
    {
        secondaryAuditionGate_ = std::move(gate);
    }

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
    /// Plugin-less controllers for `TrackKind::Midi` rows (no host entry ever exists for these ids).
    std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>> midiContentControllersByTrackId_;
    std::unique_ptr<ExperimentalInstrumentHost> instrumentStagingHost_;
    std::unique_ptr<InstrumentTrackController> instrumentStagingController_;
    /// P2 Secondary hosts (independent of the Primary host map; created on demand only).
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>
        secondaryInstrumentHostsByTrackId_;
    /// Descriptor identity currently loaded in the track's Secondary host (reload detection).
    std::unordered_map<TrackId, juce::String> secondaryLoadedIdentityByTrackId_;
    /// Descriptor identity whose load FAILED (no retry storms; cleared on reconfiguration).
    std::unordered_map<TrackId, juce::String> secondaryLoadFailureLatchByTrackId_;
    /// Tracks whose transport source is currently the Secondary host.
    std::set<TrackId> secondaryTransportActive_;
    std::function<bool(TrackId)> secondaryAuditionGate_;
    juce::String lastExperimentalPlaybackRoutingPublishFingerprint_;

    double lastPreparedDeviceSampleRate_ = 0.0;
    int lastPreparedDeviceBlockSize_ = 0;

    void runSyncInstrumentTimelineRowAttachmentCallback();
    [[nodiscard]] bool invokeDrumNamePhaseAudioProbeShouldSkipPredicate() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentRuntimeCoordinator)
};
