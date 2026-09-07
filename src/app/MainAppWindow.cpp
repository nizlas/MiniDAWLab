#include <JuceHeader.h>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "app/AddInstrumentTrackCoordinator.h"
#include "app/AudioClipImportCoordinator.h"
#include "app/InstrumentMidiImportCoordinator.h"
#include "app/ClipPasteboardController.h"
#include "app/MainAppDialogs.h"
#include "app/MainMenuModel.h"
#include "app/MidiEditorPresenter.h"
#include "app/PluginHostUiBindings.h"
#include "app/PortableProjectService.h"
#include "app/ProjectIoCoordinator.h"
#include "app/ProjectMainWindowBounds.h"
#include "app/RecordingCoordinator.h"
#include "app/TrackLanesEditCoordinator.h"
#include "app/TransportLayoutHelper.h"
#include "app/TransportPlayPauseStopController.h"
#include "app/UndoRedoCoordinator.h"
#include "app/ShortcutDiagnostics.h"
#include "app/TransportControlsFactory.h"
#include "app/TransportControlsShortcutTarget.h"
#include "app/Vst3PluginPickerCoordinator.h"
#include "plugins/InstrumentCatalog.h"
#include "app/InstrumentMusicalUndoSnapshot.h"
#include "app/ArrangementEventSelectionCoordinator.h"
#include "app/InstrumentRuntimeCoordinator.h"
#include "app/InstrumentTimelineRowCoordinator.h"
#include "app/AudioMixdownExporter.h"
#include "diagnostics/DiagnosticBuildFlags.h"
#include "diagnostics/PlaybackUiLoadLog.h"
#include "diagnostics/UiPaintLoadCounters.h"
#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"
#include "diagnostics/StabilityScenarioRunner.h"
#include "diagnostics/Spike01StateCapturePanel.h"
#include "diagnostics/TransportShortcutDiagLog.h"
#include "diagnostics/UiHangWatchdogDiag.h"

#include "domain/Session.h"
#include "domain/ProjectMusicalTime.h"
#include "domain/ArrangementMusicalSnap.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "app/AppProxyRenderEngine.h"
#include "instruments/ProxyPlaybackCoordinator.h"
#include "instruments/ProxyStatusModel.h"
#include "instruments/ProxyUpdatePolicyService.h"
#include "instruments/InstrumentTrackController.h"
#include "instruments/ProxyAssetStore.h"
#include "instruments/ProxyRenderScheduler.h"
#include "plugins/InsertSlotId.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/CoalescedRepaintFlusher.h"
#include "ui/FollowAutoscrollGovernor.h"
#include "ui/PlayheadOverlay.h"
#include "ui/UiPlayheadClock.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackLanesView.h"
#include "ui/EditToolIconStrip.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/InspectorView.h"
#include "ui/PortablePreparationWindow.h"
#include "ui/SnapResolutionComboBox.h"
#include "ui/SnapSettings.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/LatencySettingsStore.h"
#include "ui/LatencySettingsView.h"
#include "ui/experimental/ExperimentalMidiEditorWindow.h"

#include "io/AudioWaveformCache.h"
#include "io/ProjectFile.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

namespace
{
/// Compact "+" control matching `TrackHeaderView` mute/arm idle strip geometry (grey fill, subtle edge).
class AddTrackCornerGlyphButton final : public juce::Button
{
public:
    AddTrackCornerGlyphButton()
        : juce::Button("+")
    {
        setTooltip("Add track");
        setTriggeredOnMouseDown(true);
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const juce::Rectangle<int> cell = getLocalBounds();
        if (cell.isEmpty())
        {
            return;
        }

        const int cw = cell.getWidth();
        const int ch = cell.getHeight();
        const int inset = TrackHeaderView::kStripSquareBodyInsetPx;
        if (cw <= inset * 2 || ch <= inset * 2)
        {
            return;
        }

        const int availW = cw - inset * 2;
        const int availH = ch - inset * 2;
        const int side = juce::jmin(availW, availH);
        if (side < 6)
        {
            return;
        }

        const int cx = cell.getCentreX();
        const int cy = cell.getCentreY();
        const int ox = cx - side / 2;
        const int oy = cy - side / 2;
        const juce::Rectangle<int> bodyPx = juce::Rectangle<int>(ox, oy, side, side).getIntersection(cell);
        if (bodyPx.isEmpty())
        {
            return;
        }

        const juce::Rectangle<float> rf = bodyPx.toFloat();
        const float rad = juce::jlimit(1.4f, 2.85f, juce::jmin(rf.getWidth(), rf.getHeight()) * 0.16f);

        juce::Colour fill(0xff5a5858);
        juce::Colour edge(0xd0161616);
        if (shouldDrawButtonAsHighlighted && isEnabled())
        {
            fill = fill.brighter(0.12f);
            edge = edge.brighter(0.28f);
        }
        if (shouldDrawButtonAsDown)
        {
            fill = fill.darker(0.08f);
        }

        g.setColour(fill);
        g.fillRoundedRectangle(rf, rad);
        g.setColour(edge);
        g.drawRoundedRectangle(rf, rad, 1.0f);

        const float fontH = juce::jlimit(8.5f,
                                         11.5f,
                                         juce::jmin(static_cast<float>(bodyPx.getWidth()),
                                                    static_cast<float>(bodyPx.getHeight()))
                                             * 0.52f);
        g.setFont(juce::Font(juce::FontOptions().withHeight(fontH)));
        g.setColour(juce::Colour(0xffeaeaea));
        g.drawFittedText("+", bodyPx, juce::Justification::centred, 1);
    }
};
} // namespace

namespace
{
constexpr int kArrangementTimeSigCustomComboId = 100;

struct ArrangementTimeSigPreset
{
    int id;
    int num;
    int den;
    const char* label;
};

constexpr ArrangementTimeSigPreset kArrangementTimeSigPresets[] = {
    {1, 2, 4, "2/4"},
    {2, 3, 4, "3/4"},
    {3, 4, 4, "4/4"},
    {4, 5, 4, "5/4"},
    {5, 6, 8, "6/8"},
    {6, 7, 8, "7/8"},
};

[[nodiscard]] juce::String formatProjectBpmForToolbar(double bpm) noexcept
{
    if (!std::isfinite(bpm))
    {
        return "120";
    }
    juce::String s = juce::String(bpm, 2);
    while (s.endsWithChar('0') && s.containsChar('.'))
    {
        s = s.dropLastCharacters(1);
    }
    if (s.endsWithChar('.'))
    {
        s = s.dropLastCharacters(1);
    }
    return s.isEmpty() ? juce::String("120") : s;
}

[[nodiscard]] bool arrangementTimeSigPresetForComboId(const int comboId, int& numOut, int& denOut) noexcept
{
    for (const auto& p : kArrangementTimeSigPresets)
    {
        if (p.id == comboId)
        {
            numOut = p.num;
            denOut = p.den;
            return true;
        }
    }
    return false;
}

} // namespace

namespace mini_daw_app_transport
{
class TransportControlsContent : public juce::Component,
                                 public juce::ChangeListener,
                                 private juce::Timer,
                                 public collapsible_side_strip::Host,
                                 public TransportControlsShortcutTarget
{
private:
    static constexpr int kInspectorMaxW = 360;
    static constexpr int kInspectorDefaultW = 90;

    [[nodiscard]] int getSideStripWidth() const noexcept override { return inspectorCurrentWidth_; }

    void setSideStripWidth(int w) noexcept override { inspectorCurrentWidth_ = w; }

    [[nodiscard]] int getSideStripMaxWidth() const noexcept override { return kInspectorMaxW; }

    [[nodiscard]] int getSideStripDefaultWidth() const noexcept override { return kInspectorDefaultW; }

    void sideStripLayoutChanged() override { resized(); }

    void configureArrangementMusicalControls();
    void applyArrangementMusicalUiFromSession(ProjectMusicalTime mt, bool repaintTimeline);
    void rebuildArrangementTimeSignatureComboItems(const ProjectMusicalTime& mt);
    void commitArrangementBpmFromEditorIfNeeded();
    void handleArrangementTimeSignatureComboChangedByUser();

    [[nodiscard]] std::int64_t snapArrangementTimelineSample(std::int64_t sampleOnTimeline) const noexcept;

    void configureArrangementSnapControls();
    void applyArrangementSnapUiFromSettings(const SnapSettings& s, bool repaintTimeline);
    void handleArrangementSnapUiChangedByUser();
    [[nodiscard]] SnapProjectRootFields arrangementSnapPersistenceSnapshotForSave() const;
    void restoreArrangementSnapFromProjectRootFields(const SnapProjectRootFields& fields);

    void clearExperimentalInstrumentRuntimesPreserveBridgeOnly() noexcept;

    void configureTimelineRulerFormatControls();
    void applyTimelineRulerFormatButtonFromSession();

public:
    TransportControlsContent(Transport& transportIn,
                             Session& sessionIn,
                             PluginInsertHost& pluginInsertHostIn,
                             juce::AudioDeviceManager& deviceManagerIn,
                             RecorderService& recorderIn,
                             CountInClickOutput& countInClicksIn,
                             LatencySettingsStore& latencyStoreIn,
                             PlaybackEngine& playbackEngineIn,
                             proxy_render::ProxyRenderScheduler& proxyRenderSchedulerIn)
        : transport(transportIn)
        , session(sessionIn)
        , pluginHost_(pluginInsertHostIn)
        , deviceManager(deviceManagerIn)
        , recorder_(recorderIn)
        , countInClicks_(countInClicksIn)
        , latencyStore_(latencyStoreIn)
        , playbackEngine_(playbackEngineIn)
        , proxyRenderScheduler_(proxyRenderSchedulerIn)
        , timelineViewport_()
        , audioWaveformCache_()
        , rulerView(
              sessionIn,
              transportIn,
              deviceManagerIn,
              timelineViewport_,
              uiPlayheadClock_,
              [this]() {
                  return recorder_.isRecording()
                         || (recordingCoordinator_ != nullptr
                             && recordingCoordinator_->isCountInActive());
              })
        , trackLanesView(
              sessionIn,
              transportIn,
              timelineViewport_,
              deviceManagerIn,
              recorderIn,
              latencyStoreIn,
              audioWaveformCache_)
        , inspectorView_(sessionIn)
        , inspectorResizeSplitter_(*this)
        , inspectorCollapsedKnob_(*this)
    {
        recordingCoordinator_ = std::make_unique<RecordingCoordinator>(
            transport,
            session,
            deviceManager,
            recorder_,
            countInClicks_,
            latencyStore_,
            countInStatusLabel_,
            RecordingCoordinator::Callbacks{
                [this]() {
                    transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                },
                [this]() { syncViewportFromSession(); },
                [this]() {
                    rulerView.repaint();
                    trackLanesView.repaint();
                },
                [this](bool active,
                       std::int64_t cycleLocL,
                       std::int64_t cycleLocR,
                       std::int64_t recordingStartSample,
                       std::uint32_t lastSeenWrapCount) {
                    trackLanesView.setCycleRecordingPreviewContext(
                        active, cycleLocL, cycleLocR, recordingStartSample, lastSeenWrapCount);
                },
                [this]() { trackLanesView.clearCycleRecordingPreviewContext(); },
            });

        transportPlayPauseStopController_ = std::make_unique<TransportPlayPauseStopController>(
            transport,
            nullptr,
            TransportPlayPauseStopController::Callbacks{
                [this]() { return recorder_.isRecording(); },
                [this]() {
                    return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive();
                },
                [this](const char* sourceContext) {
                    recordingCoordinator_->stopRecordingAndCommitFromUi(sourceContext);
                },
                [this]() { recordingCoordinator_->cancelCountIn(); },
            });

        undoRedoCoordinator_ = std::make_unique<UndoRedoCoordinator>(
            session,
            pluginHost_,
            UndoRedoCoordinator::Callbacks{
                [this] { return recorder_.isRecording(); },
                [this] {
                    return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive();
                },
                [this] { return trackLanesView.isClipEditGestureInProgress(); },
                // C2B: immediate routing-plan republish after undo/redo snapshot restore.
                [this] { playbackEngine_.rebuildRoutingPlanFromSession(); },
                [this] { trackLanesView.cancelAllClipGesturesAndTransientUiState(); },
                [this] {
                    if (recordingCoordinator_ != nullptr)
                    {
                        recordingCoordinator_->reconcileCycleBookingAfterUndoSnapshotRestore();
                    }
                },
                [this] { syncViewportFromSession(); },
                [this] { trackLanesView.syncTracksFromSession(); },
                [this] { rulerView.repaint(); },
                [this] { trackLanesView.repaint(); },
                [this] { refreshInstrumentUi(); },
                [this] { inspectorView_.refreshFromSession(); },
                [this] {
                    applyArrangementMusicalUiFromSession(session.getProjectMusicalTime(), false);
                },
                [this]() -> std::vector<ProjectFileExperimentalInstrumentTrackV1> {
                    const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
                    if (snap == nullptr)
                    {
                        return {};
                    }
                    return mini_daw_app_transport::buildSortedInstrumentMusicalUndoSnapshot(
                        *snap,
                        InstrumentMusicalUndoSnapshotCallbacks{
                            [this](TrackId tid) {
                                return instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
                            } });
                },
                [](std::vector<ProjectFileExperimentalInstrumentTrackV1>& v) {
                    mini_daw_app_transport::stableSortInstrumentMusicalUndoVector(v);
                },
                [this](const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) {
                    instrumentRuntimeCoordinator_->applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(tracks);
                },
                [this] {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->rebindAfterInstrumentMusicalUndo();
                    }
                },
                [this](bool isRedoStep) {
                    if constexpr (undo_diagnostic::kUndoDiag)
                    {
                        ExperimentalMidiEditorWindow* midiEditorWnd = nullptr;
                        if (midiEditorPresenter_ != nullptr)
                        {
                            midiEditorWnd = midiEditorPresenter_->midiEditorWindow();
                        }
                        if (midiEditorWnd != nullptr)
                        {
                            const auto preId = midiEditorWnd->getBoundInstrumentClipId();
                            writeUndoDiagnosticLogLine(
                                (isRedoStep ? "[UndoDiag] invokeRedo pre instrument apply storedEditorClipId="
                                            : "[UndoDiag] invokeUndo pre instrument apply storedEditorClipId=")
                                + (preId.has_value()
                                       ? juce::String(static_cast<juce::int64>(*preId))
                                       : juce::String("none")));
                        }
                    }
                    else
                    {
                        juce::ignoreUnused(isRedoStep);
                    }
                },
                [this] {
                    if (instrumentRuntimeCoordinator_ != nullptr)
                    {
                        instrumentRuntimeCoordinator_->alignAllInstrumentClipTemposToProjectTempo();
                    }
                },
                [this] {
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
                },
                [this](const InstrumentTrackDeleteUndoSides& sides) {
                    if (trackLanesEditCoordinator_ != nullptr)
                    {
                        trackLanesEditCoordinator_->restoreDeletedInstrumentTrackForUndo(sides);
                    }
                },
                [this](TrackId tid) {
                    if (trackLanesEditCoordinator_ != nullptr)
                    {
                        trackLanesEditCoordinator_->redoTeardownDeletedInstrumentTrack(tid);
                    }
                },
            });

        audioClipImportCoordinator_ = std::make_unique<AudioClipImportCoordinator>(
            session,
            transport,
            deviceManager,
            trackLanesView,
            rulerView,
            inspectorView_,
            AudioClipImportCoordinator::Callbacks{
                [this](const juce::String& label, std::function<bool()> mutator) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableSessionEdit(label, std::move(mutator));
                    }
                },
                [this]() { syncViewportFromSession(); },
            });

        trackLanesView.setOnAudioTrackImportClipAtPlayhead([this](TrackId tid) {
            if (audioClipImportCoordinator_ != nullptr)
            {
                audioClipImportCoordinator_->addClipAtPlayheadForAudioTrack(tid);
            }
        });

        instrumentRuntimeCoordinator_ = std::make_unique<InstrumentRuntimeCoordinator>(
            session,
            playbackEngine_,
            InstrumentRuntimeCoordinator::Callbacks{
                [this]() noexcept {
                    return transport.readPlaybackIntentForUi() == PlaybackIntent::Playing || recorder_.isRecording()
                           || recordingCoordinator_->isCountInActive();
                },
                [this]() {
                    if (instrumentTimelineRowCoordinator_ != nullptr)
                    {
                        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
                    }
                },
            });

        // P1E/P1F: attach the production render engine (P1C capture + P1D lifecycle/executor +
        // P1F asset store + controller metadata) to the application-owned scheduler. Job
        // ownership lives in the scheduler — this view only calls the narrow API.
        {
            proxy_render::AppProxyRenderEngine::Dependencies engineDeps;
            engineDeps.session = &session;
            engineDeps.deviceManager = &deviceManager;
            engineDeps.hostForTrack = [this](const TrackId tid) -> ExperimentalInstrumentHost* {
                return instrumentRuntimeCoordinator_ != nullptr
                           ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid)
                           : nullptr;
            };
            engineDeps.controllerForTrack
                = [this](const TrackId tid) -> InstrumentTrackController* {
                return instrumentRuntimeCoordinator_ != nullptr
                           ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                           : nullptr;
            };
            engineDeps.clipsForTrack
                = [this](const TrackId tid) -> std::vector<const InstrumentMidiClip*> {
                std::vector<const InstrumentMidiClip*> clips;
                if (instrumentRuntimeCoordinator_ == nullptr)
                {
                    return clips;
                }
                InstrumentTrackController* c
                    = instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
                if (c == nullptr)
                {
                    c = instrumentRuntimeCoordinator_->getMidiContentControllerForTrack(tid);
                }
                if (c == nullptr)
                {
                    c = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
                }
                if (c != nullptr)
                {
                    for (const auto& up : c->getClips())
                    {
                        if (up != nullptr)
                        {
                            clips.push_back(up.get());
                        }
                    }
                }
                return clips;
            };
            engineDeps.onProxyPublished = [this](const TrackId tid) {
                if (proxyPlaybackCoordinator_ != nullptr)
                {
                    proxyPlaybackCoordinator_->refreshDestination(tid);
                }
                // P1H §18.3 (Recommended, Locked classification): publication metadata updates
                // DIRTY the project (the file references changed and should be saved) but are
                // excluded from musical undo (the undo snapshot strips proxy fields — §12.3
                // precedent). This is what guarantees metadata published after the previous
                // Save is not silently lost on close: close sees a dirty project and prompts,
                // and autosave persists the reference without ever triggering rendering.
                if (projectIoCoordinator_ != nullptr)
                {
                    projectIoCoordinator_->markProjectDirtyFromEdit();
                }
            };
            proxyRenderEngine_
                = std::make_unique<proxy_render::AppProxyRenderEngine>(std::move(engineDeps));
            proxyRenderScheduler_.attachEngine(proxyRenderEngine_.get());
        }

        // P1G: playback-source coordination (proxy substitution). Same project-runtime
        // owner as the render engine; holds no Session/host/UI references — every model
        // access goes through these message-thread lookups (all null-guarded).
        {
            proxy_playback::ProxyPlaybackCoordinator::Dependencies pbDeps;
            pbDeps.sessionSnapshotProvider = [this] {
                return session.loadSessionSnapshotForAudioThread();
            };
            pbDeps.projectFolderProvider = [this] { return session.getCurrentProjectFolder(); };
            pbDeps.timelineRateOrFallback
                = [this](const double fb) { return session.timelineSampleRateOr(fb); };
            pbDeps.destinationExists = [this](const TrackId tid) {
                return instrumentRuntimeCoordinator_ != nullptr
                       && instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid) != nullptr;
            };
            pbDeps.primaryUsable = [this](const TrackId tid) {
                ExperimentalInstrumentHost* const h
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid)
                          : nullptr;
                return h != nullptr && h->hasInstrument();
            };
            pbDeps.publishView
                = [this](const TrackId tid,
                         std::shared_ptr<const proxy_playback::ProxyPlaybackView> view) {
                if (ExperimentalInstrumentHost* const h
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid)
                          : nullptr)
                {
                    h->setProxyPlaybackView(std::move(view));
                }
            };
            pbDeps.proxyMetadataForTrack
                = [this](const TrackId tid) -> const ProjectFileProxyMetadataV20* {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                return c != nullptr ? c->getProxyMetadata() : nullptr;
            };
            pbDeps.proxyPublishedThisSession = [this](const TrackId tid) {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                return c != nullptr && c->wasProxyPublishedThisSession();
            };
            pbDeps.clipsForTrack
                = [this](const TrackId tid) -> std::vector<const InstrumentMidiClip*> {
                std::vector<const InstrumentMidiClip*> clips;
                if (instrumentRuntimeCoordinator_ == nullptr)
                {
                    return clips;
                }
                InstrumentTrackController* c
                    = instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
                if (c == nullptr)
                {
                    c = instrumentRuntimeCoordinator_->getMidiContentControllerForTrack(tid);
                }
                if (c == nullptr)
                {
                    c = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
                }
                if (c != nullptr)
                {
                    for (const auto& up : c->getClips())
                    {
                        if (up != nullptr)
                        {
                            clips.push_back(up.get());
                        }
                    }
                }
                return clips;
            };
            pbDeps.engineRateProvider = [this]() -> double {
                if (juce::AudioIODevice* dev = deviceManager.getCurrentAudioDevice())
                {
                    return dev->getCurrentSampleRate();
                }
                return 0.0;
            };
            proxyPlaybackCoordinator_
                = std::make_unique<proxy_playback::ProxyPlaybackCoordinator>(std::move(pbDeps));
        }

        // P1H: the per-destination update-policy engine (§18.1). Same project-runtime owner as
        // the render engine and playback coordinator; observes canonical identity, runs the
        // fixed five-minute Auto idle timers on its OWN 1 Hz timer (never the UI tick) and
        // feeds the P1E scheduler. All policy state is runtime-only (§20).
        {
            proxy_policy::ProxyUpdatePolicyService::Dependencies polDeps;
            polDeps.nowMs = [this]() -> double {
                return juce::Time::getMillisecondCounterHiRes() + proxyPolicyTestClockOffsetMs_;
            };
            polDeps.listDestinations = [this]() -> std::vector<TrackId> {
                std::vector<TrackId> out;
                if (const auto snap = session.loadSessionSnapshotForAudioThread())
                {
                    for (int i = 0; i < snap->getNumTracks(); ++i)
                    {
                        const Track& t = snap->getTrack(i);
                        if (t.getKind() == TrackKind::Instrument
                            && instrumentRuntimeCoordinator_ != nullptr
                            && instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(
                                   t.getId())
                                   != nullptr)
                        {
                            out.push_back(t.getId());
                        }
                    }
                }
                return out;
            };
            polDeps.modeForTrack = [this](const TrackId tid) -> juce::String {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                return c != nullptr ? c->getProxyUpdateMode() : juce::String("auto");
            };
            polDeps.identityForTrack = [this](const TrackId tid) {
                proxy_policy::ProxyUpdatePolicyService::DestinationIdentity id;
                if (proxyRenderEngine_ != nullptr)
                {
                    const auto cur = proxyRenderEngine_->currentIdentity(tid);
                    id.exists = cur.destinationExists && cur.expectedFingerprint.isNotEmpty();
                    id.fingerprint = cur.expectedFingerprint;
                    id.revision = cur.primarySemanticRevision;
                }
                return id;
            };
            polDeps.destinationState
                = [this](const TrackId tid) { return proxyRenderScheduler_.destinationState(tid); };
            polDeps.jobStatus
                = [this](const TrackId tid) { return proxyRenderScheduler_.jobStatus(tid); };
            polDeps.requestRender
                = [this](const TrackId tid) { return proxyRenderScheduler_.requestRender(tid); };
            polDeps.cancelDestination
                = [this](const TrackId tid) { proxyRenderScheduler_.cancelDestination(tid); };
            polDeps.notifyIdentityChanged = [this](const TrackId tid) {
                proxyRenderScheduler_.notifyDestinationIdentityChanged(tid);
            };
            polDeps.recordingActive = [this] {
                return recorder_.isRecording()
                       || (recordingCoordinator_ != nullptr
                           && recordingCoordinator_->isCountInActive());
            };
            polDeps.snapshotEligible = [this](const TrackId tid) {
                // §9.4.4 host-observable quiescence: recent host MIDI/CC delivery defers
                // snapshot capture. Notifier silence is a practical observation, never proof
                // of internal plugin quiescence (§9.4.5).
                ExperimentalInstrumentHost* const h
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid)
                          : nullptr;
                return h == nullptr
                       || h->millisecondsSinceLastHostMidiDelivery()
                              >= proxy_policy::kSnapshotQuiescenceDebounceMs;
            };
            polDeps.onRenderRelevantChangeObserved = [this](const TrackId tid) {
                if (proxyPlaybackCoordinator_ != nullptr)
                {
                    proxyPlaybackCoordinator_->refreshDestination(tid); // honest ProxyStale now
                }
            };
            proxyUpdatePolicyService_
                = std::make_unique<proxy_policy::ProxyUpdatePolicyService>(std::move(polDeps));
            proxyUpdatePolicyService_->startProductionTicker(1000);
        }

        // P1I: Inspector proxy status/control seams (§19). The view displays the pure
        // ProxyStatusModel output and invokes the narrow service actions; every wording
        // and availability rule lives in the tested model, not in the component.
        {
            InspectorProxyHost proxyUi;
            proxyUi.isProxyDestination = [this](const TrackId tid) {
                return instrumentRuntimeCoordinator_ != nullptr
                       && instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                              != nullptr;
            };
            proxyUi.getStatusView = [this](const TrackId tid) {
                proxy_status::ProxyStatusInputs in;
                if (proxyPlaybackCoordinator_ != nullptr)
                {
                    in.sourceState = proxyPlaybackCoordinator_->runtimeStateForTrack(tid);
                }
                in.destinationState = proxyRenderScheduler_.destinationState(tid);
                in.job = proxyRenderScheduler_.jobStatus(tid);
                if (proxyUpdatePolicyService_ != nullptr)
                {
                    in.policy = proxyUpdatePolicyService_->statusForTrack(tid);
                }
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                const ProjectFileProxyMetadataV20* const meta
                    = c != nullptr ? c->getProxyMetadata() : nullptr;
                in.hasMetadata = meta != nullptr;
                in.silentGeneration = meta != nullptr && meta->silentGeneration;
                return proxy_status::buildProxyStatusView(in);
            };
            proxyUi.setUpdateMode = [this](const TrackId tid, const int modeComboIndex) {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                if (c == nullptr)
                {
                    return;
                }
                const auto mode = (proxy_policy::ProxyUpdateMode)juce::jlimit(0, 3,
                                                                              modeComboIndex);
                if (c->setProxyUpdateModeFromUi(
                        proxy_policy::proxyUpdateModePersistedString(mode)))
                {
                    // §18.3: proxyUpdateMode is persisted project state — a change
                    // dirties the project but is not part of musical undo (the undo
                    // snapshot strips it). Runtime policy reacts on its next tick.
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
                    if (proxyUpdatePolicyService_ != nullptr)
                    {
                        proxyUpdatePolicyService_->tick();
                    }
                }
            };
            proxyUi.renderNow = [this](const TrackId tid) {
                if (proxyUpdatePolicyService_ != nullptr)
                {
                    (void)proxyUpdatePolicyService_->renderNow(tid);
                }
            };
            proxyUi.cancelRender = [this](const TrackId tid) {
                if (proxyUpdatePolicyService_ != nullptr)
                {
                    proxyUpdatePolicyService_->cancel(tid);
                }
            };
            proxyUi.retryRender = [this](const TrackId tid) {
                if (proxyUpdatePolicyService_ != nullptr)
                {
                    (void)proxyUpdatePolicyService_->retry(tid);
                }
            };
            inspectorView_.setInspectorProxyHost(std::move(proxyUi));
        }

        // P1J: the "Prepare Portable Project" operation owner (§16.6, PID-011). Same
        // project-runtime ownership as the policy service — never a dialog. Every seam
        // null-checks at CALL time (coordinators below are created later in this ctor).
        {
            portable_project::PortablePreparationService::Dependencies prepDeps;
            prepDeps.nowMs = [] { return juce::Time::getMillisecondCounterHiRes(); };
            prepDeps.listDestinations = [this]() -> std::vector<TrackId> {
                std::vector<TrackId> out;
                if (const auto snap = session.loadSessionSnapshotForAudioThread())
                {
                    for (int i = 0; i < snap->getNumTracks(); ++i)
                    {
                        const Track& t = snap->getTrack(i);
                        if (t.getKind() == TrackKind::Instrument
                            && instrumentRuntimeCoordinator_ != nullptr
                            && instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(
                                   t.getId())
                                   != nullptr)
                        {
                            out.push_back(t.getId());
                        }
                    }
                }
                return out;
            };
            prepDeps.destinationName = [this](const TrackId tid) -> juce::String {
                if (const auto snap = session.loadSessionSnapshotForAudioThread())
                {
                    for (int i = 0; i < snap->getNumTracks(); ++i)
                    {
                        if (snap->getTrack(i).getId() == tid)
                        {
                            return snap->getTrack(i).getName();
                        }
                    }
                }
                return "Track " + juce::String((int)tid);
            };
            prepDeps.identityForTrack = [this](const TrackId tid) {
                portable_project::PortablePreparationService::Dependencies::Identity id;
                if (proxyRenderEngine_ != nullptr)
                {
                    const auto cur = proxyRenderEngine_->currentIdentity(tid);
                    id.exists = cur.destinationExists && cur.expectedFingerprint.isNotEmpty();
                    id.fingerprint = cur.expectedFingerprint;
                    id.revision = cur.primarySemanticRevision;
                }
                return id;
            };
            prepDeps.destinationState
                = [this](const TrackId tid) { return proxyRenderScheduler_.destinationState(tid); };
            prepDeps.jobStatus
                = [this](const TrackId tid) { return proxyRenderScheduler_.jobStatus(tid); };
            prepDeps.requestRender
                = [this](const TrackId tid) { return proxyRenderScheduler_.requestRender(tid); };
            prepDeps.cancelDestination
                = [this](const TrackId tid) { proxyRenderScheduler_.cancelDestination(tid); };
            prepDeps.snapshotEligible = [this](const TrackId tid) {
                ExperimentalInstrumentHost* const h
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid)
                          : nullptr;
                return h == nullptr
                       || h->millisecondsSinceLastHostMidiDelivery()
                              >= proxy_policy::kSnapshotQuiescenceDebounceMs;
            };
            prepDeps.recordingActive = [this] {
                return recorder_.isRecording()
                       || (recordingCoordinator_ != nullptr
                           && recordingCoordinator_->isCountInActive());
            };
            prepDeps.getProxyMetadata
                = [this](const TrackId tid, ProjectFileProxyMetadataV20& out) {
                      InstrumentTrackController* const c
                          = instrumentRuntimeCoordinator_ != nullptr
                                ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(
                                      tid)
                                : nullptr;
                      const ProjectFileProxyMetadataV20* const meta
                          = c != nullptr ? c->getProxyMetadata() : nullptr;
                      if (meta == nullptr)
                      {
                          return false;
                      }
                      out = *meta;
                      return true;
                  };
            prepDeps.getProjectFile = [this] { return session.getCurrentProjectFile(); };
            prepDeps.isProjectDirty = [this] {
                return projectIoCoordinator_ != nullptr
                       && projectIoCoordinator_->isProjectDirty();
            };
            prepDeps.saveProjectNow = [this] {
                if (projectIoCoordinator_ == nullptr)
                {
                    return false;
                }
                // Synchronous known-file save (the flow requires a saved project
                // before starting): persists the freshly published proxy metadata.
                bool saved = false;
                projectIoCoordinator_->saveProjectThen([&saved](const bool ok) { saved = ok; });
                return saved && projectIoCoordinator_ != nullptr
                       && !projectIoCoordinator_->isProjectDirty();
            };
            portablePreparationService_
                = std::make_unique<portable_project::PortablePreparationService>(
                    std::move(prepDeps));
        }

        arrangementEventSelectionCoordinator_
            = std::make_unique<ArrangementEventSelectionCoordinator>(trackLanesView, *instrumentRuntimeCoordinator_);
        trackLanesView.setOnAudioClipMouseDownClearForeignSelections([this]() noexcept {
            if (arrangementEventSelectionCoordinator_ != nullptr)
            {
                arrangementEventSelectionCoordinator_->clearAllInstrumentControllerSelectionsOnly();
            }
        });

        vst3PluginPickerCoordinator_ = std::make_unique<Vst3PluginPickerCoordinator>(
            *this,
            session,
            pluginHost_,
            Vst3PluginPickerCoordinator::Callbacks{
                [this] {
                    return recorder_.isRecording()
                           || (recordingCoordinator_ != nullptr
                               && recordingCoordinator_->isCountInActive());
                },
                [this] { inspectorView_.refreshFromSession(); },
                [this](const TrackId tid) {
                    juce::ignoreUnused(tid);
                    refreshInstrumentUi();
                },
                [this](const TrackId tid) {
                    juce::ignoreUnused(tid);
                    instrumentRuntimeCoordinator_->updateExperimentalPlaybackBridgeAfterRegistryChange();
                },
                [this](const TrackId tid) {
                    return instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid);
                },
                [this](const TrackId tid) { return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid); },
                [this] { refreshInstrumentUi(); },
                [this]() { return instrumentRuntimeCoordinator_->canonicalInstrumentLaneTrackIdFromSession(); },
            });
        addChildComponent(*vst3PluginPickerCoordinator_);

        instrumentMidiImportCoordinator_ = std::make_unique<InstrumentMidiImportCoordinator>(
            session,
            transport,
            *instrumentRuntimeCoordinator_,
            trackLanesView,
            rulerView,
            inspectorView_,
            InstrumentMidiImportCoordinator::Callbacks{
                [this](const juce::String& lab, std::function<bool()> mutator) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableInstrumentEdit(lab, std::move(mutator));
                    }
                },
                [this]() { syncViewportFromSession(); },
                [this]() { refreshInstrumentUi(); },
            });

        instrumentTimelineRowCoordinator_ = std::make_unique<InstrumentTimelineRowCoordinator>(
            session,
            transport,
            trackLanesView,
            inspectorView_,
            timelineViewport_,
            *instrumentRuntimeCoordinator_,
            InstrumentTimelineRowCoordinator::Callbacks{
                [this](TrackId laneTid) {
                    vst3PluginPickerCoordinator_->runExperimentalInstrumentPluginDescriptionRescanForTrack(laneTid);
                },
                [this]() {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->refreshInstrumentUiIfOpen();
                    }
                },
                [this](TrackId timelineTid, InstrumentMidiClipId clipId) {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->openMidiEditorForInstrumentClip(timelineTid, clipId);
                    }
                },
                [this](TrackId laneTid) {
                    if (instrumentMidiImportCoordinator_ != nullptr)
                    {
                        instrumentMidiImportCoordinator_->importMidiFileForInstrumentTrack(laneTid);
                    }
                },
                [this](const juce::String& lab, std::function<bool()> mutator) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableInstrumentEdit(lab, std::move(mutator));
                    }
                },
                [this](TrackId keepInstrumentTrackId) noexcept {
                    if (arrangementEventSelectionCoordinator_ != nullptr)
                    {
                        arrangementEventSelectionCoordinator_->clearAudioAndOtherInstrumentControllerSelections(
                            keepInstrumentTrackId);
                    }
                },
                [this]() noexcept {
                    if (arrangementEventSelectionCoordinator_ != nullptr)
                    {
                        arrangementEventSelectionCoordinator_->clearAllArrangementEventSelections();
                    }
                },
                [this](std::int64_t s) noexcept { return snapArrangementTimelineSample(s); },
            });

        midiEditorPresenter_ = std::make_unique<MidiEditorPresenter>(
            transport,
            session,
            deviceManager,
            recorder_,
            timelineViewport_,
            midiEditorWindow_,
            MidiEditorPresenter::Callbacks{
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
                },
                [this](TrackId tid) { return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid); },
                [this](const juce::String& lab, std::function<bool()> m) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableInstrumentEdit(lab, std::move(m));
                    }
                },
                [this] { invokeUndoFromWindowShortcut(); },
                [this] { invokeRedoFromWindowShortcut(); },
                [this] { transportPlayPauseStopController_->invokePlayPauseToggleFromWindowShortcut(); },
                [this] { transportPlayPauseStopController_->stopOrSeekFromStopButton(); },
                [this] { invokeRecordToggleFromWindowShortcut(); },
                [this] { invokeJumpToLeftLocatorFromWindowShortcut(); },
                [this]() {
                    return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive();
                },
                [this]() {
                    return recorder_.isRecording()
                           || (recordingCoordinator_ != nullptr
                               && recordingCoordinator_->isCountInActive());
                },
                [this]() { instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow(); },
                [this] { refreshInstrumentUi(); },
                [this](double sr) {
                    instrumentRuntimeCoordinator_->applyTimelineSampleRateToKeyedAndStaging(sr);
                },
                [this]() {
                    applyArrangementSnapUiFromSettings(session.getArrangementSnapSettings(), true);
                },
                [this] {
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->saveProject();
                    }
                },
            });

        session.setOnTimelineRulerTimeDisplayChanged([this]() {
            applyTimelineRulerFormatButtonFromSession();
            rulerView.repaint();
            trackLanesView.repaint();
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->syncTimelineRulerFormatUiIfEditorOpen();
            }
        });

        ClipPasteboardController::Callbacks clipPasteCallbacks;
        clipPasteCallbacks.isRecording = [this] { return recorder_.isRecording(); };
        clipPasteCallbacks.isCountInActive = [this] {
            return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive();
        };
        clipPasteCallbacks.executeUndoableSessionEdit
            = [this](const juce::String& label, std::function<bool()> mutator) {
                  if (undoRedoCoordinator_ != nullptr)
                  {
                      undoRedoCoordinator_->executeUndoableSessionEdit(label, std::move(mutator));
                  }
              };
        clipPasteCallbacks.executeUndoableInstrumentEdit
            = [this](const juce::String& label, std::function<bool()> mutator) {
                  if (undoRedoCoordinator_ != nullptr)
                  {
                      undoRedoCoordinator_->executeUndoableInstrumentEdit(label, std::move(mutator));
                  }
              };
        clipPasteCallbacks.getInstrumentControllerForTrack = [this](const TrackId tid) {
            return instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
        };
        clipPasteCallbacks.syncViewportFromSession = [this] { syncViewportFromSession(); };
        clipPasteCallbacks.refreshInstrumentArrangementUi = [this] { refreshInstrumentUi(); };
        clipPasteCallbacks.openMidiEditorForInstrumentClip = [this](const TrackId timelineTid,
                                                                     const InstrumentMidiClipId clipId) {
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->openMidiEditorForInstrumentClip(timelineTid, clipId);
            }
        };
        clipPasteCallbacks.snapArrangementTimelineSample
            = [this](std::int64_t s) noexcept { return snapArrangementTimelineSample(s); };
        clipPasteboardController_
            = std::make_unique<ClipPasteboardController>(
                session,
                transport,
                trackLanesView,
                rulerView,
                inspectorView_,
                std::move(clipPasteCallbacks));

        addInstrumentTrackCoordinator_ = std::make_unique<AddInstrumentTrackCoordinator>(
            AddInstrumentTrackCoordinator::Refs{ session, *instrumentRuntimeCoordinator_ },
            AddInstrumentTrackCoordinator::Callbacks{
                [this]() { refreshInstrumentUi(); },
                [this]() { resized(); },
                [this]() {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
                    }
                },
            });

        playbackEngine_.setExperimentalInstrumentDeviceLifecycleHooks(
            [this](const double sr, const int bs) {
                instrumentRuntimeCoordinator_->prepareExperimentalInstrumentHostsForDevice(sr, bs);
            },
            [this] { instrumentRuntimeCoordinator_->releaseExperimentalInstrumentHostsDeviceResources(); },
            [this](const int ns) {
                instrumentRuntimeCoordinator_->experimentalBeginAudioBlockAllHosts(static_cast<std::int64_t>(ns));
            });
        trackLanesView.setStructuralTimelineEditBlockedPredicate([this]() {
            // Power / delete / inserts are not realtime-safe paths: blocked while Playing (not mute).
            return recorder_.isRecording()
                   || (recordingCoordinator_ != nullptr
                       && recordingCoordinator_->isCountInActive())
                   || transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
        });
        trackLanesView.setInstrumentMidiClipMoveBlockedPredicate([this]() {
            return recorder_.isRecording()
                   || (recordingCoordinator_ != nullptr
                       && recordingCoordinator_->isCountInActive());
        });
        trackLanesView.setArrangementTimelineSnapFunction(
            [this](std::int64_t s) noexcept { return snapArrangementTimelineSample(s); });
        setWantsKeyboardFocus(true);
        audioWaveformCache_.setOnPyramidReady([this](const AudioClip*) { trackLanesView.repaint(); });
        timelineViewport_.setOnVisibleRangeChanged([this] {
            ++statsViewportChanges_;
            // Any viewport change that is not a follow page is user/system driven (wheel zoom/pan,
            // middle-drag, extent clamp): remember it locally *and* globally so follow in this and
            // other windows briefly yields (anti-fight/anti-loop, cross-window coordination).
            if (!followPanInProgress_)
            {
                const double nowMs = juce::Time::getMillisecondCounterHiRes();
                mainFollowGovernor_.noteUserViewportChange(nowMs);
                GlobalFollowWorkCoordinator::instance().noteUserViewportGesture(this, nowMs);
                ui_hang_watchdog::noteUserViewportChange();
            }
            // Repaint-storm fix: mark dirty once per message batch, not once per wheel event —
            // otherwise a fast zoom gesture pays one full arrangement recomposition per event.
            // A follow page flushes synchronously: it happens inside the overlay frame tick whose
            // structural invalidation already paints this turn, so deferring would split the page
            // into two full paint passes.
            coalescedViewportRepaint_.requestFlush();
            if (followPanInProgress_)
            {
                coalescedViewportRepaint_.flushNowIfPending();
            }
        });
        addTrackCornerPlusButton_.onClick = [this] {
            juce::PopupMenu menu;
            menu.addItem(1, "Add Audio Track");
            menu.addItem(3, "Add Group Track");
            menu.addItem(4, "Add MIDI Track");
            juce::PopupMenu instrMenu;
            instrMenu.addItem(99, "Rescan instrument plugins...");
            instrMenu.addItem(98, "Import plugin cache...");
            instrMenu.addSeparator();
            instrMenu.addItem(100, "Groove Agent SE");
            instrMenu.addItem(101, "HALion Sonic");
            std::vector<mini_daw::InstrumentCatalogEntry> catalogEntries;
            if (mini_daw::loadInstrumentCatalogFromCache(catalogEntries))
            {
                instrMenu.addSeparator();
                instrMenu.addItem(
                    juce::PopupMenu::Item("Discovered instruments:").setEnabled(false));
                constexpr int kCatalogMenuBaseId = 2000;
                for (size_t i = 0; i < catalogEntries.size(); ++i)
                {
                    const juce::String label = catalogEntries[i].description.name.isNotEmpty()
                                                   ? catalogEntries[i].description.name
                                                   : juce::File(catalogEntries[i].bundlePath)
                                                         .getFileNameWithoutExtension();
                    instrMenu.addItem(kCatalogMenuBaseId + static_cast<int>(i), label);
                }
            }
            menu.addSubMenu("Add Instrument Track", instrMenu);
            juce::Component::SafePointer<mini_daw_app_transport::TransportControlsContent> safeThis(this);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&addTrackCornerPlusButton_),
                [safeThis](int result) {
                    if (safeThis == nullptr || result == 0)
                    {
                        return;
                    }
                    if (result == 1)
                    {
                        safeThis->session.addTrack();
                        safeThis->syncViewportFromSession();
                        safeThis->trackLanesView.syncTracksFromSession();
                        safeThis->inspectorView_.refreshFromSession();
                        return;
                    }
                    if (result == 3)
                    {
                        safeThis->session.addGroupTrack();
                        safeThis->syncViewportFromSession();
                        safeThis->trackLanesView.syncTracksFromSession();
                        safeThis->inspectorView_.refreshFromSession();
                        return;
                    }
                    if (result == 4)
                    {
                        const auto newMidiId = safeThis->session.addMidiTrack();
                        if (newMidiId.has_value()
                            && safeThis->instrumentRuntimeCoordinator_ != nullptr)
                        {
                            // The controller is what gives the row its MIDI event lane and lets
                            // the engine publish it as a MIDI source.
                            (void)safeThis->instrumentRuntimeCoordinator_
                                ->getOrCreateMidiContentControllerForTrack(*newMidiId);
                        }
                        safeThis->syncViewportFromSession();
                        safeThis->trackLanesView.syncTracksFromSession();
                        safeThis->refreshInstrumentUi();
                        safeThis->inspectorView_.refreshFromSession();
                        return;
                    }
                    if (result == 99)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            safeThis->addInstrumentTrackCoordinator_->rescanInstrumentPluginsFromMenu();
                        }
                        return;
                    }
                    if (result == 98)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            safeThis->addInstrumentTrackCoordinator_->importPluginCacheFromMenu();
                        }
                        return;
                    }
                    if (result == 100)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            safeThis->addInstrumentTrackCoordinator_->addGrooveAgentInstrumentTrackFromMenu();
                        }
                        return;
                    }
                    if (result == 101)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            safeThis->addInstrumentTrackCoordinator_->addHalionSonicInstrumentTrackFromMenu();
                        }
                        return;
                    }
                    constexpr int kCatalogMenuBaseId = 2000;
                    if (result >= kCatalogMenuBaseId)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            std::vector<mini_daw::InstrumentCatalogEntry> catalogEntries;
                            if (mini_daw::loadInstrumentCatalogFromCache(catalogEntries))
                            {
                                const int idx = result - kCatalogMenuBaseId;
                                if (idx >= 0 && idx < static_cast<int>(catalogEntries.size()))
                                {
                                    safeThis->addInstrumentTrackCoordinator_->addGenericInstrumentTrackFromCatalog(
                                        catalogEntries[static_cast<size_t>(idx)]);
                                }
                            }
                        }
                    }
                });
        };

        mainMenuModel_ = std::make_unique<mini_daw_app_menu::MainMenuModel>(mini_daw_app_menu::MainMenuActions{
            [this] {
                if (projectIoCoordinator_ != nullptr)
                {
                    projectIoCoordinator_->saveProject();
                }
            },
            [this] {
                if (projectIoCoordinator_ != nullptr)
                {
                    projectIoCoordinator_->loadProject();
                }
            },
            [this] { showAudioMixdownDialog(); },
            [this] { startPreparePortableProjectFlow(); },
            [this] { showAudioSettingsDialog(); },
            [this] { showHelpMenuPopup(); },
        });
        menuBar_ = std::make_unique<juce::MenuBarComponent>(mainMenuModel_.get());
        addAndMakeVisible(*menuBar_);

        editToolIconStrip_.onToolSelected = [this](EditTool t) { applyEditToolSelection(t); };
        addAndMakeVisible(editToolIconStrip_);

        configureArrangementMusicalControls();
        addAndMakeVisible(arrangementBpmLabel_);
        addAndMakeVisible(arrangementBpmEditor_);
        addAndMakeVisible(arrangementTimeSignatureCombo_);

        configureArrangementSnapControls();
        addAndMakeVisible(arrangementSnapToggle_);
        addAndMakeVisible(arrangementSnapResolutionCombo_);
        configureTimelineRulerFormatControls();
        addAndMakeVisible(arrangementTimelineFormatCombo_);
        applyArrangementMusicalUiFromSession(session.getProjectMusicalTime(), false);
        applyArrangementSnapUiFromSettings(SnapSettings{}, false);
        applyTimelineRulerFormatButtonFromSession();

        projectIoCoordinator_ = std::make_unique<ProjectIoCoordinator>(
            transport,
            session,
            deviceManager,
            pluginHost_,
            playbackEngine_,
            ProjectIoCoordinator::Callbacks{
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
                },
                [this] {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->snapshotOpenClipViewportFromRollIfOpen();
                    }
                },
                [this] { clearExperimentalInstrumentRuntimesPreserveBridgeOnly(); },
                [this](TrackId tid) { return instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid); },
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getOrCreateMidiContentControllerForTrack(tid);
                },
                [this] {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->refreshInstrumentUiIfOpen();
                    }
                },
                [this] {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->clearHistory();
                    }
                },
                [this] {
                    appendProjectLoadDiagnosticLine("load: before syncViewportFromSession");
                    syncViewportFromSession();
                    appendProjectLoadDiagnosticLine("load: after syncViewportFromSession");
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
                    }
                    appendProjectLoadDiagnosticLine("load: before inspector/header selection refresh");
                    trackLanesView.syncTracksFromSession();
                    inspectorView_.refreshFromSession();
                    appendProjectLoadDiagnosticLine("load: after inspector/header selection refresh");
                    applyArrangementMusicalUiFromSession(session.getProjectMusicalTime(), false);
                    rulerView.repaint();
                    trackLanesView.repaint();
                    appendProjectLoadDiagnosticLine("load: before playback bridge/runtime sync");
                    refreshInstrumentUi();
                    appendProjectLoadDiagnosticLine("load: after playback bridge/runtime sync");
                    resized();
                },
                [this]() -> SnapProjectRootFields { return arrangementSnapPersistenceSnapshotForSave(); },
                [this](const SnapProjectRootFields& root) {
                    restoreArrangementSnapFromProjectRootFields(root);
                },
                [this]() -> std::optional<ProjectFileMainWindowBoundsV1> {
                    if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                    {
                        auto b = captureProjectMainWindowBoundsForProjectSave(*dw);
                        if (b.has_value())
                        {
                            b->followPlayhead = mainFollowPlayhead_;
                        }
                        return b;
                    }
                    return std::nullopt;
                },
                [this](const ProjectFileV1& loaded) {
                    // Main-arrangement Follow: saved in the `mainWindow` object; old projects
                    // without the object (or field) load with Follow ON.
                    mainFollowPlayhead_
                        = !loaded.hasMainWindowBounds || loaded.mainWindowBounds.followPlayhead;
                    mainFollowPlayheadToggle_.setToggleState(mainFollowPlayhead_,
                                                             juce::dontSendNotification);
                    appendProjectLoadDiagnosticLine(
                        juce::String("load: main follow=") + (mainFollowPlayhead_ ? "on" : "off")
                        + (loaded.hasMainWindowBounds ? "" : " (default, no mainWindow object)"));
                    if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                    {
                        if (loaded.hasMainWindowBounds)
                        {
                            const ProjectWindowBoundsRestoreOutcome outcome
                                = applyProjectWindowBoundsClamped(*dw, loaded.mainWindowBounds);
                            appendProjectLoadDiagnosticLine(
                                "load: main window bounds restore x="
                                + juce::String(loaded.mainWindowBounds.x)
                                + " y=" + juce::String(loaded.mainWindowBounds.y)
                                + " w=" + juce::String(loaded.mainWindowBounds.width)
                                + " h=" + juce::String(loaded.mainWindowBounds.height)
                                + " maximized=" + juce::String(loaded.mainWindowBounds.maximized ? 1 : 0)
                                + " outcome=" + describeWindowBoundsRestoreOutcome(outcome));
                        }
                        else
                        {
                            appendProjectLoadDiagnosticLine(
                                "load: no main window bounds in project (keeping current placement)");
                        }
                    }
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->setMidiEditorWindowBoundsFromLoadedProject(loaded);
                        appendProjectLoadDiagnosticLine(
                            loaded.hasMidiEditorWindowBounds
                                ? juce::String("load: MIDI editor bounds memo seeded x="
                                               + juce::String(loaded.midiEditorWindowBounds.x) + " y="
                                               + juce::String(loaded.midiEditorWindowBounds.y) + " w="
                                               + juce::String(loaded.midiEditorWindowBounds.width) + " h="
                                               + juce::String(loaded.midiEditorWindowBounds.height))
                                : juce::String("load: no MIDI editor bounds in project (memo cleared)"));
                    }
                },
                [this]() -> std::optional<ProjectFileMainWindowBoundsV1> {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        return midiEditorPresenter_->getMidiEditorWindowBoundsForProjectSave();
                    }
                    return std::nullopt;
                },
                [this]() -> std::optional<ProjectFileMidiEditorWorkspaceV1> {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        return midiEditorPresenter_->getMidiEditorWorkspaceForProjectSave();
                    }
                    return std::nullopt;
                },
                [this](const ProjectFileV1& loaded) {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->tryRestoreMidiEditorWorkspaceAfterProjectLoad(loaded);
                    }
                },
                [this] { showSavingProjectToast(); },
                // P1H onProjectAboutToBeReplaced: obsolete/cancel the OLD project's proxy work
                // and drop the runtime-only policy timers before runtimes are cleared (§13.3).
                // P1J: a running portable preparation belongs to the OLD project — bounded
                // cancellation + staging cleanup before the replacement proceeds (§16.6).
                [this] {
                    if (portablePreparationService_ != nullptr)
                    {
                        portablePreparationService_->shutdownAndJoin();
                    }
                    proxyRenderScheduler_.notifyProjectChanged();
                    if (proxyUpdatePolicyService_ != nullptr)
                    {
                        proxyUpdatePolicyService_->noteProjectChanged();
                    }
                },
                // P1H onProjectLoaded: capture asset source hints for later Save As rehoming.
                [this](const juce::File& projectFolder) {
                    captureProxyAssetSourceHints(projectFolder);
                },
                // P1H §18.2 onSuccessfulUserSave: queue proxy work per destination update mode
                // (On Save queues stale destinations; Auto queues only already-eligible work;
                // Manual/Off queue nothing). Never waits for rendering; autosave never fires it.
                [this] {
                    if (proxyUpdatePolicyService_ != nullptr)
                    {
                        proxyUpdatePolicyService_->noteSuccessfulUserSave();
                    }
                },
                // P1H §16.6 Save As: copy referenced generations into the new project layout.
                [this](const juce::File& projectFolder) {
                    rehomeProxyAssetsIntoFolder(projectFolder);
                },
            });

        // Stability C5: app-level states that must block a periodic autosave tick. Everything
        // else (save/load/export/undo/redo/delete) runs synchronously on the message thread and
        // cannot overlap the timer; modal prompts are covered inside the coordinator.
        projectIoCoordinator_->setAutosaveBlockReasonProvider([this]() -> juce::String {
            if (recorder_.isRecording())
            {
                return "recording active";
            }
            if (recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive())
            {
                return "count-in active";
            }
            return {};
        });

        addAndMakeVisible(addTrackCornerPlusButton_);
        if (shortcut_diagnostics::kShowKeyDiagnostic)
        {
            addAndMakeVisible(keyDiagLabel_);
            keyDiagLabel_.setFont(juce::FontOptions(11.0f));
            keyDiagLabel_.setJustificationType(juce::Justification::centredLeft);
            keyDiagLabel_.setText("key: —", juce::dontSendNotification);
        }
        if constexpr (shortcut_diagnostics::kShowShortcutDiagnostics)
        {
            shortcutDiagLabel_ = std::make_unique<juce::Label>();
            shortcutDiagLabel_->setFont(juce::FontOptions(12.0f));
            shortcutDiagLabel_->setJustificationType(juce::Justification::centredLeft);
            shortcutDiagLabel_->setInterceptsMouseClicks(false, false);
            shortcutDiagLabel_->setMinimumHorizontalScale(1.0f);
            shortcutDiagLabel_->setText(
                "[ShortcutDiag ui] (press a key — same source as routeShortcut logger line)",
                juce::dontSendNotification);
            addAndMakeVisible(*shortcutDiagLabel_);
        }
        countInStatusLabel_.setFont(juce::FontOptions(12.0f));
        countInStatusLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(countInStatusLabel_);
        addAndMakeVisible(mainFollowPlayheadToggle_);
        mainFollowPlayheadToggle_.setButtonText("Follow");
        mainFollowPlayheadToggle_.setClickingTogglesState(true);
        mainFollowPlayheadToggle_.setToggleState(mainFollowPlayhead_, juce::dontSendNotification);
        mainFollowPlayheadToggle_.setTooltip("Follow playhead during playback (main arrangement)");
        // Never take keyboard focus: a focused button would consume Space (play/pause) after a click.
        mainFollowPlayheadToggle_.setWantsKeyboardFocus(false);
        mainFollowPlayheadToggle_.onClick = [this] {
            mainFollowPlayhead_ = mainFollowPlayheadToggle_.getToggleState();
            if (mainFollowPlayhead_)
            {
                // Bring the playhead into view right away instead of waiting for the edge trigger.
                maybeFollowMainArrangementPlayhead(
                    (double)transport.readPlayheadSamplesForUi(), false);
            }
        };
        savingProjectToastLabel_.setText("Saving project", juce::dontSendNotification);
        savingProjectToastLabel_.setJustificationType(juce::Justification::centred);
        savingProjectToastLabel_.setFont(juce::FontOptions(14.0f));
        savingProjectToastLabel_.setColour(juce::Label::backgroundColourId, juce::Colour(0xee2a2a33));
        savingProjectToastLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        savingProjectToastLabel_.setColour(juce::Label::outlineColourId,
                                           juce::Colours::white.withAlpha(0.25f));
        savingProjectToastLabel_.setInterceptsMouseClicks(false, false);
        addChildComponent(savingProjectToastLabel_);
        addAndMakeVisible(inspectorView_);
        addAndMakeVisible(inspectorResizeSplitter_);
        addAndMakeVisible(rulerView);
        addAndMakeVisible(trackLanesView);
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
        lanePlayheadOverlay_ = std::make_unique<PlayheadOverlay>(
            session, transport, timelineViewport_, deviceManager, uiPlayheadClock_);
        // Single playhead frame per tick: the overlay samples the shared clock once and pushes the
        // same display sample to the ruler, so every main-window indicator draws one position.
        // Follow-autoscroll runs first so the ruler maps this frame with the panned viewport.
        lanePlayheadOverlay_->setOnPlayheadFrameAdvanced(
            [this](const double displaySamples) {
                ui_hang_watchdog::heartbeat();
                ui_hang_watchdog::notePlayheadFrame(
                    (long long)timelineViewport_.getVisibleStartSamples(),
                    timelineViewport_.getSamplesPerPixel(),
                    displaySamples);
                // Frame-interval bookkeeping for the follow governor: the interval of the tick
                // *after* a follow pan includes that pan's repaint cost, which is exactly the
                // capacity signal the clean-frame rule needs.
                mainFollowGovernor_.noteFrameTick(juce::Time::getMillisecondCounterHiRes());
                maybeFollowMainArrangementPlayhead(displaySamples, true);
                // No ruler push: the overlay covers the ruler band and draws the marker itself.
            });
        addAndMakeVisible(*lanePlayheadOverlay_);
        refreshInstrumentUi();
        addAndMakeVisible(inspectorCollapsedKnob_);
        inspectorCollapsedKnob_.setVisible(false);
        PluginHostUiBindings::install({
            pluginHost_,
            trackLanesView,
            inspectorView_,
            *vst3PluginPickerCoordinator_,
            *this,
        });
        trackLanesView.setActiveEditToolProvider([this]() { return currentEditTool_; });

        trackLanesEditCoordinator_ = std::make_unique<TrackLanesEditCoordinator>(
            session,
            playbackEngine_,
            pluginHost_,
            trackLanesView,
            rulerView,
            inspectorView_,
            TrackLanesEditCoordinator::Callbacks{
                [this] { return recorder_.isRecording(); },
                [this] {
                    return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive();
                },
                [this](const juce::String& label, std::function<bool()> mutator) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableSessionEdit(label, std::move(mutator));
                    }
                },
                [this](const juce::String& label,
                       std::function<bool(std::optional<PluginUndoStepSides>&,
                                          std::optional<InstrumentTrackDeleteUndoSides>&)> mutator) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableTrackDelete(label, std::move(mutator));
                    }
                },
                [this] { syncViewportFromSession(); },
                [this](TrackId tid) { return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid); },
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
                },
                [this](TrackId tid) {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->resetWindowAndBookingIfOpenOnTrack(tid);
                    }
                },
                [this](TrackId tid) {
                    instrumentTimelineRowCoordinator_->tearDownExperimentalInstrumentTimelineUiForTrack(tid);
                },
                [this](TrackId tid) {
                    instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
                    instrumentRuntimeCoordinator_->removeMidiContentControllerForTrack(tid);
                },
                [this] { refreshInstrumentUi(); },
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid);
                },
                [this]() -> double {
                    if (juce::AudioIODevice* dev = deviceManager.getCurrentAudioDevice())
                    {
                        return dev->getCurrentSampleRate();
                    }
                    return 0.0;
                },
                [this] { return instrumentRuntimeCoordinator_->hasAnyKeyedInstrumentControllerActive(); },
                [this] { instrumentRuntimeCoordinator_->deactivateKeyedInstrumentControllersOnly(); },
                [this](const TrackId tid) {
                    if (recorder_.getArmedTrackId() == tid)
                    {
                        recorder_.disarm();
                    }
                },
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getOrCreateMidiContentControllerForTrack(tid);
                },
            });
        trackLanesEditCoordinator_->install();

        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->rewireInstrumentTrackRenameHandlers();
        }

        deviceManager.addChangeListener(this);
        transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
        ui_hang_watchdog::install();
        startTimerHz(10);
        syncViewportFromSession();
        if (midiEditorPresenter_ != nullptr)
        {
            midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
        }
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();

        // Stability C3: coordinators (delete/undo/load) trigger invariant checks through this
        // global registration; the context getters read live subsystem state at check time.
        stability_invariants::registerGlobalStabilityInvariantChecker(
            [this](const juce::String& reason)
            { return stability_invariants::verifyStableState(reason, buildStabilityInvariantContext()); });
    }

    ~TransportControlsContent() override
    {
        // P1J shutdown: bounded cancel + worker join + staging cleanup BEFORE the render
        // scheduler/engine teardown (the preparation may own in-flight render requests).
        portablePreparationWindow_.reset();
        if (portablePreparationService_ != nullptr)
        {
            portablePreparationService_->shutdownAndJoin();
            portablePreparationService_.reset();
        }

        // P1H shutdown: the policy ticker dies FIRST so no timer callback runs into the
        // scheduler/engine teardown below. Every timer/pending flag is runtime-only (§20) —
        // dropping them loses nothing persisted; staleness is re-derived on next load.
        if (proxyUpdatePolicyService_ != nullptr)
        {
            proxyUpdatePolicyService_->stopProductionTicker();
            proxyUpdatePolicyService_.reset();
        }

        // P1E shutdown order: cancel + join + tear down every render job BEFORE the instrument
        // hosts/coordinators the engine reaches into are destroyed. The scheduler itself (app-
        // owned) outlives this view; only the engine attachment ends here.
        proxyRenderScheduler_.detachEngineAndShutdownJobs();
        proxyRenderEngine_.reset();

        // P1G shutdown: unpublish every playback view, drain the audio callback, destroy
        // readers off-audio, stop the I/O thread — all BEFORE the instrument hosts that
        // hold the atomic view slots are destroyed below.
        if (proxyPlaybackCoordinator_ != nullptr)
        {
            proxyPlaybackCoordinator_->shutdown();
            proxyPlaybackCoordinator_.reset();
        }

        stability_invariants::registerGlobalStabilityInvariantChecker(nullptr);
        session.setOnTimelineRulerTimeDisplayChanged({});
        audioWaveformCache_.setOnPyramidReady({});
        playbackEngine_.setExperimentalInstrumentDeviceLifecycleHooks({}, {}, {});
        deviceManager.removeChangeListener(this);
        recordingCoordinator_->cancelCountIn();
        clearExperimentalInstrumentRuntimesPreserveBridgeOnly();
    }

    // [Message thread] Invoked only from `MainWindow` shortcut router (not from child
    // `keyPressed` — avoids duplicate `numpadRecordToggled` on one physical keypress).
    void invokeRecordToggleFromWindowShortcut() override
    {
        recordingCoordinator_->numpadRecordToggled();
    }
    // [Message thread] Space: when recording, commit (source tag `space`); else same as Play/Pause.
    void invokePlayPauseToggleFromWindowShortcut() override
    {
        transportPlayPauseStopController_->invokePlayPauseToggleFromWindowShortcut();
    }

    void invokeJumpToLeftLocatorFromWindowShortcut() override
    {
        if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
        {
            juce::Logger::writeToLog("[Shortcut] numpad1 ignored (recording or count-in)");
            if constexpr (transport_shortcut_diag::kEnabled)
            {
                transport_shortcut_diag::appendLine("jumpL ignored reason=recording-or-count-in");
            }
            return;
        }
        const std::int64_t L = session.getLeftLocatorSamples();
        const std::int64_t R = session.getRightLocatorSamples();
        if (R > L && R > 0)
        {
            if constexpr (transport_shortcut_diag::kEnabled)
            {
                transport_shortcut_diag::appendLine(
                    "jumpL seek L=" + juce::String(L) + " R=" + juce::String(R)
                    + " playheadBefore=" + juce::String(transport.readPlayheadSamplesForUi())
                    + " playing="
                    + juce::String(
                        transport.readPlaybackIntentForUi() == PlaybackIntent::Playing ? "Y" : "n"));
            }
            transport.requestSeek(L);
            // Snap every main-window current-time indicator to L in this same frame. Without the
            // re-anchor, a backwards jump smaller than the clock's hard-resync threshold
            // (~0.17 s) is filtered out by its monotonic smoothing, so a press landing near L
            // looks like it did nothing even though the transport did seek.
            uiPlayheadClock_.reanchorTo(L);
            // With Follow ON the view pans so L is visible even when stopped (the frame callback
            // below only auto-follows while playing).
            maybeFollowMainArrangementPlayhead((double)L, false);
            if (lanePlayheadOverlay_ != nullptr)
            {
                // The overlay draws both the lane line and the ruler marker segment.
                lanePlayheadOverlay_->snapFrameDisplaySamplesForSeek((double)L);
            }
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->notifyMidiEditorExternalTransportSeekIfOpen(L);
            }
            return;
        }
        juce::Logger::writeToLog("[Shortcut] numpad1 ignored: no valid locator range");
        if constexpr (transport_shortcut_diag::kEnabled)
        {
            transport_shortcut_diag::appendLine(
                "jumpL ignored reason=no-valid-locator-range L=" + juce::String(L)
                + " R=" + juce::String(R));
        }
    }

    void invokeDeleteSelectedPlacedClipFromWindowShortcut() override;
    void invokeCopySelectedClipFromWindowShortcut() override;
    void invokePasteClipFromWindowShortcut() override;

    void invokeUndoFromWindowShortcut() override
    {
        if (undoRedoCoordinator_ != nullptr)
        {
            undoRedoCoordinator_->invokeUndoFromWindowShortcut();
        }
    }

    void invokeRedoFromWindowShortcut() override
    {
        if (undoRedoCoordinator_ != nullptr)
        {
            undoRedoCoordinator_->invokeRedoFromWindowShortcut();
        }
    }

    void invokeSaveProjectFromWindowShortcut() override
    {
        if (projectIoCoordinator_ != nullptr)
        {
            projectIoCoordinator_->saveProject();
        }
    }

    bool invokeQuitUnsavedGuardFromWindow() override
    {
        if (projectIoCoordinator_ == nullptr)
        {
            return false;
        }
        return projectIoCoordinator_->interceptQuitForUnsavedChanges();
    }

    void invokeAutosaveRecoveryCheckFromStartup(const bool commandLineProjectOpenQueued) override
    {
        if (projectIoCoordinator_ != nullptr)
        {
            projectIoCoordinator_->offerAutosaveRecoveryOnStartup(commandLineProjectOpenQueued);
        }
    }

    void invokeLoadProjectFileFromStartup(const juce::File& projectFile) override
    {
        if (projectIoCoordinator_ != nullptr)
        {
            projectIoCoordinator_->loadProjectFromFile(projectFile);
        }
    }

    // SPIKE-01 (P0/P1A validation spike; removable): opens the hidden diagnostic panel. Reached
    // only from the `--spike01-state-capture` command line (Main.cpp); no product path calls it.
    void invokeStartSpike01StateCaptureProbeFromStartup(const juce::String& autoPlanId) override
    {
        if (spike01StateCapturePanel_ != nullptr)
        {
            spike01StateCapturePanel_->setVisible(true);
            spike01StateCapturePanel_->toFront(true);
            return;
        }
        Spike01PanelCallbacks cb;
        cb.appVersion = juce::JUCEApplication::getInstance() != nullptr
                            ? juce::JUCEApplication::getInstance()->getApplicationVersion()
                            : juce::String("unknown");
        cb.listInstrumentRuntimes = [this]() -> std::vector<Spike01RuntimeChoice> {
            std::vector<Spike01RuntimeChoice> out;
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                return out;
            }
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return out;
            }
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& tr = snap->getTrack(i);
                if (tr.getKind() != TrackKind::Instrument)
                {
                    continue;
                }
                auto* host = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tr.getId());
                Spike01RuntimeChoice c;
                c.trackId = tr.getId();
                c.label = tr.getName() + " — "
                          + (host != nullptr && host->hasInstrument() ? host->getInstrumentNameForUi()
                                                                      : juce::String("(no instrument)"));
                out.push_back(std::move(c));
            }
            return out;
        };
        cb.resolveHostForTrack = [this](const TrackId tid) -> ExperimentalInstrumentHost* {
            return instrumentRuntimeCoordinator_ != nullptr
                       ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid)
                       : nullptr;
        };
        cb.isTransportPlaying = [this] {
            return transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
        };
        // SPIKE-01B-M auto mode: transport control through the same controller the
        // transport strip uses (start only when not already playing; stop = stop button path).
        cb.startTransport = [this] {
            if (transportPlayPauseStopController_ != nullptr
                && transport.readPlaybackIntentForUi() != PlaybackIntent::Playing)
            {
                transportPlayPauseStopController_->togglePlayPauseFromUi();
            }
        };
        cb.stopTransport = [this] {
            if (transportPlayPauseStopController_ != nullptr
                && transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
            {
                transportPlayPauseStopController_->stopOrSeekFromStopButton();
            }
        };
        cb.seekTransport = [this](std::int64_t sampleIndex) { transport.requestSeek(sampleIndex); };
        cb.readCycleWrapCount = [this] { return transport.readCycleWrapCountForUi(); };
        // P1EF integration plan: the NARROW production service API only (job ownership and
        // request capture live in the application-owned scheduler + AppProxyRenderEngine —
        // the former P1D request builder moved there). No plugin instance crosses this seam.
        cb.requestProxyRender = [this](const TrackId tid) {
            return proxyRenderScheduler_.requestRender(tid);
        };
        cb.queryProxyJobStatus = [this](const TrackId tid) {
            return proxyRenderScheduler_.jobStatus(tid);
        };
        cb.queryProxyDestinationState = [this](const TrackId tid) {
            return proxyRenderScheduler_.destinationState(tid);
        };
        cb.getPublishedProxyMetadata
            = [this](const TrackId tid, ProjectFileProxyMetadataV20& out) -> bool {
            InstrumentTrackController* c
                = instrumentRuntimeCoordinator_ != nullptr
                      ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                      : nullptr;
            const auto* meta = c != nullptr ? c->getProxyMetadata() : nullptr;
            if (meta == nullptr)
            {
                return false;
            }
            out = *meta;
            return true;
        };
        cb.getProjectFolder = [this] { return session.getCurrentProjectFolder(); };
        // P1G integration plan: playback-source coordination test seams. Everything goes
        // through the production coordinator/session/exporter — the panel owns no state.
        cb.setProxyPrimaryForcedUnavailable = [this](const TrackId tid, const bool unavailable) {
            if (proxyPlaybackCoordinator_ != nullptr)
            {
                proxyPlaybackCoordinator_->setPrimaryForcedUnavailableForTests(tid, unavailable);
                proxyPlaybackCoordinator_->refreshDestination(tid);
            }
        };
        cb.queryProxyPlaybackRuntimeState = [this](const TrackId tid) -> int {
            return proxyPlaybackCoordinator_ != nullptr
                       ? (int)proxyPlaybackCoordinator_->runtimeStateForTrack(tid)
                       : -1;
        };
        cb.isProxyViewSelected = [this](const TrackId tid) -> bool {
            if (proxyPlaybackCoordinator_ == nullptr)
            {
                return false;
            }
            const auto view = proxyPlaybackCoordinator_->publishedViewForTrack(tid);
            return view != nullptr && view->useProxy;
        };
        cb.queryProxyReaderUnderrunCount = [this](const TrackId tid) -> std::int64_t {
            if (proxyPlaybackCoordinator_ == nullptr)
            {
                return -1;
            }
            const auto view = proxyPlaybackCoordinator_->publishedViewForTrack(tid);
            return (view != nullptr && view->reader != nullptr)
                       ? (std::int64_t)view->reader->underrunCount()
                       : -1;
        };
        cb.saveProjectNow = [this] {
            if (projectIoCoordinator_ == nullptr)
            {
                return false;
            }
            projectIoCoordinator_->saveProject();
            return !projectIoCoordinator_->isProjectDirty();
        };
        cb.runOfflineMixdownWav = [this](const juce::File& outputFile) -> juce::Result {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                return juce::Result::fail("no active audio device");
            }
            auto syncUi = [this] {
                if (transportPlayPauseStopController_ != nullptr)
                {
                    transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                }
            };
            mini_daw_audio_mixdown::MixdownExportRequest req;
            req.outputFile = outputFile;
            req.sampleRate = device->getCurrentSampleRate();
            req.bits = mini_daw_audio_mixdown::MixdownWaveBits::Pcm24;
            req.overwriteConfirmed = true;
            return mini_daw_audio_mixdown::exportStereoMixdownWavBlocking(
                transport, session, playbackEngine_, deviceManager, syncUi, req);
        };
        cb.setTrackMuted = [this](const TrackId tid, const bool muted) {
            session.setTrackMuted(tid, muted);
        };
        cb.listSoundProducingTracks = [this]() -> std::vector<TrackId> {
            std::vector<TrackId> out;
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return out;
            }
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& tr = snap->getTrack(i);
                if (tr.getKind() == TrackKind::Audio || tr.getKind() == TrackKind::Instrument)
                {
                    out.push_back(tr.getId());
                }
            }
            return out;
        };
        cb.appendStaleTestClip = [this](const TrackId tid) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr
                      ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                      : nullptr;
            if (c == nullptr)
            {
                return false;
            }
            // One real render-relevant note through the normal arrangement-import seam.
            TimelineMidiNote n;
            n.midiNote = 60;
            n.velocity = 100;
            n.channel = 1;
            n.startTick = 0;
            n.durationTicks = kDefaultExperimentalTicksPerQuarter / 2;
            const auto clipId = c->appendImportedTimelineMidiClipAtSamples(
                { n }, 0, "P1G stale-test clip");
            if (clipId == 0)
            {
                return false;
            }
            if (proxyPlaybackCoordinator_ != nullptr)
            {
                proxyPlaybackCoordinator_->refreshDestination(tid);
            }
            return true;
        };
        cb.getEngineSampleRate = [this]() -> double {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            return device != nullptr ? device->getCurrentSampleRate() : 0.0;
        };
        cb.trySetEngineSampleRate = [this](const double rate) -> bool {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                return false;
            }
            if (!device->getAvailableSampleRates().contains(rate))
            {
                juce::String rates;
                for (const double r : device->getAvailableSampleRates())
                {
                    rates << juce::String(r, 0) << " ";
                }
                appendMixdownDiagnosticLine("p1g: device \"" + device->getName()
                                            + "\" does not offer " + juce::String(rate, 0)
                                            + " Hz (available: " + rates.trim() + ")");
                return false;
            }
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.sampleRate = rate;
            const juce::String err = deviceManager.setAudioDeviceSetup(setup, false);
            if (err.isNotEmpty())
            {
                appendMixdownDiagnosticLine("p1g: setAudioDeviceSetup(" + juce::String(rate, 0)
                                            + ") failed: " + err);
                return false;
            }
            juce::AudioIODevice* const now = deviceManager.getCurrentAudioDevice();
            return now != nullptr && std::abs(now->getCurrentSampleRate() - rate) < 1.0;
        };
        // P1H integration plan: policy-service test seams. The injectable clock offset makes
        // the five-minute boundary deterministic (§18.1); everything else goes through the
        // production policy service / project I/O — the panel owns no policy state.
        cb.advanceProxyPolicyClockMs = [this](const double ms) {
            proxyPolicyTestClockOffsetMs_ += juce::jmax(0.0, ms);
            if (proxyUpdatePolicyService_ != nullptr)
            {
                proxyUpdatePolicyService_->tick();
            }
        };
        cb.queryProxyPolicyStatus = [this](const TrackId tid) {
            return proxyUpdatePolicyService_ != nullptr
                       ? proxyUpdatePolicyService_->statusForTrack(tid)
                       : proxy_policy::ProxyPolicyStatus{};
        };
        cb.setProxyUpdateMode = [this](const TrackId tid, const int modeComboIndex) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr
                      ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                      : nullptr;
            if (c == nullptr)
            {
                return false;
            }
            const auto mode
                = (proxy_policy::ProxyUpdateMode)juce::jlimit(0, 3, modeComboIndex);
            if (!c->setProxyUpdateModeFromUi(proxy_policy::proxyUpdateModePersistedString(mode)))
            {
                return false;
            }
            if (projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->markProjectDirtyFromEdit(); // §18.3 persisted mode
            }
            if (proxyUpdatePolicyService_ != nullptr)
            {
                proxyUpdatePolicyService_->tick();
            }
            return true;
        };
        cb.policyRenderNow = [this](const TrackId tid) -> bool {
            return proxyUpdatePolicyService_ != nullptr
                   && proxyUpdatePolicyService_->renderNow(tid);
        };
        cb.policyCancel = [this](const TrackId tid) {
            if (proxyUpdatePolicyService_ != nullptr)
            {
                proxyUpdatePolicyService_->cancel(tid);
            }
        };
        cb.forceAutosaveNow = [this]() -> bool {
            if (projectIoCoordinator_ == nullptr)
            {
                return false;
            }
            juce::String failReason;
            const bool ok = projectIoCoordinator_->forceAutosaveNowForStabilityTest(failReason);
            if (!ok)
            {
                appendMixdownDiagnosticLine("p1h: autosave skipped: " + failReason);
            }
            return ok;
        };
        cb.loadProjectNow = [this](const juce::File& f) {
            if (projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->loadProjectFromFile(f);
            }
        };
        cb.getProjectFile = [this] { return session.getCurrentProjectFile(); };
        // P1J integration plan: drive the PRODUCTION portable-preparation service.
        cb.portableStart = [this](const juce::File& dest) {
            return portablePreparationService_ != nullptr
                   && portablePreparationService_->start(dest);
        };
        cb.portablePhaseName = [this]() -> juce::String {
            return portablePreparationService_ != nullptr
                       ? juce::String(portable_project::preparationPhaseName(
                             portablePreparationService_->status().phase))
                       : juce::String("Idle");
        };
        cb.portableDetails = [this]() -> juce::String {
            if (portablePreparationService_ == nullptr)
            {
                return {};
            }
            const auto st = portablePreparationService_->status();
            return st.failureReason
                   + (st.blockers.isEmpty() ? juce::String()
                                            : " | " + st.blockers.joinIntoString(" | "));
        };
        cb.portableFinalFolder = [this]() -> juce::File {
            return portablePreparationService_ != nullptr
                       ? portablePreparationService_->status().finalFolder
                       : juce::File();
        };
        spike01StateCapturePanel_ = std::make_unique<Spike01StateCapturePanel>(std::move(cb),
                                                                               autoPlanId);
    }

    // Stability C2: build hooks over the real coordinators/views and start the scenario runner.
    // Every hook goes through the same entry point the UI uses (delete = header context-menu path,
    // undo/redo = window shortcut path, save = Ctrl+S path, mixdown = the real blocking exporter).
    void invokeStartStabilityScenarioFromStartup(const StabilityScenarioRequest& request) override
    {
        StabilityRunnerHooks hooks;
        hooks.loadProjectFromFile = [this](const juce::File& f) {
            if (projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->loadProjectFromFile(f);
            }
        };
        hooks.saveProject = [this] { invokeSaveProjectFromWindowShortcut(); };
        hooks.getTrackCount = [this]() -> int {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            return snap != nullptr ? snap->getNumTracks() : 0;
        };
        hooks.listDeletableTracks = [this]() -> std::vector<StabilityTrackInfo> {
            std::vector<StabilityTrackInfo> out;
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return out;
            }
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& tr = snap->getTrack(i);
                if (tr.getKind() == TrackKind::Master)
                {
                    continue;
                }
                StabilityTrackInfo info;
                info.id = tr.getId();
                info.name = tr.getName();
                info.isInstrument = tr.getKind() == TrackKind::Instrument;
                switch (tr.getKind())
                {
                    case TrackKind::Audio: info.kindName = "audio"; break;
                    case TrackKind::Instrument: info.kindName = "instrument"; break;
                    case TrackKind::Group: info.kindName = "group"; break;
                    case TrackKind::Master: info.kindName = "master"; break;
                    case TrackKind::Midi: info.kindName = "midi"; break;
                }
                out.push_back(std::move(info));
            }
            return out;
        };
        hooks.requestDeleteTrack = [this](const TrackId tid) {
            trackLanesView.requestDeleteTrackForHeaderMenu(tid);
        };
        hooks.invokeUndo = [this] { invokeUndoFromWindowShortcut(); };
        hooks.invokeRedo = [this] { invokeRedoFromWindowShortcut(); };
        hooks.setPlaybackActive = [this](const bool play) {
            transport.requestPlaybackIntent(play ? PlaybackIntent::Playing
                                                 : PlaybackIntent::Stopped);
            if (transportPlayPauseStopController_ != nullptr)
            {
                transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
            }
        };
        hooks.renameTrackUndoable = [this](const TrackId tid, juce::String name) -> bool {
            return trackLanesView.invokeUndoableRenameTrackRequested(tid, std::move(name));
        };
        hooks.openMidiEditorOnFirstClip = [this](const TrackId tid) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr || midiEditorPresenter_ == nullptr)
            {
                return false;
            }
            InstrumentTrackController* const ctl
                = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
            if (ctl == nullptr || ctl->getClips().empty() || ctl->getClips().front() == nullptr)
            {
                return false;
            }
            midiEditorPresenter_->openMidiEditorForInstrumentClip(tid, ctl->getClips().front()->id);
            return true;
        };
        hooks.closeMidiEditor = [this] {
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->resetWindowAndBooking();
            }
        };
        hooks.runMixdownBlocking = [this](const juce::File& outputFile,
                                          const bool mp3) -> juce::Result {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                return juce::Result::fail("no active audio device");
            }
            auto syncUi = [this] {
                if (transportPlayPauseStopController_ != nullptr)
                {
                    transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                }
            };
            // Normal users get the replace prompt in the mixdown dialog; scenario runs are
            // headless and deliberately re-export to the same temp output, so overwrite is
            // auto-confirmed here (and logged, so a diag trace never looks like a silent skip).
            if (outputFile.existsAsFile())
            {
                appendMixdownDiagnosticLine("stability test: overwrite auto-confirmed path=\""
                                            + outputFile.getFullPathName() + "\"");
            }
            if (mp3)
            {
                return mini_daw_audio_mixdown::exportStereoMixdownMp3Blocking(
                    transport, session, playbackEngine_, deviceManager, syncUi, outputFile,
                    /*bitrateKbps*/ 192, /*progressSink*/ nullptr, /*overwriteConfirmed*/ true);
            }
            mini_daw_audio_mixdown::MixdownExportRequest req;
            req.outputFile = outputFile;
            req.sampleRate = device->getCurrentSampleRate();
            req.bits = mini_daw_audio_mixdown::MixdownWaveBits::Pcm24;
            req.overwriteConfirmed = true;
            return mini_daw_audio_mixdown::exportStereoMixdownWavBlocking(
                transport, session, playbackEngine_, deviceManager, syncUi, req);
        };

        // Stability C3: run the full invariant battery after every scenario step.
        hooks.verifyInvariants = [this](const juce::String& reason)
        { return stability_invariants::verifyStableState(reason, buildStabilityInvariantContext()); };

        // Stability C5: autosave/recovery hooks over the real coordinator paths.
        hooks.forceAutosaveNow = [this](juce::String& failReason) -> bool {
            if (projectIoCoordinator_ == nullptr)
            {
                failReason = "no project IO coordinator";
                return false;
            }
            return projectIoCoordinator_->forceAutosaveNowForStabilityTest(failReason);
        };
        hooks.recoverAutosaveNow = [this](juce::String& failReason) -> bool {
            if (projectIoCoordinator_ == nullptr)
            {
                failReason = "no project IO coordinator";
                return false;
            }
            return projectIoCoordinator_->recoverAutosaveNowForStabilityTest(failReason);
        };
        hooks.getAutosaveFilePath = [this]() -> juce::File {
            return projectIoCoordinator_ != nullptr
                       ? projectIoCoordinator_->getCurrentAutosavePathForDiagnostics()
                       : juce::File{};
        };
        hooks.getAutosavePointerFilePath = []() -> juce::File {
            return ProjectIoCoordinator::getAutosavePointerPathForDiagnostics();
        };
        hooks.isProjectDirty = [this]() -> bool {
            return projectIoCoordinator_ != nullptr && projectIoCoordinator_->isProjectDirty();
        };
        hooks.getCurrentProjectPath = [this]() -> juce::String {
            return session.hasKnownProjectFile()
                       ? session.getCurrentProjectFile().getFullPathName()
                       : juce::String{};
        };

        // --- Phase B: MIDI-track routing scenario (capture seam; no real VST3 needed) ---
        hooks.midiRoutingFixtureSetup = [this](juce::String& failReason) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                failReason = "no instrument runtime coordinator";
                return false;
            }
            // Destination: plugin-less GrooveAgent shell; the capture sink makes it accept
            // transport MIDI without any plugin (Level-1 deterministic path).
            const auto instIdOpt = session.appendExperimentalInstrumentShellTrack("MidiRouteDest");
            if (!instIdOpt.has_value())
            {
                failReason = "could not append instrument shell row";
                return false;
            }
            stabilityMidiRoutingInstTid_ = *instIdOpt;
            const auto pr = instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(
                stabilityMidiRoutingInstTid_);
            if (pr.first == nullptr || pr.second == nullptr)
            {
                failReason = "could not create destination runtime";
                return false;
            }
            stabilityMidiRoutingCaptureSink_.reset();
            pr.first->installMidiDeliveryCaptureSinkForTests(&stabilityMidiRoutingCaptureSink_);
            stabilityMidiRoutingDestHost_ = pr.first;
            if (!pr.second->bootstrapGrooveAgentShellForSessionTrack(stabilityMidiRoutingInstTid_))
            {
                failReason = "could not bootstrap destination shell";
                return false;
            }

            const auto makeNotes = [](const std::vector<int>& pitches, const int nativeChannel) {
                constexpr int kTpq = kDefaultExperimentalTicksPerQuarter;
                std::vector<TimelineMidiNote> notes;
                int q = 0;
                for (const int pitch : pitches)
                {
                    TimelineMidiNote n;
                    n.midiNote = pitch;
                    n.velocity = 100;
                    n.channel = static_cast<std::uint8_t>(nativeChannel);
                    n.startTick = static_cast<std::int64_t>(q++) * kTpq;
                    n.durationTicks = kTpq / 2;
                    notes.push_back(n);
                }
                return notes;
            };

            const auto makeCcPair = [](const int nativeChannel, const int secondValue) {
                // Two CC11 Hold points: baseline 127 at tick 0, `secondValue` exactly on the
                // second note's start tick — same-offset CC-before-Note-On is exercised for real.
                MidiCcPoint a;
                a.startTick = 0;
                a.controller = 11;
                a.value = 127;
                a.channel = static_cast<std::uint8_t>(nativeChannel);
                a.interpolationToNext = MidiCcInterpolation::hold;
                MidiCcPoint b = a;
                b.startTick = kDefaultExperimentalTicksPerQuarter;
                b.value = static_cast<std::uint8_t>(secondValue);
                return std::vector<MidiCcPoint>{ a, b };
            };

            // Destination's OWN MIDI content: four quarter notes, native channel 1 (the
            // instrument track's default fixed output channel is 1, so they stay on channel 1).
            // Stage D: plus CC11 automation on the same native channel.
            {
                const std::vector<int> ownPitches{ 60, 62, 64, 65 };
                stabilityMidiRoutingExpectedOwnNotes_ = static_cast<int>(ownPitches.size());
                const InstrumentMidiClipId cid = pr.second->appendImportedTimelineMidiClipAtSamples(
                    makeNotes(ownPitches, 1), 0, "RouteOwn");
                if (cid == 0)
                {
                    failReason = "could not create the destination's own MIDI clip";
                    return false;
                }
                if (auto* c = pr.second->getClipById(cid))
                {
                    c->pattern.ccPoints = makeCcPair(1, 90);
                    pr.second->notifyClipPatternMutated(cid);
                }
                stabilityMidiRoutingExpectedOwnCc_ = 2;
            }

            // Two TrackKind::Midi sources routed to the SAME destination with distinct FIXED
            // output channels (2 and 3). Their stored native channels (5 and 6) differ from the
            // output channels on purpose: capture must see the effective channels while save/load
            // must preserve the native ones. Pitches include the range boundaries 0 and 127.
            const auto addSource = [this, &failReason, &makeNotes](
                                       const char* label,
                                       const int outputChannel,
                                       const int nativeChannel,
                                       const std::vector<int>& pitches,
                                       TrackId& outTid,
                                       const std::vector<MidiCcPoint>& ccPoints) -> bool {
                const auto midiIdOpt = session.addMidiTrack();
                if (!midiIdOpt.has_value())
                {
                    failReason = juce::String(label) + ": could not add Midi track row";
                    return false;
                }
                outTid = *midiIdOpt;
                InstrumentTrackController* const midiCtl
                    = instrumentRuntimeCoordinator_->getOrCreateMidiContentControllerForTrack(outTid);
                if (midiCtl == nullptr)
                {
                    failReason = juce::String(label) + ": could not create midi content controller";
                    return false;
                }
                if (!session.setTrackMidiDestination(outTid, stabilityMidiRoutingInstTid_))
                {
                    failReason = juce::String(label) + ": setTrackMidiDestination refused";
                    return false;
                }
                if (!session.setTrackMidiOutputChannel(outTid, outputChannel))
                {
                    failReason = juce::String(label) + ": setTrackMidiOutputChannel refused";
                    return false;
                }
                midiCtl->refreshMidiOutputChannelFromSession();
                const InstrumentMidiClipId cid = midiCtl->appendImportedTimelineMidiClipAtSamples(
                    makeNotes(pitches, nativeChannel), 0, label);
                if (cid == 0)
                {
                    failReason = juce::String(label) + ": could not create MIDI clip";
                    return false;
                }
                if (!ccPoints.empty())
                {
                    if (auto* c = midiCtl->getClipById(cid))
                    {
                        c->pattern.ccPoints = ccPoints;
                        midiCtl->notifyClipPatternMutated(cid);
                    }
                }
                return true;
            };
            // Lower additionally carries CC11 stored on native channel 5: delivery must arrive on
            // the EFFECTIVE fixed channel 2 (routed MIDI-only source; Pedal stays CC-free so
            // channel 3 proves streams do not leak).
            if (!addSource("Lower", 2, 5, { 0, 48, 50, 52 }, stabilityMidiRoutingMidiLowerTid_,
                           makeCcPair(5, 80)))
            {
                return false;
            }
            stabilityMidiRoutingExpectedLowerNotes_ = 4;
            stabilityMidiRoutingExpectedLowerCc_ = 2;
            if (!addSource("Pedal", 3, 6, { 127, 36, 38 }, stabilityMidiRoutingMidiPedalTid_, {}))
            {
                return false;
            }
            stabilityMidiRoutingExpectedPedalNotes_ = 3;

            // Same post-add sync as the UI add-track menu: rebuilds the routing plan (track
            // indices shifted) and attaches the new timeline rows.
            syncViewportFromSession();
            trackLanesView.syncTracksFromSession();
            refreshInstrumentUi();
            inspectorView_.refreshFromSession();
            transport.requestSeek(0);
            appendStabilityRunLine(
                "  fixture: instTid=" + juce::String((juce::int64)stabilityMidiRoutingInstTid_)
                + " lowerTid=" + juce::String((juce::int64)stabilityMidiRoutingMidiLowerTid_)
                + " pedalTid=" + juce::String((juce::int64)stabilityMidiRoutingMidiPedalTid_)
                + " expected ch1/ch2/ch3=" + juce::String(stabilityMidiRoutingExpectedOwnNotes_)
                + "/" + juce::String(stabilityMidiRoutingExpectedLowerNotes_) + "/"
                + juce::String(stabilityMidiRoutingExpectedPedalNotes_));
            return true;
        };
        // Shared Phase B.1 assertion set: exact per-channel counts prove three distinct streams
        // reach ONE destination with channels 1/2/3 kept apart, and that no per-source duplicate
        // processing happens (a double-invoked boundary would double the counts). Used for both
        // the realtime playback pass and the offline mixdown parity pass.
        hooks.midiRoutingVerifyDelivery = [this](juce::String& failReason) -> bool {
            return stabilityVerifyMidiRoutingCapture("realtime", failReason,
                                                     /*requireStopFlushOffs=*/true);
        };
        // Copy (not reference) of the mixdown hook: `hooks` is moved into the runner below.
        hooks.midiRoutingRunOfflineParity = [this, runMixdown = hooks.runMixdownBlocking](
                                                juce::String& failReason) -> bool {
            if (runMixdown == nullptr)
            {
                failReason = "no mixdown hook";
                return false;
            }
            // Active loop range over the fixture notes (mixdown renders the loop span only).
            const double sr = [this] {
                juce::AudioIODevice* const d = deviceManager.getCurrentAudioDevice();
                return d != nullptr && d->getCurrentSampleRate() > 0.0 ? d->getCurrentSampleRate()
                                                                       : 48000.0;
            }();
            session.setLeftLocatorAtSample(0);
            session.setRightLocatorAtSample((std::int64_t)(sr * 4.0));
            transport.requestCycleEnabled(true);
            stabilityMidiRoutingCaptureSink_.reset();
            const juce::File out = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("minidaw-midirouting-offline.wav");
            const juce::Result r = runMixdown(out, /*mp3=*/false);
            (void)out.deleteFile();
            transport.requestCycleEnabled(false);
            if (r.failed())
            {
                failReason = "offline mixdown failed: " + r.getErrorMessage();
                return false;
            }
            // Offline stop is implicit (bounded render), so scheduled note-offs inside the span
            // must balance; the equivalence claim is the per-channel note-on counts.
            return stabilityVerifyMidiRoutingCapture("offline", failReason,
                                                     /*requireStopFlushOffs=*/true);
        };
        hooks.midiRoutingVerifyAfterReload = [this](juce::String& failReason) -> bool {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                failReason = "no session snapshot after reload";
                return false;
            }
            // Ids are stable across save/load (v18 writes track ids). Verify BOTH MIDI sources:
            // destination id, fixed output channel, note count, exact stored pitches and native
            // channels — the full-range editor must never rewrite persisted MIDI pitches, incl.
            // the boundary pitches 0 (C-2) and 127 (G8).
            struct ExpectedSource
            {
                TrackId tid;
                int outputChannel;
                std::vector<int> pitches;
                int nativeChannel;
                const char* label;
            };
            const ExpectedSource expected[] = {
                { stabilityMidiRoutingMidiLowerTid_, 2, { 0, 48, 50, 52 }, 5, "Lower" },
                { stabilityMidiRoutingMidiPedalTid_, 3, { 127, 36, 38 }, 6, "Pedal" },
            };
            for (const auto& exp : expected)
            {
                const int mix = snap->findTrackIndexById(exp.tid);
                if (mix < 0 || snap->getTrack(mix).getKind() != TrackKind::Midi)
                {
                    failReason = juce::String(exp.label) + ": Midi row missing or wrong kind after reload";
                    return false;
                }
                if (snap->getTrack(mix).getMidiDestinationTrackId() != stabilityMidiRoutingInstTid_)
                {
                    failReason = juce::String(exp.label) + ": Midi destination not restored after reload";
                    return false;
                }
                if (snap->getTrack(mix).getMidiOutputChannel() != exp.outputChannel)
                {
                    failReason = juce::String(exp.label) + ": output channel not restored (expected "
                                 + juce::String(exp.outputChannel) + ", got "
                                 + juce::String(snap->getTrack(mix).getMidiOutputChannel()) + ")";
                    return false;
                }
                InstrumentTrackController* const midiCtl
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getMidiContentControllerForTrack(exp.tid)
                          : nullptr;
                if (midiCtl == nullptr)
                {
                    failReason = juce::String(exp.label) + ": midi content controller missing after reload";
                    return false;
                }
                std::vector<int> pitches;
                for (const auto& cp : midiCtl->getClips())
                {
                    if (cp == nullptr)
                    {
                        continue;
                    }
                    for (const auto& n : cp->pattern.timelineNotes)
                    {
                        pitches.push_back(n.midiNote);
                        if ((int)n.channel != exp.nativeChannel)
                        {
                            failReason = juce::String(exp.label) + ": native channel rewritten (expected "
                                         + juce::String(exp.nativeChannel) + ", found "
                                         + juce::String((int)n.channel) + ")";
                            return false;
                        }
                    }
                }
                std::vector<int> want = exp.pitches;
                std::sort(pitches.begin(), pitches.end());
                std::sort(want.begin(), want.end());
                if (pitches != want)
                {
                    failReason = juce::String(exp.label)
                                 + ": stored pitches changed across save/load (full-range editor must "
                                   "be non-destructive)";
                    return false;
                }
            }
            // The destination's own clip survives too (its controller path predates Phase B).
            InstrumentTrackController* const destCtl
                = instrumentRuntimeCoordinator_ != nullptr
                      ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(
                            stabilityMidiRoutingInstTid_)
                      : nullptr;
            int ownNotes = 0;
            if (destCtl != nullptr)
            {
                for (const auto& cp : destCtl->getClips())
                {
                    if (cp != nullptr)
                    {
                        ownNotes += (int)cp->pattern.timelineNotes.size();
                    }
                }
            }
            if (ownNotes != stabilityMidiRoutingExpectedOwnNotes_)
            {
                failReason = "destination's own clip notes not restored (expected "
                             + juce::String(stabilityMidiRoutingExpectedOwnNotes_) + ", found "
                             + juce::String(ownNotes) + ")";
                return false;
            }
            return true;
        };

        stabilityScenarioRunner_ = std::make_unique<StabilityScenarioRunner>(std::move(hooks));
        stabilityScenarioRunner_->start(request);
    }

    // [Message thread] Transient non-modal "Saving project" indicator. Painted immediately (before
    // the synchronous project write blocks the message loop), auto-hidden shortly after. Purely
    // informational — save errors still surface through the existing alert dialogs.
    void showSavingProjectToast()
    {
        // The active window shows the feedback: when the save came from the MIDI editor (Ctrl+S
        // there, or a menu save while it is focused), mirror the toast in that window too.
        if (midiEditorWindow_ != nullptr && midiEditorWindow_->isShowing()
            && midiEditorWindow_->isActiveWindow())
        {
            midiEditorWindow_->showSavingProjectToast();
        }
        constexpr int kToastW = 170;
        constexpr int kToastH = 34;
        savingProjectToastLabel_.setBounds(
            juce::jmax(0, (getWidth() - kToastW) / 2), 48, kToastW, kToastH);
        savingProjectToastLabel_.setVisible(true);
        savingProjectToastLabel_.toFront(false);
        if (auto* peer = getPeer())
        {
            peer->performAnyPendingRepaintsNow();
        }
        juce::Component::SafePointer<juce::Label> toast(&savingProjectToastLabel_);
        juce::Timer::callAfterDelay(1300, [toast] {
            if (toast != nullptr)
            {
                toast->setVisible(false);
            }
        });
    }

    void setKeyDiagnosticLine(const juce::String& line) override
    {
        if (shortcut_diagnostics::kShowKeyDiagnostic)
        {
            keyDiagLabel_.setText(line, juce::dontSendNotification);
        }
    }

    void setShortcutDiagVisibleCaption(const juce::String& line) override
    {
        if constexpr (shortcut_diagnostics::kShowShortcutDiagnostics)
        {
            if (shortcutDiagLabel_)
            {
                shortcutDiagLabel_->setText(line, juce::dontSendNotification);
            }
        }
        else
        {
            juce::ignoreUnused(line);
        }
    }

    // [Message thread] Keeps `currentEditTool_`, lane repaint, and strip highlights aligned.
    void applyEditToolSelection(EditTool t) noexcept
    {
        currentEditTool_ = t;
        editToolIconStrip_.setSelectedTool(t, juce::dontSendNotification);
        trackLanesView.repaint();
    }

    // [Message thread] Layout: one row of buttons, fixed-height time ruler, then event lane.
    void resized() override
    {
        instrumentRuntimeCoordinator_->syncAllKeyedAndStagingShellWithHostState();
        applyTransportControlsLayout(TransportLayoutRefs{
            *this,
            rulerView,
            trackLanesView,
            inspectorView_,
            *menuBar_,
            addTrackCornerPlusButton_,
            editToolIconStrip_,
            arrangementBpmLabel_,
            arrangementBpmEditor_,
            arrangementTimeSignatureCombo_,
            arrangementTimelineFormatCombo_,
            arrangementSnapToggle_,
            arrangementSnapResolutionCombo_,
            mainFollowPlayheadToggle_,
            countInStatusLabel_,
            keyDiagLabel_,
            shortcutDiagLabel_.get(),
            inspectorCurrentWidth_,
            inspectorResizeSplitter_,
            inspectorCollapsedKnob_,
            lanePlayheadOverlay_.get(),
        });
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        juce::ignoreUnused(source);
        // Multi-line device detail is logged once at app init; on change, persist is best-effort.
        mini_daw::trySaveAudioDeviceState(deviceManager, mini_daw::getAudioSettingsFile());
        latencyStore_.refreshFromCurrentDevice();
        latencyStore_.save();
        playbackEngine_.setPlaybackOffsetSamples(latencyStore_.getCurrentPlaybackOffsetSamples());
        if (midiEditorPresenter_ != nullptr)
        {
            midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
        }
        if (auto* lv = audioLatencySettingsWeak_.getComponent())
        {
            lv->syncFromStore();
        }
        // P1G (PI-030): an engine/device-rate change rebuilds proxy readers as derived
        // state — generation currency is untouched; status shows ProxyPreparing until
        // the new-rate representation is primed.
        if (proxyPlaybackCoordinator_ != nullptr)
        {
            proxyPlaybackCoordinator_->notifyEngineRateMaybeChanged();
        }
    }

    void showAudioMixdownDialog()
    {
        // Slice 5 dirty guard now runs when the user clicks Export inside the dialog (not at
        // dialog open), so settings can be adjusted first; the dialog closes once export starts.
        const auto confirmSaveBeforeExport = [this](std::function<void()> proceed) {
            if (projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->confirmUnsavedChangesThen(
                    ProjectIoCoordinator::UnsavedGuardKind::Export, std::move(proceed));
            }
            else if (proceed != nullptr)
            {
                proceed();
            }
        };
        mini_daw_app_dialogs::showAudioMixdownDialog(*this,
                                                     transport,
                                                     session,
                                                     playbackEngine_,
                                                     deviceManager,
                                                     [this]() {
                                                         transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                                                     },
                                                     confirmSaveBeforeExport);
    }

    // ---------------------------------------------------------------- P1J UI
    // "Prepare Portable Project..." (§16.6): explicit user operation. The flow
    // guarantees a coherent SAVED project before capture (never a mixture of
    // saved and live state), asks for a destination, then starts the service.
    void startPreparePortableProjectFlow()
    {
        if (portablePreparationService_ == nullptr)
        {
            return;
        }
        if (portablePreparationService_->running())
        {
            openPortablePreparationWindow(); // one operation at a time
            return;
        }
        const bool needsSave = !session.hasKnownProjectFile()
                               || (projectIoCoordinator_ != nullptr
                                   && projectIoCoordinator_->isProjectDirty());
        if (needsSave && projectIoCoordinator_ != nullptr)
        {
            // Explicit Save (with chooser on first save) — the portable package
            // is prepared from the saved project state only.
            projectIoCoordinator_->saveProjectThen([this](const bool saved) {
                if (saved)
                {
                    choosePortableDestinationAndStart();
                }
            });
            return;
        }
        choosePortableDestinationAndStart();
    }

    void choosePortableDestinationAndStart()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Prepare Portable Project: choose where to create the portable folder",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                const juce::File parent = fc.getResult();
                if (parent == juce::File())
                {
                    return; // user cancelled the chooser
                }
                const juce::String name
                    = session.getCurrentProjectFile().getFileNameWithoutExtension()
                      + " Portable";
                startPortablePreparationInto(parent.getChildFile(name));
            });
    }

    void startPortablePreparationInto(const juce::File& finalFolder)
    {
        if (portablePreparationService_ != nullptr
            && portablePreparationService_->start(finalFolder))
        {
            openPortablePreparationWindow();
        }
    }

    void openPortablePreparationWindow()
    {
        if (portablePreparationWindow_ == nullptr)
        {
            PortablePreparationWindow::Callbacks cb;
            cb.getStatus = [this]() -> portable_project::PreparationStatus {
                return portablePreparationService_ != nullptr
                           ? portablePreparationService_->status()
                           : portable_project::PreparationStatus{};
            };
            cb.cancelOperation = [this] {
                if (portablePreparationService_ != nullptr)
                {
                    portablePreparationService_->cancel();
                }
            };
            cb.restartOperation = [this] { startPreparePortableProjectFlow(); };
            cb.onWindowClosed = [this] {
                // View only: hiding the window never stops the operation (the
                // service owns the lifetime; reopen via the File menu).
                if (portablePreparationWindow_ != nullptr)
                {
                    portablePreparationWindow_->setVisible(false);
                }
            };
            portablePreparationWindow_
                = std::make_unique<PortablePreparationWindow>(std::move(cb));
        }
        portablePreparationWindow_->setVisible(true);
        portablePreparationWindow_->toFront(true);
        portablePreparationWindow_->refreshNow();
    }

    void showAudioSettingsDialog()
    {
        mini_daw_app_dialogs::showAudioSettingsDialog(*this,
                                                      transport,
                                                      [this]() {
                                                          transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                                                      },
                                                      recorder_,
                                                      *recordingCoordinator_,
                                                      deviceManager,
                                                      latencyStore_,
                                                      playbackEngine_,
                                                      audioLatencySettingsWeak_);
    }

    void showHelpMenuPopup()
    {
        mini_daw_app_dialogs::showHelpMenuPopup(
            *menuBar_,
            this,
            []() { mini_daw_app_dialogs::showUndoBehaviorDialog(); },
            [this]() { mini_daw_app_dialogs::showMidiChannelsHelpDialog(*this); });
    }

    void timerCallback() override
    {
        // Secondary watchdog heartbeat: keeps the stall detector alive when playback (and thereby
        // the 60 Hz overlay frame callback) is stopped.
        ui_hang_watchdog::heartbeat();
        // P1E §14.3 resource policy: recording pauses starting/progressing background proxy
        // rendering. Pushed as an immutable flag on this 10 Hz message-thread tick (transport
        // PLAYBACK intentionally never pauses rendering — measured default, revision 6).
        proxyRenderScheduler_.notifyRecordingState(
            recorder_.isRecording()
            || (recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive()));
        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->tickStructuralEditBlockedHeaderStripRepaint(
                trackLanesView.isStructuralTimelineEditBlocked());
        }
        transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
        inspectorView_.refreshFromSession();
        updateMainWindowTitleWithDirtyState();
        tickPlaybackUiLoadDiagnostics();
    }

    /// Part E: one aggregated `playback-ui-load.log` line per second (audio callback cost + UI
    /// playhead timer/invalidation), so UI render jitter can be separated from audio load. Compiled
    /// out unless `MINIDAW_DIAG_PLAYBACK_UI_LOAD` is 1.
    void tickPlaybackUiLoadDiagnostics()
    {
#if MINIDAW_DIAG_PLAYBACK_UI_LOAD
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (lastPlaybackUiLoadLogMs_ > 0.0 && nowMs - lastPlaybackUiLoadLogMs_ < 1000.0)
        {
            return;
        }
        lastPlaybackUiLoadLogMs_ = nowMs;

        const auto audio = playbackEngine_.snapshotAudioCallbackLoadAndReset();
        PlayheadOverlay::UiRenderStats ui;
        if (lanePlayheadOverlay_ != nullptr)
        {
            ui = lanePlayheadOverlay_->snapshotUiRenderStatsAndReset();
        }
        if (audio.blocks == 0 && ui.timerTicks == 0)
        {
            return;
        }
        const auto snap = session.loadSessionSnapshotForAudioThread();
        const int trackCount = snap != nullptr ? snap->getNumTracks() : 0;
        const bool playing = transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
        appendPlaybackUiLoadDiagnosticLine(
            juce::String("playing=") + (playing ? "1" : "0")
            + " tracks=" + juce::String(trackCount)
            + " audio.blocks=" + juce::String((int)audio.blocks)
            + " audio.ms(min/mean/max)=" + juce::String(audio.minMs, 3) + "/"
            + juce::String(audio.meanMs, 3) + "/" + juce::String(audio.maxMs, 3)
            + " audio.budget%(mean/max)=" + juce::String(audio.meanBudgetPercent, 1) + "/"
            + juce::String(audio.maxBudgetPercent, 1)
            + " audio.nearOverruns=" + juce::String((int)audio.nearOverruns)
            + " audio.overruns=" + juce::String((int)audio.overruns)
            + " audio.block=" + juce::String(audio.lastBlockSamples)
            + " audio.sr=" + juce::String(audio.sampleRate, 0)
            + " ui.ticks=" + juce::String(ui.timerTicks)
            + " ui.repaints=" + juce::String(ui.repaintRequests)
            + " ui.repaintAreaPx=" + juce::String(ui.repaintAreaPx, 0)
            + " ui.tickMs(min/mean/max)=" + juce::String(ui.timerIntervalMinMs, 1) + "/"
            + juce::String(ui.timerIntervalMeanMs, 1) + "/"
            + juce::String(ui.timerIntervalMaxMs, 1)
            + " ui.frameSample=" + juce::String(ui.lastFrameDisplaySamples, 0)
            + " ui.frameX=" + juce::String(ui.lastFrameCentreX, 1)
            + " follow.on=" + (mainFollowPlayhead_ ? "1" : "0")
            + " follow.pages=" + juce::String((int)statsFollowPans_)
            + " follow.skip.gesture=" + juce::String((int)statsFollowSkipsGesture_)
            + " follow.skip.late=" + juce::String((int)statsFollowSkipsLateFrame_)
            + " follow.skip.clean=" + juce::String((int)statsFollowSkipsAwaitClean_)
            + " follow.skip.boundary=" + juce::String((int)statsFollowSkipsBoundary_)
            + " follow.skip.pace=" + juce::String((int)statsFollowSkipsPacing_)
            + " follow.skip.xwin=" + juce::String((int)statsFollowSkipsCrossWindow_)
            + " follow.skip.budget=" + juce::String((int)statsFollowSkipsGlobalBudget_)
            + " viewport.changes=" + juce::String((int)statsViewportChanges_)
            + " viewport.flushes=" + juce::String((int)statsViewportRepaintFlushes_)
            + " follow.global.pages=" + juce::String(
                (int)(GlobalFollowWorkCoordinator::instance().totalPagesApplied()
                      - lastGlobalFollowPagesSnapshot_))
            + " follow.span=" + juce::String(
                (double)rulerView.getWidth() * timelineViewport_.getSamplesPerPixel(), 0)
            + " follow.frameMs=" + juce::String(mainFollowGovernor_.lastFrameIntervalMs(), 1)
            + " vp.spp=" + juce::String(timelineViewport_.getSamplesPerPixel(), 2)
            + " vp.visStart=" + juce::String(timelineViewport_.getVisibleStartSamples())
            + paintLoadCountersDiagSuffix());
        statsFollowPans_ = 0;
        statsFollowSkipsGesture_ = 0;
        statsFollowSkipsLateFrame_ = 0;
        statsFollowSkipsAwaitClean_ = 0;
        statsFollowSkipsPacing_ = 0;
        statsFollowSkipsBoundary_ = 0;
        statsFollowSkipsCrossWindow_ = 0;
        statsFollowSkipsGlobalBudget_ = 0;
        statsViewportChanges_ = 0;
        statsViewportRepaintFlushes_ = 0;
        lastGlobalFollowPagesSnapshot_ = GlobalFollowWorkCoordinator::instance().totalPagesApplied();
#endif
    }

#if MINIDAW_DIAG_PLAYBACK_UI_LOAD
    /// Zoom-freeze forensic audit: per-second paint counters per main-window component (see
    /// `UiPaintLoadCounters.h`) appended to the 1 Hz ui-load line to prove which paint path
    /// saturates during playback + zoom.
    [[nodiscard]] static juce::String paintLoadCountersDiagSuffix()
    {
        const auto p = ui_paint_load::snapshotAndReset();
        return juce::String(" paint.clipLane=") + juce::String((int)p.clipLanePaints)
               + " paint.raster=" + juce::String((int)p.clipLaneRasterRebuilds)
               + " paint.rasterUs=" + juce::String((juce::int64)p.clipLaneRasterRebuildUs)
               + " paint.uncached=" + juce::String((int)p.clipLaneUncachedPaints)
               + " paint.staleBlit=" + juce::String((int)p.clipLaneStaleBlits)
               + " paint.deferBuild=" + juce::String((int)p.clipLaneDeferredBuilds)
               + " paint.midiLane=" + juce::String((int)p.midiLanePaints)
               + " paint.midiNotes=" + juce::String((juce::int64)p.midiLaneNoteIterations)
               + " paint.lanes=" + juce::String((int)p.lanesViewPaints)
               + " paint.ruler=" + juce::String((int)p.rulerPaints)
               + " paint.overlay=" + juce::String((int)p.overlayPaints);
    }
#endif

    /// Stability Slice 5 (Part E): "<ProjectName>[*] - <AppName>" window title; the asterisk marks
    /// unsaved changes. Polled from the 10 Hz UI timer; setName only runs when the text changes.
    void updateMainWindowTitleWithDirtyState()
    {
        if (projectIoCoordinator_ == nullptr)
        {
            return;
        }
        auto* dw = findParentComponentOfClass<juce::DocumentWindow>();
        if (dw == nullptr)
        {
            return;
        }
        juce::String appName = "Danielssons Audio Lab";
        if (auto* app = juce::JUCEApplication::getInstance())
        {
            appName = app->getApplicationName();
        }
        const juce::File pf = session.getCurrentProjectFile();
        const juce::String projectPart
            = pf.getFullPathName().isNotEmpty() ? pf.getFileNameWithoutExtension()
                                                : juce::String("Untitled");
        const juce::String title = projectPart
                                   + (projectIoCoordinator_->isProjectDirty() ? "*" : "") + " - "
                                   + appName;
        if (dw->getName() != title)
        {
            dw->setName(title);
        }
    }

    // [Message thread] Seed default arrangement + samples-per-pixel once sample rate is known;
    // clamp the pan window to the current arrangement extent (when ruler width is known).
    void syncViewportFromSession()
    {
        juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
        if (dev != nullptr)
        {
            const double sr = dev->getCurrentSampleRate();
            if (sr > 0.0)
            {
                if (session.getStoredArrangementExtentSamples() == 0
                    && session.getContentEndSamples() == 0)
                {
                    session.setArrangementExtentSamples(
                        (std::int64_t)std::llround(3600.0 * sr));
                }
                // Default: 10 pixels per second of session time; visible **length in samples** is
                // derived as `round(rulerWidthPx * (sr/10))` and grows/shrinks with window width.
                constexpr double kDefaultPixelsPerSecond = 10.0;
                timelineViewport_.setSamplesPerPixelIfUnset(sr / kDefaultPixelsPerSecond);
            }
        }
        {
            const double rw = (double)rulerView.getWidth();
            if (rw > 0.0)
            {
                timelineViewport_.clampToExtent(rw, session.getArrangementExtentSamples());
            }
        }
        playbackEngine_.rebuildRoutingPlanFromSession();
    }

    // [Message thread] Conny follow-playhead: edge-style follow for the main arrangement — same
    // convention as the MIDI editor's piano roll (only pan when the playhead nears the right or
    // left edge; reset it to 20 % / 25 % from the left). Called once per playhead UI frame from
    // the `PlayheadOverlay` frame callback (`fromPlayheadFrame = true`), and explicitly on
    // jump-to-left-locator / Follow toggled ON (`fromPlayheadFrame = false`). `panBySamples`
    // clamps to the arrangement extent and its change callback repaints ruler + lanes.
    //
    // Freeze hardening (page-follow slice): follow is **page/event-driven**, never frame-driven.
    // Each follow page forces a full repaint of the whole arrangement column, so page admission is
    // delegated to `FollowAutoscrollGovernor` (boundary trigger + re-arm, capacity gates, local
    // gesture holdoff) plus `GlobalFollowWorkCoordinator` (one page across *all* DAL windows per
    // 250 ms; yield while the user pans/zooms another window). A page that cannot capture the
    // playhead (extent clamp, fine zoom) switches the governor to a sparse re-arm interval instead
    // of re-firing every opportunity — the edge condition alone must never be a standing pan
    // permission (see MAIN_FOLLOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md). At most one page can occur
    // per frame tick (single call site in the overlay frame callback), and `followPanInProgress_`
    // keeps the viewport listener from mistaking follow pages for user gestures. Explicit calls
    // (seek, Follow toggled ON) bypass the gates — single user-initiated adjustments — but still
    // register via `notePageApplied` on both objects so frame-driven paging pauses right after.
    void maybeFollowMainArrangementPlayhead(const double displaySamples, const bool fromPlayheadFrame)
    {
        constexpr double kFollowRightThreshold = 0.92;
        constexpr double kFollowLeftThreshold = 0.08;
        constexpr double kFollowForwardResetPosition = 0.20;
        constexpr double kFollowBackwardResetPosition = 0.25;

        if (!mainFollowPlayhead_ || followPanInProgress_)
        {
            return;
        }
        if (fromPlayheadFrame && transport.readPlaybackIntentForUi() != PlaybackIntent::Playing)
        {
            return;
        }
        const double w = (double)rulerView.getWidth();
        const double spp = timelineViewport_.getSamplesPerPixel();
        const std::int64_t arr = session.getArrangementExtentSamples();
        if (w <= 0.0 || spp <= 0.0 || !std::isfinite(spp) || arr <= 0)
        {
            return;
        }
        const double span = w * spp;
        const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
        const double rel = span > 1e-9 ? (displaySamples - (double)visStart) / span : 0.5;

        double target = 0.0;
        if (rel >= kFollowRightThreshold)
        {
            target = displaySamples - kFollowForwardResetPosition * span;
        }
        else if (rel <= kFollowLeftThreshold)
        {
            target = displaySamples - kFollowBackwardResetPosition * span;
        }
        else
        {
            return;
        }
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (fromPlayheadFrame)
        {
            switch (mainFollowGovernor_.decidePage(nowMs, displaySamples, (double)visStart, span))
            {
                case FollowAutoscrollGovernor::Decision::apply:
                    break;
                case FollowAutoscrollGovernor::Decision::skipNotNeeded:
                    return;
                case FollowAutoscrollGovernor::Decision::skipUserGestureHoldoff:
                    ++statsFollowSkipsGesture_;
                    return;
                case FollowAutoscrollGovernor::Decision::skipLateFrame:
                    ++statsFollowSkipsLateFrame_;
                    return;
                case FollowAutoscrollGovernor::Decision::skipAwaitCleanFrame:
                    ++statsFollowSkipsAwaitClean_;
                    return;
                case FollowAutoscrollGovernor::Decision::skipBoundaryWait:
                    ++statsFollowSkipsBoundary_;
                    return;
                case FollowAutoscrollGovernor::Decision::skipMinInterval:
                    ++statsFollowSkipsPacing_;
                    return;
            }
            auto& global = GlobalFollowWorkCoordinator::instance();
            if (global.otherWindowGestureActive(this, nowMs))
            {
                ++statsFollowSkipsCrossWindow_;
                return;
            }
            if (!global.pageSlotAvailable(nowMs))
            {
                ++statsFollowSkipsGlobalBudget_;
                return;
            }
        }
        const std::int64_t delta
            = (std::int64_t)std::llround(juce::jmax(0.0, target)) - visStart;
        if (delta != 0)
        {
            const juce::ScopedValueSetter<bool> reentrancyGuard(followPanInProgress_, true);
            timelineViewport_.panBySamples(delta, w, arr);
            mainFollowGovernor_.notePageApplied(
                nowMs, displaySamples, (double)timelineViewport_.getVisibleStartSamples(), span);
            GlobalFollowWorkCoordinator::instance().notePageApplied(nowMs);
            ++statsFollowPans_;
            ui_hang_watchdog::noteFollowPan();
        }
        else if (fromPlayheadFrame)
        {
            // Page attempted but the viewport could not move (fully clamped, e.g. arrangement
            // end): record it so the governor drops to the sparse re-arm cadence instead of
            // re-deciding every minimum interval for as long as the playhead stays outside.
            mainFollowGovernor_.notePageApplied(nowMs, displaySamples, (double)visStart, span);
        }
    }

    // Stability C3: live-context getters for `stability_invariants::verifyStableState`. Each
    // getter is evaluated at check time on the message thread; all members outlive the global
    // registration (deregistered first in the destructor).
    [[nodiscard]] stability_invariants::Context buildStabilityInvariantContext()
    {
        stability_invariants::Context ctx;
        ctx.getSessionSnapshot = [this] { return session.loadSessionSnapshotForAudioThread(); };
        ctx.getActiveTrackId = [this] { return session.getActiveTrackId(); };
        ctx.getRoutingPlan = [this] { return playbackEngine_.loadRoutingPlanForDiagnostics(); };
        ctx.getPlaybackBridgeSnapshot = [this]
        { return playbackEngine_.loadExperimentalInstrumentPlaybackSnapshotForAudioThread(); };
        ctx.listInstrumentRuntimes = [this]() -> std::vector<stability_invariants::InstrumentRuntimeInfo>
        {
            std::vector<stability_invariants::InstrumentRuntimeInfo> out;
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                for (const auto& [tid, host, ctl] :
                     instrumentRuntimeCoordinator_->exportKeyedRuntimePointersForDiagnostics())
                {
                    out.push_back({ tid, host, ctl });
                }
            }
            return out;
        };
        ctx.listInsertChains = [this]() -> std::vector<stability_invariants::InsertEntryInfo>
        {
            std::vector<stability_invariants::InsertEntryInfo> out;
            for (auto& [tid, procs] : pluginHost_.exportChainInstancePointersForDiagnostics())
            {
                out.push_back({ tid, std::move(procs) });
            }
            return out;
        };
        ctx.listPublishedInsertMap = [this]() -> std::vector<stability_invariants::InsertEntryInfo>
        {
            std::vector<stability_invariants::InsertEntryInfo> out;
            for (auto& [tid, procs] : pluginHost_.exportPublishedMapPointersForDiagnostics())
            {
                out.push_back({ tid, std::move(procs) });
            }
            return out;
        };
        ctx.listInstrumentTimelineAttachmentTrackIds = [this]
        { return trackLanesView.exportInstrumentTimelineAttachmentTrackIdsForDiagnostics(); };
        ctx.hasStaleHeaderDragSource = [this]
        { return trackLanesView.hasStaleHeaderDragSourceForDiagnostics(); };
        ctx.getOpenMidiEditorTrackId = [this]() -> TrackId
        {
            if (midiEditorPresenter_ == nullptr)
            {
                return kInvalidTrackId;
            }
            const std::optional<TrackId> tid = midiEditorPresenter_->openedTrackId();
            return tid.has_value() ? *tid : kInvalidTrackId;
        };
        ctx.describeAutosavePointerIssue = []() -> juce::String
        {
            const juce::File pointer
                = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                      .getChildFile("MiniDAWLab")
                      .getChildFile("autosave-location.txt");
            if (!pointer.existsAsFile())
            {
                return {};
            }
            // C5 pointer format: line 1 = autosave path, line 2 (optional) = original project.
            juce::StringArray lines;
            pointer.readLines(lines);
            const juce::String recorded = lines.size() > 0 ? lines[0].trim() : juce::String{};
            if (recorded.isEmpty() || !juce::File::isAbsolutePath(recorded))
            {
                return "autosave pointer file has no absolute path (recovery falls back cleanly)";
            }
            if (!juce::File(recorded).existsAsFile())
            {
                return "autosave pointer references missing file " + recorded
                       + " (recovery falls back cleanly)";
            }
            // Autosave polish: line 2 (owner project) must be an absolute path when present, so
            // cleanup/recovery can attribute the autosave to the right project.
            const juce::String owner = lines.size() > 1 ? lines[1].trim() : juce::String{};
            if (owner.isNotEmpty() && owner != "(never saved)"
                && !juce::File::isAbsolutePath(owner))
            {
                return "autosave pointer line 2 (owner) is not an absolute path: " + owner;
            }
            return {};
        };
        // Stability C5 (invariant 8): a recovered autosave must never own the save path.
        ctx.getCurrentProjectFilePath = [this]() -> juce::String
        {
            return session.hasKnownProjectFile()
                       ? session.getCurrentProjectFile().getFullPathName()
                       : juce::String{};
        };
        return ctx;
    }

    void refreshInstrumentUi()
    {
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
        instrumentRuntimeCoordinator_->updateExperimentalPlaybackBridgeAfterRegistryChange();
        // P1G: Primary availability may have changed (load/unload/replace/project load) —
        // re-evaluate the authoritative playback source for every destination.
        if (proxyPlaybackCoordinator_ != nullptr)
        {
            proxyPlaybackCoordinator_->refreshAllInstrumentDestinations();
        }
        instrumentRuntimeCoordinator_->syncAllKeyedAndStagingShellWithHostState();
        trackLanesView.refreshInstrumentHeaderReorderAttachments();
        trackLanesView.rebuildVisibleTrackEntries();
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
        resized();

        if (midiEditorPresenter_ != nullptr)
        {
            midiEditorPresenter_->refreshInstrumentUiIfOpen();
        }
    }

    /// [Message thread] P1H: remember each destination's referenced asset's ABSOLUTE location
    /// while the project folder is authoritatively known (after load / after publication). The
    /// hint is runtime-only; Save As uses it as the copy source (§16.6) because by then the
    /// coordinator has already switched the save path to the NEW folder.
    void captureProxyAssetSourceHints(const juce::File& projectFolder)
    {
        if (instrumentRuntimeCoordinator_ == nullptr || projectFolder == juce::File())
        {
            return;
        }
        const auto snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return;
        }
        for (int i = 0; i < snap->getNumTracks(); ++i)
        {
            const Track& t = snap->getTrack(i);
            if (t.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(t.getId());
            if (c == nullptr)
            {
                continue;
            }
            const ProjectFileProxyMetadataV20* const meta = c->getProxyMetadata();
            if (meta == nullptr || meta->silentGeneration || meta->relativePath.isEmpty())
            {
                continue;
            }
            const juce::File f
                = proxy_store::resolveProxyRelativePath(projectFolder, meta->relativePath);
            if (f != juce::File() && f.existsAsFile())
            {
                c->setProxyAssetSourceHint(f);
            }
        }
    }

    /// [Message thread] P1H §16.6 Save As rehoming: copy every referenced generation asset into
    /// the NEW project's `InstrumentProxies/` layout (copy + validate + immutable rename; the
    /// original project and its assets are never touched). Failures are honest nonfatal states
    /// (the destination shows ProxyMissing in the new project) — no modal, diagnostics only.
    void rehomeProxyAssetsIntoFolder(const juce::File& newProjectFolder)
    {
        if (instrumentRuntimeCoordinator_ == nullptr)
        {
            return;
        }
        const auto snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return;
        }
        std::vector<proxy_store::ProxyRehomeItem> items;
        for (int i = 0; i < snap->getNumTracks(); ++i)
        {
            const Track& t = snap->getTrack(i);
            if (t.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(t.getId());
            const ProjectFileProxyMetadataV20* const meta
                = c != nullptr ? c->getProxyMetadata() : nullptr;
            if (meta == nullptr)
            {
                continue;
            }
            proxy_store::ProxyRehomeItem item;
            item.trackId = t.getId();
            item.metadata = *meta;
            item.sourceFile = c->getProxyAssetSourceHint();
            items.push_back(std::move(item));
        }
        if (items.empty())
        {
            return;
        }
        const auto outcome = proxy_store::rehomeProxyAssets(newProjectFolder, items);
        juce::Logger::writeToLog("[ProxyRehome] copied=" + juce::String(outcome.copied)
                                 + " alreadyPresent=" + juce::String(outcome.alreadyPresent)
                                 + " silent=" + juce::String(outcome.silent)
                                 + " errors=" + juce::String(outcome.errors.size()));
        for (const auto& e : outcome.errors)
        {
            juce::Logger::writeToLog("[ProxyRehome] " + e);
        }
        // Successfully rehomed assets are the new authoritative source location.
        captureProxyAssetSourceHints(newProjectFolder);
        // Re-derive playback views/status against the new project folder.
        if (proxyPlaybackCoordinator_ != nullptr)
        {
            proxyPlaybackCoordinator_->refreshAllInstrumentDestinations();
        }
    }

    Transport& transport;
    Session& session;
    PluginInsertHost& pluginHost_;
    juce::AudioDeviceManager& deviceManager;
    RecorderService& recorder_;
    CountInClickOutput& countInClicks_;
    LatencySettingsStore& latencyStore_;
    PlaybackEngine& playbackEngine_;
    /// P1E: application-owned scheduler (narrow API only — this view never owns jobs). The
    /// production engine below reaches into the runtime coordinator, so the destructor MUST
    /// call detachEngineAndShutdownJobs() before the coordinators are destroyed.
    proxy_render::ProxyRenderScheduler& proxyRenderScheduler_;
    std::unique_ptr<proxy_render::AppProxyRenderEngine> proxyRenderEngine_;
    /// P1G: playback-source coordination (proxy substitution views + reader lifecycle).
    /// Shut down in the destructor BEFORE the instrument runtime is torn down.
    std::unique_ptr<proxy_playback::ProxyPlaybackCoordinator> proxyPlaybackCoordinator_;
    /// P1H: per-destination update-policy engine (§18.1) — owns the fixed five-minute Auto
    /// idle timers on a dedicated 1 Hz timer. Destroyed FIRST in the destructor (before the
    /// scheduler detach) so no tick runs into torn-down runtimes. All state runtime-only.
    std::unique_ptr<proxy_policy::ProxyUpdatePolicyService> proxyUpdatePolicyService_;
    /// Test/integration clock skew added to the policy clock (0 in production; the automated
    /// P1H integration advances it to cross the five-minute boundary without waiting).
    double proxyPolicyTestClockOffsetMs_ = 0.0;
    /// P1J: "Prepare Portable Project" operation owner (§16.6). App-runtime service — never
    /// owned by the transient progress window below. Shut down (bounded cancel + worker join
    /// + staging cleanup) on project replacement and in the destructor.
    std::unique_ptr<portable_project::PortablePreparationService> portablePreparationService_;
    /// P1J progress surface (view only; closing it never stops the operation).
    std::unique_ptr<PortablePreparationWindow> portablePreparationWindow_;

    std::unique_ptr<InstrumentRuntimeCoordinator> instrumentRuntimeCoordinator_;
    /// Listed after IRC: reverse member destruction runs this dtor first while `instrumentRuntimeCoordinator_` still exists.
    std::unique_ptr<AddInstrumentTrackCoordinator> addInstrumentTrackCoordinator_;

    /// When Audio Settings is open; auto-clears when the dialog-owned view is destroyed.
    juce::Component::SafePointer<LatencySettingsView> audioLatencySettingsWeak_;
    /// Count-in / recording line (no always-visible audio device debug; use Audio menu).
    juce::Label countInStatusLabel_;
    /// Conny follow-playhead: main-arrangement Follow toggle (far right of the toolbar row).
    /// Independent of the MIDI editor's per-clip Follow. Persisted in the project `mainWindow`
    /// object; default ON for new projects and old projects without the field.
    juce::TextButton mainFollowPlayheadToggle_;
    bool mainFollowPlayhead_ = true;
    /// Freeze hardening: reentrancy guard + page/event-driven follow governor (see
    /// `maybeFollowMainArrangementPlayhead`). Counters feed `playback-ui-load.log`.
    bool followPanInProgress_ = false;
    FollowAutoscrollGovernor mainFollowGovernor_;
    // Unsigned: only reset when the ui-load diag flag is on; wraparound is harmless when it is off.
    unsigned int statsFollowPans_ = 0;
    unsigned int statsFollowSkipsGesture_ = 0;
    unsigned int statsFollowSkipsLateFrame_ = 0;
    unsigned int statsFollowSkipsAwaitClean_ = 0;
    unsigned int statsFollowSkipsPacing_ = 0;
    unsigned int statsFollowSkipsBoundary_ = 0;
    unsigned int statsFollowSkipsCrossWindow_ = 0;
    unsigned int statsFollowSkipsGlobalBudget_ = 0;
    unsigned int statsViewportChanges_ = 0;
    unsigned int statsViewportRepaintFlushes_ = 0;
    unsigned int lastGlobalFollowPagesSnapshot_ = 0;
    /// Transient "Saving project" indicator (see `showSavingProjectToast`).
    juce::Label savingProjectToastLabel_;
    std::unique_ptr<RecordingCoordinator> recordingCoordinator_;
    std::unique_ptr<TransportPlayPauseStopController> transportPlayPauseStopController_;
    std::unique_ptr<UndoRedoCoordinator> undoRedoCoordinator_;
    std::unique_ptr<ClipPasteboardController> clipPasteboardController_;
    std::unique_ptr<AudioClipImportCoordinator> audioClipImportCoordinator_;
    std::unique_ptr<InstrumentMidiImportCoordinator> instrumentMidiImportCoordinator_;
    std::unique_ptr<Vst3PluginPickerCoordinator> vst3PluginPickerCoordinator_;
    std::unique_ptr<ExperimentalMidiEditorWindow> midiEditorWindow_;
    std::unique_ptr<MidiEditorPresenter> midiEditorPresenter_;
    /// NOTE: must stay declared *before* `trackLanesView` — the lanes view's destructor detaches
    /// instrument row components owned by this coordinator, so the coordinator (and those
    /// components) must still be alive when `trackLanesView` is destroyed (reverse declaration
    /// order destruction). See `~TrackLanesView`.
    std::unique_ptr<InstrumentTimelineRowCoordinator> instrumentTimelineRowCoordinator_;
    std::unique_ptr<ProjectIoCoordinator> projectIoCoordinator_;
    /// SPIKE-01 (P0/P1A validation spike; removable): hidden `--spike01-state-capture` panel.
    /// Declared after `instrumentRuntimeCoordinator_` so it is destroyed first (its detach
    /// logic resolves hosts through the coordinator).
    std::unique_ptr<Spike01StateCapturePanel> spike01StateCapturePanel_;

    EditTool currentEditTool_ = EditTool::Pointer;

    std::unique_ptr<mini_daw_app_menu::MainMenuModel> mainMenuModel_;
    std::unique_ptr<juce::MenuBarComponent> menuBar_;

    AddTrackCornerGlyphButton addTrackCornerPlusButton_;
    EditToolIconStrip editToolIconStrip_;
    juce::Label arrangementBpmLabel_;
    juce::TextEditor arrangementBpmEditor_;
    juce::ComboBox arrangementTimeSignatureCombo_;
    bool arrangementMusicalUiApplyingFromSession_{false};
    juce::ToggleButton arrangementSnapToggle_;
    juce::ComboBox arrangementSnapResolutionCombo_;
    juce::ComboBox arrangementTimelineFormatCombo_;
    SnapSettings arrangementSnapSettings_;
    bool arrangementSnapUiApplyingFromProject_{false};

    juce::Label keyDiagLabel_;
    std::unique_ptr<juce::Label> shortcutDiagLabel_;

    /// UI-only: shared x–span for ruler and lanes; never stored in `Session` (see `PHASE_PLAN`).
    TimelineViewportModel timelineViewport_;
    AudioWaveformCache audioWaveformCache_;
#if MINIDAW_DIAG_PLAYBACK_UI_LOAD
    double lastPlaybackUiLoadLogMs_ = 0.0;
#endif
    /// One shared current-time source for this window's ruler stroke and lane playhead line.
    /// Declared before `rulerView` so it is alive when the ruler is constructed.
    UiPlayheadClock uiPlayheadClock_;
    TimelineRulerView rulerView;
    TrackLanesView trackLanesView;
    /// Repaint-storm fix: viewport-change repaints (ruler + lanes + instrument row) are marked once
    /// per message batch instead of once per wheel/drag event. Declared after the views it flushes
    /// so it is destroyed (and its pending update cancelled) before they are.
    CoalescedRepaintFlusher coalescedViewportRepaint_ { [this] {
        ++statsViewportRepaintFlushes_;
        rulerView.repaint();
        trackLanesView.repaint();
        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
        }
    } };
    std::unique_ptr<PlayheadOverlay> lanePlayheadOverlay_;
    InspectorView inspectorView_;
    collapsible_side_strip::ResizeSplitter inspectorResizeSplitter_;
    collapsible_side_strip::CollapsedKnob inspectorCollapsedKnob_;
    int inspectorCurrentWidth_ = kInspectorDefaultW;

    /// Destroyed before `trackLanesView` / `instrumentRuntimeCoordinator_` reverse dtors run (non-owning refs).
    std::unique_ptr<ArrangementEventSelectionCoordinator> arrangementEventSelectionCoordinator_;

    /// Declared LAST among data members (after `trackLanesView`, `rulerView`, `inspectorView_`)
    /// so it is destroyed FIRST in reverse-declaration order, while every UI object it borrows
    /// is still alive. See `TrackLanesEditCoordinator` ctor — it stores `&` to those views.
    std::unique_ptr<TrackLanesEditCoordinator> trackLanesEditCoordinator_;

    /// Shared Phase B.1 many-to-one capture assertions, run after the realtime playback pass and
    /// again after the offline mixdown pass: all three source streams reach the ONE destination,
    /// channels 1/2/3 stay distinct with exact per-channel counts (a per-source double-processed
    /// boundary would double them), the channel mask is 0x07, and stop/end-of-render leaves no
    /// dangling note-ons.
    [[nodiscard]] bool stabilityVerifyMidiRoutingCapture(const char* passName,
                                                         juce::String& failReason,
                                                         const bool requireStopFlushOffs)
    {
        const int ons = stabilityMidiRoutingCaptureSink_.noteOns.load(std::memory_order_relaxed);
        const int offs = stabilityMidiRoutingCaptureSink_.noteOffs.load(std::memory_order_relaxed);
        const std::uint32_t chMask
            = stabilityMidiRoutingCaptureSink_.noteOnChannelMask.load(std::memory_order_relaxed);
        const int ch1 = stabilityMidiRoutingCaptureSink_.noteOnsPerChannel[0].load(std::memory_order_relaxed);
        const int ch2 = stabilityMidiRoutingCaptureSink_.noteOnsPerChannel[1].load(std::memory_order_relaxed);
        const int ch3 = stabilityMidiRoutingCaptureSink_.noteOnsPerChannel[2].load(std::memory_order_relaxed);
        const std::uint64_t blocks
            = stabilityMidiRoutingCaptureSink_.blocksDelivered.load(std::memory_order_relaxed);
        const std::uint64_t boundaryTotal
            = stabilityMidiRoutingDestHost_ != nullptr
                  ? stabilityMidiRoutingDestHost_->getMidiDeliveryBoundaryBlockCountRelaxed()
                  : 0;
        appendStabilityRunLine("  capture(" + juce::String(passName) + "): noteOns=" + juce::String(ons)
                               + " noteOffs=" + juce::String(offs) + " ch1/ch2/ch3="
                               + juce::String(ch1) + "/" + juce::String(ch2) + "/" + juce::String(ch3)
                               + " channelMask=0x" + juce::String::toHexString((int)chMask)
                               + " blocksDelivered=" + juce::String((juce::int64)blocks)
                               + " hostBoundaryTotal=" + juce::String((juce::int64)boundaryTotal));
        if (ch1 != stabilityMidiRoutingExpectedOwnNotes_
            || ch2 != stabilityMidiRoutingExpectedLowerNotes_
            || ch3 != stabilityMidiRoutingExpectedPedalNotes_)
        {
            failReason = juce::String(passName) + ": per-channel note-ons "
                         + juce::String(ch1) + "/" + juce::String(ch2) + "/" + juce::String(ch3)
                         + " != expected " + juce::String(stabilityMidiRoutingExpectedOwnNotes_) + "/"
                         + juce::String(stabilityMidiRoutingExpectedLowerNotes_) + "/"
                         + juce::String(stabilityMidiRoutingExpectedPedalNotes_)
                         + " (channels 1/2/3 must stay distinct; boundary once per block)";
            return false;
        }
        const int expectedTotal = stabilityMidiRoutingExpectedOwnNotes_
                                  + stabilityMidiRoutingExpectedLowerNotes_
                                  + stabilityMidiRoutingExpectedPedalNotes_;
        if (ons != expectedTotal)
        {
            failReason = juce::String(passName) + ": expected " + juce::String(expectedTotal)
                         + " note-ons at the destination, captured " + juce::String(ons);
            return false;
        }
        constexpr std::uint32_t kExpectedMask = 0x07; // channels 1, 2, 3 (bit = channel - 1)
        if (chMask != kExpectedMask)
        {
            failReason = juce::String(passName) + ": note-on channel mask 0x"
                         + juce::String::toHexString((int)chMask) + " != expected 0x07";
            return false;
        }
        if (requireStopFlushOffs && offs < ons)
        {
            failReason = juce::String(passName) + ": captured fewer note-offs (" + juce::String(offs)
                         + ") than note-ons (" + juce::String(ons) + ") after stop flush";
            return false;
        }
        if (blocks == 0)
        {
            failReason = juce::String(passName) + ": capture sink saw no delivered blocks";
            return false;
        }
        if (blocks > boundaryTotal)
        {
            failReason = juce::String(passName) + ": sink deliveries ("
                         + juce::String((juce::int64)blocks) + ") exceed the host boundary count ("
                         + juce::String((juce::int64)boundaryTotal)
                         + ") — boundary must run once per block";
            return false;
        }

        // Stage D: CC automation must reach the destination on the EFFECTIVE channels, with no
        // repeats-flood, no leakage onto the CC-free source's channel, and never after a Note On
        // at the same sample offset.
        {
            const int cc = stabilityMidiRoutingCaptureSink_.ccEvents.load(std::memory_order_relaxed);
            const int cc1 = stabilityMidiRoutingCaptureSink_.ccEventsPerChannel[0].load(std::memory_order_relaxed);
            const int cc2 = stabilityMidiRoutingCaptureSink_.ccEventsPerChannel[1].load(std::memory_order_relaxed);
            const int cc3 = stabilityMidiRoutingCaptureSink_.ccEventsPerChannel[2].load(std::memory_order_relaxed);
            const int ccOrderViolations = stabilityMidiRoutingCaptureSink_.ccAfterNoteOnAtSameOffset
                                              .load(std::memory_order_relaxed);
            appendStabilityRunLine("  capture(" + juce::String(passName) + "): ccEvents="
                                   + juce::String(cc) + " cc1/cc2/cc3=" + juce::String(cc1) + "/"
                                   + juce::String(cc2) + "/" + juce::String(cc3)
                                   + " ccAfterNoteOnAtSameOffset=" + juce::String(ccOrderViolations));
            if (cc1 != stabilityMidiRoutingExpectedOwnCc_
                || cc2 != stabilityMidiRoutingExpectedLowerCc_ || cc3 != 0)
            {
                failReason = juce::String(passName) + ": per-channel CC events " + juce::String(cc1)
                             + "/" + juce::String(cc2) + "/" + juce::String(cc3) + " != expected "
                             + juce::String(stabilityMidiRoutingExpectedOwnCc_) + "/"
                             + juce::String(stabilityMidiRoutingExpectedLowerCc_)
                             + "/0 (effective channels; no flood; no leak onto Pedal's channel)";
                return false;
            }
            if (cc != cc1 + cc2 + cc3)
            {
                failReason = juce::String(passName) + ": CC events on unexpected channels (total "
                             + juce::String(cc) + " != ch1+ch2+ch3 " + juce::String(cc1 + cc2 + cc3)
                             + ")";
                return false;
            }
            if (ccOrderViolations != 0)
            {
                failReason = juce::String(passName) + ": " + juce::String(ccOrderViolations)
                             + " CC event(s) arrived AFTER a Note On at the same sample offset";
                return false;
            }
        }
        return true;
    }

    // --- Phase B/B.1 stability scenario (`--stability-midi-routing`) fixture state ---
    /// RT-safe MIDI delivery observer for the destination host (counters only; no locks/alloc).
    /// Phase B.1 adds per-channel note-on counts (many-to-one: channels 1/2/3 must stay distinct)
    /// and a delivered-block counter (destination boundary invoked once per block, not per source).
    struct StabilityMidiRoutingCaptureSink final : ExperimentalInstrumentHost::MidiDeliveryCaptureSink
    {
        std::atomic<int> noteOns{ 0 };
        std::atomic<int> noteOffs{ 0 };
        /// Bit (channel - 1) set for every captured note-on channel.
        std::atomic<std::uint32_t> noteOnChannelMask{ 0 };
        /// Note-ons per MIDI channel (index = channel - 1).
        std::atomic<int> noteOnsPerChannel[16]{};
        /// Stage D: Control Change events per MIDI channel (index = channel - 1) + total.
        std::atomic<int> ccEvents{ 0 };
        std::atomic<int> ccEventsPerChannel[16]{};
        /// Stage D ordering invariant: a CC found at the SAME sample offset AFTER a Note On in the
        /// merged buffer would reach the plugin after the attack — must stay 0.
        std::atomic<int> ccAfterNoteOnAtSameOffset{ 0 };
        /// `onMidiBlockDelivered` invocations (== destination processing-boundary deliveries).
        std::atomic<std::uint64_t> blocksDelivered{ 0 };

        void reset() noexcept
        {
            noteOns.store(0, std::memory_order_relaxed);
            noteOffs.store(0, std::memory_order_relaxed);
            noteOnChannelMask.store(0, std::memory_order_relaxed);
            for (auto& c : noteOnsPerChannel)
            {
                c.store(0, std::memory_order_relaxed);
            }
            ccEvents.store(0, std::memory_order_relaxed);
            for (auto& c : ccEventsPerChannel)
            {
                c.store(0, std::memory_order_relaxed);
            }
            ccAfterNoteOnAtSameOffset.store(0, std::memory_order_relaxed);
            blocksDelivered.store(0, std::memory_order_relaxed);
        }
        void onMidiBlockDelivered(const juce::MidiBuffer& merged, int) override
        {
            blocksDelivered.fetch_add(1, std::memory_order_relaxed);
            int lastPos = -1;
            std::uint32_t noteOnChannelsAtPos = 0;
            for (const auto meta : merged)
            {
                const juce::MidiMessage m = meta.getMessage();
                if (meta.samplePosition != lastPos)
                {
                    lastPos = meta.samplePosition;
                    noteOnChannelsAtPos = 0;
                }
                const int ch = m.getChannel();
                if (m.isNoteOn())
                {
                    noteOns.fetch_add(1, std::memory_order_relaxed);
                    if (ch >= 1 && ch <= 16)
                    {
                        noteOnChannelsAtPos |= 1u << (ch - 1);
                        noteOnChannelMask.fetch_or(1u << (ch - 1), std::memory_order_relaxed);
                        noteOnsPerChannel[ch - 1].fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else if (m.isNoteOff())
                {
                    noteOffs.fetch_add(1, std::memory_order_relaxed);
                }
                else if (m.isController() && m.getControllerNumber() < 120)
                {
                    // Channel-voice controllers only: the stop flush legitimately sends channel
                    // MODE messages (All Notes Off = CC 123) on every channel — not automation.
                    ccEvents.fetch_add(1, std::memory_order_relaxed);
                    if (ch >= 1 && ch <= 16)
                    {
                        ccEventsPerChannel[ch - 1].fetch_add(1, std::memory_order_relaxed);
                        // Ordering is per channel: a controller value only "arrives late" for a
                        // note attack on ITS OWN channel; interleaving with other sources'
                        // channels at the same offset is fine.
                        if ((noteOnChannelsAtPos & (1u << (ch - 1))) != 0)
                        {
                            ccAfterNoteOnAtSameOffset.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        }
    };
    StabilityMidiRoutingCaptureSink stabilityMidiRoutingCaptureSink_;
    TrackId stabilityMidiRoutingInstTid_ = kInvalidTrackId;
    /// Phase B.1 many-to-one sources: "Lower" (fixed output channel 2), "Pedal" (fixed 3).
    TrackId stabilityMidiRoutingMidiLowerTid_ = kInvalidTrackId;
    TrackId stabilityMidiRoutingMidiPedalTid_ = kInvalidTrackId;
    /// Expected note-ons per stream: destination's own clip (ch 1), Lower (ch 2), Pedal (ch 3).
    int stabilityMidiRoutingExpectedOwnNotes_ = 0;
    int stabilityMidiRoutingExpectedLowerNotes_ = 0;
    int stabilityMidiRoutingExpectedPedalNotes_ = 0;
    /// Stage D: expected CC11 deliveries — destination's own stream (effective ch 1) and the
    /// Lower source (native 5 → effective ch 2). Pedal carries none (ch 3 must stay CC-free).
    int stabilityMidiRoutingExpectedOwnCc_ = 0;
    int stabilityMidiRoutingExpectedLowerCc_ = 0;
    /// Destination host (installed sink) for boundary-count comparison in the verify hooks.
    ExperimentalInstrumentHost* stabilityMidiRoutingDestHost_ = nullptr;

    /// Stability C2 only (`--stability-*` command line); null in normal use. Declared last:
    /// its hooks capture `this` and touch most members above, so it must be destroyed first.
    std::unique_ptr<StabilityScenarioRunner> stabilityScenarioRunner_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportControlsContent)
};

} // namespace mini_daw_app_transport

std::int64_t mini_daw_app_transport::TransportControlsContent::snapArrangementTimelineSample(
    std::int64_t sampleOnTimeline) const noexcept
{
    juce::AudioIODevice* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr)
    {
        return juce::jmax(std::int64_t{ 0 }, sampleOnTimeline);
    }
    const double sr = dev->getCurrentSampleRate();
    return snapSampleToGridIfEnabled(
        sampleOnTimeline, arrangementSnapSettings_, session.getProjectMusicalTime(), sr);
}

void mini_daw_app_transport::TransportControlsContent::configureArrangementMusicalControls()
{
    arrangementBpmLabel_.setText("BPM", juce::dontSendNotification);
    arrangementBpmLabel_.setJustificationType(juce::Justification::centredRight);
    arrangementBpmLabel_.setFont(juce::FontOptions(11.0f));
    arrangementBpmLabel_.setInterceptsMouseClicks(false, false);

    arrangementBpmEditor_.setMultiLine(false);
    arrangementBpmEditor_.setReturnKeyStartsNewLine(false);
    arrangementBpmEditor_.setReadOnly(false);
    arrangementBpmEditor_.setScrollbarsShown(false);
    arrangementBpmEditor_.setCaretVisible(true);
    arrangementBpmEditor_.setPopupMenuEnabled(false);
    arrangementBpmEditor_.setInputRestrictions(16, "0123456789.");
    arrangementBpmEditor_.setTooltip("Project tempo (arrangement ruler)");
    arrangementBpmEditor_.setFont(juce::FontOptions(11.0f));
    // Enter commits and *releases focus*: a TextEditor that keeps focus consumes every later
    // digit/space keypress as text input, silently killing transport shortcuts (numpad 1, Space)
    // until the user happens to click elsewhere.
    arrangementBpmEditor_.onReturnKey = [this] {
        commitArrangementBpmFromEditorIfNeeded();
        arrangementBpmEditor_.giveAwayKeyboardFocus();
    };
    arrangementBpmEditor_.onFocusLost = [this] { commitArrangementBpmFromEditorIfNeeded(); };

    arrangementTimeSignatureCombo_.setTooltip("Project time signature");
    arrangementTimeSignatureCombo_.onChange = [this] { handleArrangementTimeSignatureComboChangedByUser(); };
}

void mini_daw_app_transport::TransportControlsContent::applyArrangementMusicalUiFromSession(
    const ProjectMusicalTime mt,
    const bool repaintTimeline)
{
    arrangementMusicalUiApplyingFromSession_ = true;
    arrangementBpmEditor_.setText(formatProjectBpmForToolbar(mt.bpm), juce::dontSendNotification);
    rebuildArrangementTimeSignatureComboItems(mt);
    arrangementMusicalUiApplyingFromSession_ = false;
    if (repaintTimeline)
    {
        rulerView.repaint();
        trackLanesView.repaint();
    }
}

void mini_daw_app_transport::TransportControlsContent::rebuildArrangementTimeSignatureComboItems(
    const ProjectMusicalTime& mt)
{
    arrangementTimeSignatureCombo_.clear(juce::dontSendNotification);
    for (const auto& p : kArrangementTimeSigPresets)
    {
        arrangementTimeSignatureCombo_.addItem(p.label, p.id);
    }

    bool matched = false;
    for (const auto& p : kArrangementTimeSigPresets)
    {
        if (p.num == mt.numerator && p.den == mt.denominator)
        {
            arrangementTimeSignatureCombo_.setSelectedId(p.id, juce::dontSendNotification);
            matched = true;
            break;
        }
    }

    if (!matched)
    {
        arrangementTimeSignatureCombo_.addItem(
            juce::String(mt.numerator) + "/" + juce::String(mt.denominator), kArrangementTimeSigCustomComboId);
        arrangementTimeSignatureCombo_.setSelectedId(kArrangementTimeSigCustomComboId, juce::dontSendNotification);
    }
}

void mini_daw_app_transport::TransportControlsContent::commitArrangementBpmFromEditorIfNeeded()
{
    if (arrangementMusicalUiApplyingFromSession_)
    {
        return;
    }

    const double parsed = arrangementBpmEditor_.getText().getDoubleValue();
    const ProjectMusicalTime cur = session.getProjectMusicalTime();

    if (!std::isfinite(parsed) || parsed <= 0.0)
    {
        applyArrangementMusicalUiFromSession(cur, false);
        return;
    }

    ProjectMusicalTime probe = cur;
    probe.bpm = parsed;
    probe = sanitizeProjectMusicalTime(probe);

    if (std::abs(probe.bpm - cur.bpm) < 1e-9)
    {
        return;
    }

    if (undoRedoCoordinator_ != nullptr)
    {
        undoRedoCoordinator_->executeUndoableSessionEdit(
            "Project BPM",
            [this, parsed]() -> bool {
                ProjectMusicalTime live = session.getProjectMusicalTime();
                ProjectMusicalTime next = live;
                next.bpm = parsed;
                next = sanitizeProjectMusicalTime(next);
                if (std::abs(next.bpm - live.bpm) < 1e-9)
                {
                    return false;
                }
                session.setProjectBpm(next.bpm);
                // Clips always play at the project tempo (undo/redo re-aligns via
                // refreshAfterSessionSnapshotRestore).
                if (instrumentRuntimeCoordinator_ != nullptr)
                {
                    instrumentRuntimeCoordinator_->alignAllInstrumentClipTemposToProjectTempo();
                }
                return true;
            });
    }

    applyArrangementMusicalUiFromSession(session.getProjectMusicalTime(), true);
}

void mini_daw_app_transport::TransportControlsContent::handleArrangementTimeSignatureComboChangedByUser()
{
    if (arrangementMusicalUiApplyingFromSession_)
    {
        return;
    }

    const int id = arrangementTimeSignatureCombo_.getSelectedId();
    if (id == kArrangementTimeSigCustomComboId)
    {
        return;
    }

    int num = 4;
    int den = 4;
    if (!arrangementTimeSigPresetForComboId(id, num, den))
    {
        return;
    }

    const ProjectMusicalTime cur = session.getProjectMusicalTime();
    if (cur.numerator == num && cur.denominator == den)
    {
        return;
    }

    if (undoRedoCoordinator_ != nullptr)
    {
        undoRedoCoordinator_->executeUndoableSessionEdit(
            "Project time signature",
            [this, num, den]() -> bool {
                ProjectMusicalTime live = session.getProjectMusicalTime();
                if (live.numerator == num && live.denominator == den)
                {
                    return false;
                }
                ProjectMusicalTime next = live;
                next.numerator = num;
                next.denominator = den;
                session.setProjectMusicalTime(sanitizeProjectMusicalTime(next));
                return true;
            });
    }

    applyArrangementMusicalUiFromSession(session.getProjectMusicalTime(), true);
}

void mini_daw_app_transport::TransportControlsContent::configureArrangementSnapControls()
{
    arrangementSnapToggle_.setClickingTogglesState(true);
    arrangementSnapToggle_.setTooltip("Snap");
    arrangementSnapToggle_.setButtonText("Snap");
    arrangementSnapToggle_.onClick = [this] { handleArrangementSnapUiChangedByUser(); };

    clearAndPopulateSnapResolutionComboBox(arrangementSnapResolutionCombo_);
    arrangementSnapResolutionCombo_.setTooltip("Snap resolution");
    arrangementSnapResolutionCombo_.onChange = [this] { handleArrangementSnapUiChangedByUser(); };
}

void mini_daw_app_transport::TransportControlsContent::applyArrangementSnapUiFromSettings(
    const SnapSettings& s,
    const bool repaintTimeline)
{
    arrangementSnapUiApplyingFromProject_ = true;
    arrangementSnapToggle_.setToggleState(s.enabled, juce::dontSendNotification);
    arrangementSnapResolutionCombo_.setSelectedId(snapResolutionToComboItemId(s.resolution),
                                                 juce::dontSendNotification);
    arrangementSnapUiApplyingFromProject_ = false;
    arrangementSnapSettings_ = s;
    session.setArrangementSnapSettings(s);
    if (midiEditorPresenter_ != nullptr)
    {
        midiEditorPresenter_->refreshArrangementSnapMirrorFromSession();
    }
    if (repaintTimeline)
    {
        rulerView.repaint();
        trackLanesView.repaint();
    }
}

void mini_daw_app_transport::TransportControlsContent::handleArrangementSnapUiChangedByUser()
{
    if (arrangementSnapUiApplyingFromProject_)
    {
        return;
    }
    arrangementSnapSettings_.enabled = arrangementSnapToggle_.getToggleState();
    arrangementSnapSettings_.resolution
        = snapResolutionFromComboItemId(arrangementSnapResolutionCombo_.getSelectedId());
    session.setArrangementSnapSettings(arrangementSnapSettings_);
    if (midiEditorPresenter_ != nullptr)
    {
        midiEditorPresenter_->refreshArrangementSnapMirrorFromSession();
    }
    rulerView.repaint();
    trackLanesView.repaint();
}

SnapProjectRootFields mini_daw_app_transport::TransportControlsContent::arrangementSnapPersistenceSnapshotForSave()
    const
{
    return {
        arrangementSnapToggle_.getToggleState(),
        snapResolutionToProjectString(
            snapResolutionFromComboItemId(arrangementSnapResolutionCombo_.getSelectedId())),
    };
}

void mini_daw_app_transport::TransportControlsContent::restoreArrangementSnapFromProjectRootFields(
    const SnapProjectRootFields& fields)
{
    SnapSettings s;
    s.enabled = fields.enabled;
    s.resolution = snapResolutionFromProjectString(fields.resolutionKey);
    applyArrangementSnapUiFromSettings(s, true);
}

void mini_daw_app_transport::TransportControlsContent::configureTimelineRulerFormatControls()
{
    arrangementTimelineFormatCombo_.clear(juce::dontSendNotification);
    arrangementTimelineFormatCombo_.addItem("Bars + Beats", Session::kTimelineRulerFormatComboIdBarsBeats);
    arrangementTimelineFormatCombo_.addItem("Seconds", Session::kTimelineRulerFormatComboIdSeconds);
    arrangementTimelineFormatCombo_.setTooltip("Timeline ruler time format (shared with the MIDI editor).");
    arrangementTimelineFormatCombo_.onChange = [this] {
        const int id = arrangementTimelineFormatCombo_.getSelectedId();
        if (id == Session::kTimelineRulerFormatComboIdBarsBeats)
        {
            session.setTimelineRulerTimeDisplay(Session::TimelineRulerTimeDisplay::MusicalBarsBeats);
        }
        else if (id == Session::kTimelineRulerFormatComboIdSeconds)
        {
            session.setTimelineRulerTimeDisplay(Session::TimelineRulerTimeDisplay::TimeSeconds);
        }
    };
}

void mini_daw_app_transport::TransportControlsContent::applyTimelineRulerFormatButtonFromSession()
{
    const int id = session.getTimelineRulerTimeDisplay() == Session::TimelineRulerTimeDisplay::MusicalBarsBeats
                       ? Session::kTimelineRulerFormatComboIdBarsBeats
                       : Session::kTimelineRulerFormatComboIdSeconds;
    arrangementTimelineFormatCombo_.setSelectedId(id, juce::dontSendNotification);
}

void mini_daw_app_transport::TransportControlsContent::invokeDeleteSelectedPlacedClipFromWindowShortcut()
{
    if (clipPasteboardController_ != nullptr)
    {
        clipPasteboardController_->invokeDeleteSelectedPlacedClipFromWindowShortcut();
    }
}

void mini_daw_app_transport::TransportControlsContent::invokeCopySelectedClipFromWindowShortcut()
{
    if (clipPasteboardController_ != nullptr)
    {
        clipPasteboardController_->invokeCopySelectedClipFromWindowShortcut();
    }
}

void mini_daw_app_transport::TransportControlsContent::invokePasteClipFromWindowShortcut()
{
    if (clipPasteboardController_ != nullptr)
    {
        clipPasteboardController_->invokePasteClipFromWindowShortcut();
    }
}

void mini_daw_app_transport::TransportControlsContent::clearExperimentalInstrumentRuntimesPreserveBridgeOnly() noexcept
{
    transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    if (transportPlayPauseStopController_ != nullptr)
    {
        transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
    }
    if (midiEditorPresenter_ != nullptr)
    {
        midiEditorPresenter_->resetWindowAndBooking();
    }
    trackLanesView.syncInstrumentTimelineAttachments({});
    instrumentTimelineRowCoordinator_->clearInstrumentTimelineLanesAndHeaders();
    if (instrumentRuntimeCoordinator_ != nullptr)
    {
        instrumentRuntimeCoordinator_->releaseExperimentalInstrumentHostsDeviceResources();
        juce::Thread::sleep(120);
        instrumentRuntimeCoordinator_->clearRuntimesPreserveBridgeOnly();
    }
}

CreatedTransportUiForMainWindow createTransportUiForMainWindow(
    Transport& transport,
    Session& session,
    PluginInsertHost& pluginInsertHost,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorderService,
    CountInClickOutput& countInClicks,
    LatencySettingsStore& latencyStore,
    PlaybackEngine& playbackEngine,
    proxy_render::ProxyRenderScheduler& proxyRenderScheduler)
{
    auto component = std::make_unique<mini_daw_app_transport::TransportControlsContent>(
        transport,
        session,
        pluginInsertHost,
        deviceManager,
        recorderService,
        countInClicks,
        latencyStore,
        playbackEngine,
        proxyRenderScheduler);

    TransportControlsShortcutTarget* const shortcutTarget =
        static_cast<TransportControlsShortcutTarget*>(component.get());

    return {std::move(component), shortcutTarget};
}
