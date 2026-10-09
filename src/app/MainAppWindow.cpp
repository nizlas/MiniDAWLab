#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "app/AddInstrumentTrackCoordinator.h"
#include "app/AudioClipImportCoordinator.h"
#include "app/InstrumentMidiImportCoordinator.h"
#include "app/LiveMidiInputCoordinator.h"
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
#include "app/SoloCoordinator.h"
#include "app/UndoRedoCoordinator.h"
#include "app/ShortcutDiagnostics.h"
#include "app/TransportControlsFactory.h"
#include "app/TransportControlsShortcutTarget.h"
#include "app/Vst3PluginPickerCoordinator.h"
#include "plugins/InstrumentCatalog.h"
#include "plugins/Vst3ChildProcessScan.h"
#include "app/InstrumentMusicalUndoSnapshot.h"
#include "app/ArrangementEventSelectionCoordinator.h"
#include "app/InstrumentRuntimeCoordinator.h"
#include "app/InstrumentTimelineRowCoordinator.h"
#include "app/AudioMixdownExporter.h"
#include "diagnostics/AudioThreadProfiler.h"
#include "diagnostics/DiagnosticBuildFlags.h"
#include "diagnostics/PlaybackUiLoadLog.h"
#include "diagnostics/UiPaintLoadCounters.h"
#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/ProxyRenderProbeAnalysis.h"
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
#include "instruments/ProxyOfflineSequencer.h"
#include "instruments/ProxyRenderScheduler.h"
#include "instruments/ProxyRenderSnapshot.h"
#include "plugins/InsertSlotId.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/CoalescedRepaintFlusher.h"
#include "ui/FollowAutoscrollGovernor.h"
#include "ui/PlayheadOverlay.h"
#include "ui/UiPlayheadClock.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "ui/InspectorPanel.h"
#include "ui/LevelMeterHub.h"
#include "ui/TrackLanesView.h"
#include "ui/UiLayoutSettingsStore.h"
#include "ui/mixer/MixerWindow.h"
#include "ui/EditToolIconStrip.h"
#include "ui/SoloMemoryStrip.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/InspectorView.h"
#include "ui/InstrumentAlternativesPopup.h"
#include "ui/PortablePreparationWindow.h"
#include "ui/SnapResolutionComboBox.h"
#include "ui/SnapSettings.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/LatencySettingsStore.h"
#include "ui/LatencySettingsView.h"
#include "ui/experimental/ExperimentalMidiEditorWindow.h"

#include "io/AudioWaveformCache.h"
#include "io/InstrumentMidiClipExport.h"
#include "io/ProjectFile.h"
#include "ui/experimental/ExperimentalMidiImport.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

namespace
{
/// P2 Secondary selector: dedicated-kind instruments (HALion Sonic family) resolved through the
/// SAME cache path the Add Instrument Track menu uses (`tryLoadHalionSonicCacheCandidates`,
/// experimental VST3 descriptions cache). These instruments are deliberately NOT part of the
/// scanned GenericVst3 catalogue, so an empty scanned catalogue must not hide them here.
struct DedicatedSecondaryChoice
{
    juce::PluginDescription description;
    juce::File bundle;
};

[[nodiscard]] std::vector<DedicatedSecondaryChoice> listDedicatedSecondaryChoices()
{
    std::vector<DedicatedSecondaryChoice> out;
    mini_daw::Vst3GrooveCacheLoadCandidate v2Cand;
    mini_daw::Vst3GrooveCacheLoadCandidate v1Cand;
    juce::String infoIgnored;
    (void)mini_daw::tryLoadHalionSonicCacheCandidates({}, v2Cand, v1Cand, infoIgnored);
    const mini_daw::Vst3GrooveCacheLoadCandidate& cand = v2Cand.valid ? v2Cand : v1Cand;
    if (cand.valid && !cand.descriptions.empty() && cand.resolvedBundle.exists())
    {
        out.push_back({ cand.descriptions.front(), cand.resolvedBundle });
    }
    return out;
}

/// True when any scanned-catalogue name already covers the HALion Sonic family (no duplicate row).
[[nodiscard]] bool scannedCatalogHasHalionSonic(
    const std::vector<mini_daw::InstrumentCatalogEntry>& entries)
{
    for (const auto& e : entries)
    {
        if (mini_daw::instrumentDisplayNameLooksLikeHalionSonic(e.description.name))
        {
            return true;
        }
    }
    return false;
}

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
    static constexpr int kInspectorMaxW = 420;
    /// Wide enough for the channel panel's three columns (fader + track meter + Stereo Out meter)
    /// with their scales; the strip is still collapsible / resizable exactly as before.
    static constexpr int kInspectorDefaultW = 160;

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
                  return anyRecordingInProgress()
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
        , inspectorPanel_(sessionIn)
        , inspectorView_(inspectorPanel_.inspector())
        , inspectorResizeSplitter_(*this)
        , inspectorCollapsedKnob_(*this)
    {
        // Level meters: ONE hub drains the engine's concurrent per-row accumulators (and the
        // Stereo Out window) at 30 Hz and hands every window to each view that shows the row —
        // the Inspector channel panel and the mixer strips never drain the engine themselves.
        levelMeterHub_.setEngineHooks(LevelMeterHub::EngineHooks{
            [this](const std::vector<TrackId>& ids) { return playbackEngine_.setConcurrentlyMeteredTracks(ids); },
            [this](const TrackId tid) { return playbackEngine_.drainConcurrentTrackLevels(tid); },
            [this] { return playbackEngine_.drainMasterOutputLevels(); },
            [this]() -> TrackId {
                const auto snap = session.loadSessionSnapshotForAudioThread();
                return snap != nullptr ? snap->findCanonicalMasterTrackId() : kInvalidTrackId;
            },
        });
        inspectorPanel_.channelPanel().setMeterHub(&levelMeterHub_);
        // Shared track-header column width: app-wide preference (`%APPDATA%\MiniDAWLab\ui-layout.xml`),
        // restored before the first layout; absent/invalid ⇒ the view's default. Persisted once per
        // completed drag of the header/timeline boundary handle (no I/O per mouse move, no undo step).
        uiLayoutSettings_.loadFromFile();
        trackLanesView.setTrackHeaderColumnWidthPx(
            uiLayoutSettings_.getTrackHeaderColumnWidthPx().value_or(TrackLanesView::kTrackHeaderColumnDefaultWidthPx),
            /*notifyOwner*/ false);
        trackLanesView.setOnTrackHeaderColumnWidthChanged([this](const int widthPx, const bool dragEnded) {
            resized(); // ruler corner, add-track button and playhead overlay move to the same boundary
            if (dragEnded)
            {
                uiLayoutSettings_.setTrackHeaderColumnWidthPx(widthPx);
                uiLayoutSettings_.save();
            }
        });

        recordingCoordinator_ = std::make_unique<RecordingCoordinator>(
            transport,
            session,
            playbackEngine_,
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
                [this]() { return anyRecordingInProgress(); },
                [this]() {
                    return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive();
                },
                [this](const char* sourceContext) {
                    recordingCoordinator_->stopRecordingAndCommitFromUi(sourceContext);
                },
                [this]() { recordingCoordinator_->cancelCountIn(); },
                [this]() { return isProjectLoadInProgress(); },
            });

        undoRedoCoordinator_ = std::make_unique<UndoRedoCoordinator>(
            session,
            pluginHost_,
            UndoRedoCoordinator::Callbacks{
                [this] { return anyRecordingInProgress(); },
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
                [this] {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->detachOpenEditorIfBoundClipMissing();
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
                [this] { return isProjectLoadInProgress(); },
                [this] {
                    if (soloCoordinator_ != nullptr)
                    {
                        soloCoordinator_->republishDerivedView();
                    }
                },
                // Visual track groups: a narrow group-metadata undo step replaced the full group
                // list in `Session` — relayout the arrangement (markers, handles, collapsed rows).
                [this] { trackLanesView.refreshVisualTrackGroupsFromSession(); },
                // Track colours: a colour command / undo / redo changed one track's palette key —
                // repaint headers + lanes (cached wave rasters re-derive their fill).
                [this] { trackLanesView.refreshTrackColoursFromSession(); },
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
                    return transport.readPlaybackIntentForUi() == PlaybackIntent::Playing || anyRecordingInProgress()
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
                // P1 acceptance correction (§18.3/§18.4): after the WAV publication and the
                // immediate runtime activation above, checkpoint the new metadata reference
                // into the main `.dalproj` automatically WHEN SAFE (saved-generation guard:
                // real saved project file, no unsaved user edits since the last successful
                // Save/load, unchanged on-disk identity). The checkpoint re-reads the last
                // saved representation and replaces only this destination's proxy metadata —
                // it can never silently save unrelated user edits, creates no undo entry and
                // never fires onSuccessfulUserSave (no On Save render recursion). One user
                // Save in On Save mode therefore fully persists: Save → render → publish →
                // automatic metadata checkpoint, no second Save.
                //
                // When the checkpoint is refused (e.g. unsaved user edits) or fails, the P1H
                // §18.3 behavior remains: publication metadata DIRTIES the project (excluded
                // from musical undo — §12.3 blob-stripping precedent), so close prompts and
                // the next explicit Save persists the reference (the save DTO reads the
                // controller's in-memory metadata); autosave persists it to the recovery
                // artifact without ever triggering rendering.
                if (projectIoCoordinator_ != nullptr)
                {
                    bool persisted = false;
                    InstrumentTrackController* const c
                        = instrumentRuntimeCoordinator_ != nullptr
                              ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                              : nullptr;
                    ProjectFileProxyMetadataV20 published;
                    bool savedStatePairingProven = false;
                    if (c != nullptr
                        && c->getProxyMetadataForCheckpoint(published, savedStatePairingProven))
                    {
                        persisted = projectIoCoordinator_->persistPublishedProxyMetadataIfSafe(
                            tid, published, savedStatePairingProven);
                    }
                    if (!persisted)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
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
            // P2 (steering §17): the Secondary fallback. Consulted only when neither Primary nor
            // a usable Current proxy can play — the ensure call lazily instantiates the Secondary
            // (message thread; existing safe load path) and latches per-descriptor failures.
            pbDeps.secondaryUsable = [this](const TrackId tid) {
                return instrumentRuntimeCoordinator_ != nullptr
                       && instrumentRuntimeCoordinator_->ensureSecondaryInstrumentLoadedForTrack(tid);
            };
            pbDeps.setSecondaryTransportActive = [this](const TrackId tid, const bool active) {
                if (instrumentRuntimeCoordinator_ != nullptr)
                {
                    instrumentRuntimeCoordinator_->setSecondaryTransportActive(tid, active);
                }
            };
            // Live MIDI: a monitored row needs a live instrument — the coordinator decides
            // whether that means a temporary Secondary (see ProxyPlaybackCoordinator deps).
            pbDeps.liveMonitorRequested = [this](const TrackId tid) {
                return liveMidiInputCoordinator_ != nullptr
                       && liveMidiInputCoordinator_->liveMonitorRequestedForDestination(tid);
            };
            // A take on one of the destination's source rows needs a live source too (the row's
            // earlier clips must be silent) — evaluated separately from Monitor.
            pbDeps.liveRecordingRequested = [this](const TrackId tid) {
                return liveMidiInputCoordinator_ != nullptr
                       && liveMidiInputCoordinator_->liveRecordingRequestedForDestination(tid);
            };
            // Solo (spec §5): PARTIAL isolation inside a proxied destination (own clips gated, or
            // a gated subset of its routed sources) forces the safe Secondary live path — a proxy
            // mixes the full material and cannot isolate. Whole-destination solo never sets this,
            // so a valid proxy keeps playing for it. Stored proxy identity/currency is untouched.
            pbDeps.soloIsolationRequested = [this](const TrackId tid) {
                return soloCoordinator_ != nullptr && soloCoordinator_->soloIsolationRequested(tid);
            };
            proxyPlaybackCoordinator_
                = std::make_unique<proxy_playback::ProxyPlaybackCoordinator>(std::move(pbDeps));
        }

        // Live MIDI input (keyboard → hosts → take capture): message-thread owner of devices,
        // Monitor/Arm runtime flags and the routing published to the engine's bus. Built after
        // the instrument runtime and the playback-source coordinator it talks to.
        liveMidiInputCoordinator_ = std::make_unique<LiveMidiInputCoordinator>(
            session,
            deviceManager,
            playbackEngine_,
            *instrumentRuntimeCoordinator_,
            latencyStore_,
            LiveMidiInputCoordinator::Callbacks{
                [this](const TrackId dest) {
                    if (proxyPlaybackCoordinator_ != nullptr)
                    {
                        proxyPlaybackCoordinator_->refreshDestination(dest);
                    }
                },
                [this](const TrackId dest) {
                    return proxyPlaybackCoordinator_ != nullptr && proxyPlaybackCoordinator_->isLiveMonitorOverrideActive(dest);
                },
                [this](const TrackId dest) {
                    return proxyPlaybackCoordinator_ != nullptr && proxyPlaybackCoordinator_->isLiveRecordingOverrideActive(dest);
                },
                [this](const TrackId dest) {
                    return proxyPlaybackCoordinator_ != nullptr && proxyPlaybackCoordinator_->isPlayingProxy(dest);
                },
                [this] {
                    if (instrumentTimelineRowCoordinator_ != nullptr)
                    {
                        instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
                    }
                    trackLanesView.repaint();
                    inspectorView_.refreshFromSession();
                },
                [](const juce::String& line) {
                    juce::Logger::writeToLog(line);
                    // Scenario runs keep the take / pass bookkeeping next to the step log.
                    if (isStabilityTestModeActive())
                    {
                        appendStabilityRunLine("  " + line);
                    }
                },
            });
        // ---- Solo (spec §§1–7): ONE command path for every S button and the memory strip, plus
        // the derived `SoloMuteView` publish flow. Built after the proxy / undo / live-MIDI
        // coordinators it consults (all access is null-checked for teardown safety anyway).
        soloCoordinator_ = std::make_unique<SoloCoordinator>(
            session,
            playbackEngine_,
            SoloCoordinator::Callbacks{
                [this](const TrackId tid) {
                    return proxyPlaybackCoordinator_ != nullptr
                           && proxyPlaybackCoordinator_->isPlayingProxy(tid);
                },
                [this](const TrackId tid) {
                    // Same lazy-ensure the proxy coordinator's SecondaryLive override uses.
                    return instrumentRuntimeCoordinator_ != nullptr
                           && instrumentRuntimeCoordinator_->ensureSecondaryInstrumentLoadedForTrack(tid);
                },
                [this](const TrackId tid) {
                    if (proxyPlaybackCoordinator_ != nullptr)
                    {
                        proxyPlaybackCoordinator_->refreshDestination(tid);
                    }
                },
                [this] { refreshAllSoloUi(); },
                [](const juce::String& reason) {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Solo", reason);
                },
                [this](const juce::String& label, const int memoryIndex, std::function<bool()> mutator) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableSoloMemoryEdit(label, memoryIndex,
                                                                            std::move(mutator));
                    }
                    else if (mutator != nullptr)
                    {
                        juce::ignoreUnused(mutator());
                    }
                },
            });
        trackLanesView.setSoloUiHooks(makeSoloUiHooks());

        recordingCoordinator_->setRecordStartBlockedPredicate([this]() -> juce::String {
            return isProjectLoadInProgress() ? juce::String("A project is still opening. Wait until it has finished loading.")
                                             : juce::String();
        });
        recordingCoordinator_->setLiveMidiTakeCallbacks(
            [this] { return liveMidiInputCoordinator_->armedTracksReadyToRecord(); },
            [this]() -> juce::StringArray {
                juce::StringArray lines;
                for (const auto& s : liveMidiInputCoordinator_->armedRowsStatus())
                {
                    if (!s.ready)
                    {
                        lines.add(s.trackName + ": " + s.reason);
                    }
                }
                return lines;
            },
            [this](const std::int64_t startSample, const double sr, const bool cycleActive,
                   const std::int64_t locL, const std::int64_t locR, const std::uint32_t wrapSerialAtStart) {
                liveMidiInputCoordinator_->beginTake(startSample, sr, cycleActive, locL, locR, wrapSerialAtStart);
                trackLanesView.repaint();
            },
            [this](const RecordRunBoundaries& run) {
                const LiveMidiTakeCommitResult r = liveMidiInputCoordinator_->commitTake(
                    run.startTimelineSample, run.startMonoSample, run.stopTimelineSample, run.stopMonoSample,
                    run.stopWrapSerial);
                if (r.captureOverflowSeen)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::InfoIcon, "Recording",
                        "The MIDI take was committed, but some incoming MIDI could not be kept "
                        "(capture queue overflow). Held notes were ended at the stop boundary.");
                }
                refreshInstrumentUi();
                return r.clipsCreated;
            },
            [this] { liveMidiInputCoordinator_->abortTake(); },
            [this](const juce::String& label, std::function<void()> commit) {
                if (undoRedoCoordinator_ != nullptr)
                {
                    undoRedoCoordinator_->executeUndoableRecordingCommit(label, std::move(commit));
                }
                else
                {
                    commit();
                }
            },
            [this](const bool pending) { liveMidiInputCoordinator_->setTakePending(pending); });

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
                    // "exists" = present AND renderable (usable Primary). A missing
                    // Primary keeps its honest derived status (Current under the
                    // recorded configuration, otherwise Stale) but never arms policy
                    // state — no impossible render is ever queued.
                    id.exists = cur.destinationExists && cur.primaryAvailable
                                && cur.expectedFingerprint.isNotEmpty();
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
                return anyRecordingInProgress()
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

        // P1I: proxy status/control seams (§19), shown in the track-header "Instrument
        // alternatives" popup. The view displays the pure ProxyStatusModel output and invokes the
        // narrow service actions; every wording and availability rule lives in the tested model,
        // not in the component.
        {
            InstrumentProxyUiHost proxyUi;
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
            instrumentProxyUiHost_ = std::move(proxyUi);
        }

        // P2: "Instrument alternatives" seams (steering §17/§19, PID-008/PID-009), shown in the
        // track-header popup. Secondary configuration is persisted project state — every change
        // dirties through the normal configuration-edit mechanism (§18.3) and is stripped from
        // musical undo.
        {
            InstrumentSecondaryUiHost secUi;
            secUi.isInstrumentDestination = [this](const TrackId tid) {
                return instrumentRuntimeCoordinator_ != nullptr
                       && instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                              != nullptr;
            };
            secUi.getView = [this](const TrackId tid) {
                InstrumentSecondaryUiHost::View v;
                if (instrumentRuntimeCoordinator_ == nullptr)
                {
                    return v;
                }
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
                ExperimentalInstrumentHost* const h
                    = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
                if (c == nullptr)
                {
                    return v;
                }
                if (h != nullptr && h->hasInstrument())
                {
                    v.primaryText = h->getInstrumentNameForUi();
                }
                else
                {
                    const juce::String name = c->getPrimaryIdentityNameForUi();
                    v.primaryText = (name.isNotEmpty() ? name : juce::String("Primary"))
                                    + " (missing)";
                }
                v.hasSecondary = c->hasSecondaryInstrument();
                if (v.hasSecondary)
                {
                    v.secondaryText = c->getSecondaryDescriptor().name;
                    ExperimentalInstrumentHost* const sh
                        = instrumentRuntimeCoordinator_->getSecondaryInstrumentHostForTrack(tid);
                    if (sh == nullptr || !sh->hasInstrument())
                    {
                        // Distinguish "not needed yet" (lazy loading is normal and healthy) from
                        // a real recorded failure so the popup can surface the reason + Retry
                        // (P2 fix 2). Reading this NEVER instantiates the Secondary.
                        const bool failed = instrumentRuntimeCoordinator_
                                                ->getSecondaryLoadFailureReasonForTrack(tid)
                                                .isNotEmpty();
                        v.secondaryText += failed ? " (load failed)" : " (loads when needed)";
                    }
                }
                v.forcedMidiChannel = c->getSecondaryForcedMidiChannel();
                return v;
            };
            secUi.listCatalogInstrumentNames = [] {
                juce::StringArray names;
                std::vector<mini_daw::InstrumentCatalogEntry> entries;
                if (mini_daw::loadInstrumentCatalogFromCache(entries))
                {
                    for (const auto& e : entries)
                    {
                        names.add(e.description.name);
                    }
                }
                // Dedicated-kind instruments (HALion Sonic family) resolve through their own
                // cache path — append them AFTER the scanned entries so the itemId -> index
                // contract stays intact (selectSecondaryFromCatalog maps indexes identically).
                if (!scannedCatalogHasHalionSonic(entries))
                {
                    for (const auto& d : listDedicatedSecondaryChoices())
                    {
                        names.add(d.description.name);
                    }
                }
                return names;
            };
            secUi.selectSecondaryFromCatalog = [this](const TrackId tid, const int catalogIndex) {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                if (c == nullptr || catalogIndex < 0)
                {
                    return;
                }
                // Index contract mirrors listCatalogInstrumentNames exactly: scanned catalogue
                // entries first (may be legitimately empty), then the dedicated-kind choices.
                std::vector<mini_daw::InstrumentCatalogEntry> entries;
                (void)mini_daw::loadInstrumentCatalogFromCache(entries);
                juce::PluginDescription chosenDescription;
                juce::String chosenBundlePath;
                if (catalogIndex < (int)entries.size())
                {
                    const mini_daw::InstrumentCatalogEntry& entry = entries[(size_t)catalogIndex];
                    chosenDescription = entry.description;
                    chosenBundlePath = entry.bundlePath;
                }
                else
                {
                    if (scannedCatalogHasHalionSonic(entries))
                    {
                        return; // no dedicated rows were appended for this list
                    }
                    const auto dedicated = listDedicatedSecondaryChoices();
                    const int dedicatedIndex = catalogIndex - (int)entries.size();
                    if (dedicatedIndex >= (int)dedicated.size())
                    {
                        return;
                    }
                    chosenDescription = dedicated[(size_t)dedicatedIndex].description;
                    chosenBundlePath
                        = dedicated[(size_t)dedicatedIndex].bundle.getFullPathName();
                }
                ProjectFileGenericVst3DescriptorV1 desc;
                mini_daw::fillProjectGenericVst3DescriptorFromPluginDescription(
                    desc, chosenDescription);
                if (c->setSecondaryInstrumentFromUi(desc, chosenBundlePath))
                {
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
                    instrumentRuntimeCoordinator_->noteSecondaryConfigurationChanged(tid);
                    // Explicit configuration is an "actually needed" moment: instantiate now so
                    // selection problems surface immediately (failure latches; honest silence
                    // remains the fallback), then re-evaluate the playback source.
                    (void)instrumentRuntimeCoordinator_->ensureSecondaryInstrumentLoadedForTrack(
                        tid);
                    if (proxyPlaybackCoordinator_ != nullptr)
                    {
                        proxyPlaybackCoordinator_->refreshDestination(tid);
                    }
                }
            };
            secUi.removeSecondary = [this](const TrackId tid) {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                if (c == nullptr)
                {
                    return;
                }
                if (c->clearSecondaryInstrumentFromUi())
                {
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
                    instrumentRuntimeCoordinator_->noteSecondaryConfigurationChanged(tid);
                    if (proxyPlaybackCoordinator_ != nullptr)
                    {
                        proxyPlaybackCoordinator_->refreshDestination(tid);
                    }
                }
            };
            secUi.openSecondaryEditor = [this](const TrackId tid) {
                if (instrumentRuntimeCoordinator_ == nullptr)
                {
                    return;
                }
                // EXPLICIT user action: bypass the automatic failure latch (P2 fix 2) — a fresh
                // load attempt runs even after earlier automatic attempts failed.
                if (!instrumentRuntimeCoordinator_->retrySecondaryInstrumentLoadForTrack(tid))
                {
                    return;
                }
                if (ExperimentalInstrumentHost* const sh
                    = instrumentRuntimeCoordinator_->getSecondaryInstrumentHostForTrack(tid))
                {
                    sh->openNativeEditor();
                }
            };
            secUi.setChannelMapping = [this](const TrackId tid, const int forcedChannel) {
                InstrumentTrackController* const c
                    = instrumentRuntimeCoordinator_ != nullptr
                          ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid)
                          : nullptr;
                if (c == nullptr)
                {
                    return;
                }
                if (c->setSecondaryForcedMidiChannelFromUi(forcedChannel))
                {
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
                    instrumentRuntimeCoordinator_->noteSecondaryConfigurationChanged(tid);
                }
            };
            secUi.getLoadFailureReason = [this](const TrackId tid) {
                return instrumentRuntimeCoordinator_ != nullptr
                           ? instrumentRuntimeCoordinator_->getSecondaryLoadFailureReasonForTrack(tid)
                           : juce::String{};
            };
            secUi.retryLoad = [this](const TrackId tid) {
                const bool ok = instrumentRuntimeCoordinator_ != nullptr
                                && instrumentRuntimeCoordinator_->retrySecondaryInstrumentLoadForTrack(tid);
                if (ok && proxyPlaybackCoordinator_ != nullptr)
                {
                    proxyPlaybackCoordinator_->refreshDestination(tid);
                }
                return ok;
            };
            instrumentSecondaryUiHost_ = std::move(secUi);
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
                    // "exists" = renderable for the preparation's one-shot render.
                    // A missing Primary with a Current proxy is Ready (collected
                    // as-is); missing Primary with a non-current proxy is Blocked.
                    id.exists = cur.destinationExists && cur.primaryAvailable
                                && cur.expectedFingerprint.isNotEmpty();
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
                return anyRecordingInProgress()
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
                    return anyRecordingInProgress()
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
                        instrumentMidiImportCoordinator_->importMidiFileForTrack(laneTid);
                    }
                },
                // P2: track-header "Instrument alternatives" popup (replaces Inspector sections).
                [this](TrackId laneTid, juce::Rectangle<int> screenAnchor) {
                    instrument_alternatives_popup::show(laneTid,
                                                        screenAnchor,
                                                        instrumentProxyUiHost_,
                                                        instrumentSecondaryUiHost_);
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
        // Solo: Instrument + Midi rows consume the same seam as the audio / group headers.
        instrumentTimelineRowCoordinator_->setSoloUiHooks(makeSoloUiHooks());
        // Live MIDI: Monitor / Arm cells of Instrument + Midi rows read and toggle the runtime
        // flags owned by the live-MIDI coordinator (independent of row selection and of each other).
        instrumentTimelineRowCoordinator_->setLiveMidiCallbacks(
            [this](const TrackId tid) {
                return liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isMonitorEnabled(tid);
            },
            [this](const TrackId tid) {
                if (liveMidiInputCoordinator_ != nullptr)
                {
                    liveMidiInputCoordinator_->setMonitorEnabled(tid, !liveMidiInputCoordinator_->isMonitorEnabled(tid));
                }
            },
            [this](const TrackId tid) {
                return liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isRecordArmed(tid);
            },
            [this](const TrackId tid) {
                if (liveMidiInputCoordinator_ != nullptr)
                {
                    liveMidiInputCoordinator_->setRecordArmed(tid, !liveMidiInputCoordinator_->isRecordArmed(tid));
                }
            },
            [this](const TrackId tid) {
                return liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isTrackMidiActive(tid);
            },
            [this](const TrackId tid) {
                InstrumentTimelineRowCoordinator::Callbacks::LiveTakePreview preview;
                if (liveMidiInputCoordinator_ == nullptr)
                {
                    return preview;
                }
                const LiveMidiTakePreviewInfo info = liveMidiInputCoordinator_->takePreviewInfoForTrack(tid);
                if (!info.active)
                {
                    return preview;
                }
                preview.active = true;
                preview.currentPassStartSample = info.recordStartSample;
                if (info.cycleActive)
                {
                    // Pass geometry from the TRANSPORT's wrap count (the same counter the engine
                    // advances at the wrap) — never a lane-local pass number.
                    const std::uint32_t wraps = transport.readCycleWrapCountForUi() - info.wrapSerialAtStart;
                    if (wraps > 0)
                    {
                        preview.currentPassStartSample = info.leftLocatorSample;
                        preview.completedStart = juce::jmin(info.recordStartSample, info.leftLocatorSample);
                        preview.completedEndExclusive = info.rightLocatorSample;
                    }
                }
                return preview;
            },
            [this]() -> double {
                return std::isfinite(lastPlayheadFrameDisplaySamples_)
                           ? lastPlayheadFrameDisplaySamples_
                           : (double)transport.readPlayheadSamplesForUi();
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
                    return anyRecordingInProgress()
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
        clipPasteCallbacks.isRecording = [this] { return anyRecordingInProgress(); };
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
        // §12.2 missing-Primary correction: the audio device typically STARTS before these
        // hooks exist, so the initial audioDeviceAboutToStart never reached
        // prepareExperimentalInstrumentHostsForDevice — lastPreparedDevice* stayed unset and
        // every host whose plugin failed to load (the genuine missing-Primary case) kept a
        // 0x0 stereo scratch: the published proxy view was Current yet the proxy branch
        // bailed on every block (silent playback on machines without the plugin). Stamp the
        // already-running device spec now, exactly as audioDeviceAboutToStart would have.
        if (juce::AudioIODevice* dev = deviceManager.getCurrentAudioDevice())
        {
            instrumentRuntimeCoordinator_->prepareExperimentalInstrumentHostsForDevice(
                dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
        }
        // P2 audition gate (steering §17 audition split, PID-008): Secondary audition of
        // UI/editor notes is allowed while the transport is stopped, or while the Secondary
        // itself IS the transport source — never layered over proxy transport playback.
        instrumentRuntimeCoordinator_->setSecondaryAuditionGate([this](const TrackId tid) {
            if (transport.readPlaybackIntentForUi() != PlaybackIntent::Playing)
            {
                return true;
            }
            return instrumentRuntimeCoordinator_ != nullptr
                   && instrumentRuntimeCoordinator_->isSecondaryTransportActive(tid);
        });
        trackLanesView.setStructuralTimelineEditBlockedPredicate([this]() {
            // Power / delete / duplicate / inserts are not realtime-safe paths: blocked while
            // Playing (not mute) and while a staged project load owns the session.
            return anyRecordingInProgress()
                   || (recordingCoordinator_ != nullptr
                       && recordingCoordinator_->isCountInActive())
                   || transport.readPlaybackIntentForUi() == PlaybackIntent::Playing
                   || isProjectLoadInProgress();
        });
        trackLanesView.setInstrumentMidiClipMoveBlockedPredicate([this]() {
            return anyRecordingInProgress()
                   || (recordingCoordinator_ != nullptr
                       && recordingCoordinator_->isCountInActive())
                   || isProjectLoadInProgress();
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
            if (isProjectLoadInProgress())
            {
                return; // the staged load owns the session until finalize
            }
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
                    if (safeThis == nullptr || result == 0 || safeThis->isProjectLoadInProgress())
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
                        (void)safeThis->addMidiTrackFromUi();
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
            [this] { invokeToggleMixerWindowFromWindowShortcut(); },
            [this] { return mixerWindow_ != nullptr && mixerWindow_->isVisible(); },
        });
        menuBar_ = std::make_unique<juce::MenuBarComponent>(mainMenuModel_.get());
        addAndMakeVisible(*menuBar_);

        editToolIconStrip_.onToolSelected = [this](EditTool t) { applyEditToolSelection(t); };
        addAndMakeVisible(editToolIconStrip_);

        // Solo memory strip (spec §3): four toggle buttons above the track-header column, laid
        // out by `applyTransportControlsLayout`. All semantics live in `SoloCoordinator`.
        soloMemoryStrip_.activeMemoryIndexProvider = [this] {
            return soloCoordinator_ != nullptr ? soloCoordinator_->activeSoloMemoryIndex() : -1;
        };
        soloMemoryStrip_.onMemoryButtonClick = [this](const int memoryIndex) {
            if (soloCoordinator_ != nullptr)
            {
                soloCoordinator_->handleSoloMemoryButtonClick(memoryIndex);
            }
        };
        addAndMakeVisible(soloMemoryStrip_);

        // Shared track heights (one-shot commands, not a mode): item ids match the preset enum + 1.
        // "Custom" is only the no-selection status text — deliberately NOT an item.
        for (const auto preset : { track_row_heights::TrackRowHeightPreset::Micro, track_row_heights::TrackRowHeightPreset::Mini,
                                   track_row_heights::TrackRowHeightPreset::Small, track_row_heights::TrackRowHeightPreset::Medium,
                                   track_row_heights::TrackRowHeightPreset::Large })
        {
            trackRowHeightPresetCombo_.addItem(track_row_heights::displayNameForPreset(preset),
                                               trackRowHeightComboIdForPreset(preset));
        }
        trackRowHeightPresetCombo_.setTextWhenNothingSelected("Custom");
        trackRowHeightPresetCombo_.setTooltip("Set the height of all arrangement tracks");
        // Never take keyboard focus: a focused combo would consume Space (play/pause) after a click.
        trackRowHeightPresetCombo_.setWantsKeyboardFocus(false);
        trackRowHeightPresetCombo_.onChange = [this] {
            const int id = trackRowHeightPresetCombo_.getSelectedId();
            if (id <= 0)
            {
                return; // deselected programmatically (Custom status) — no command.
            }
            // Item id = grid enum value + 1 (Micro 1 … Large 5): one-shot preset command.
            trackLanesView.applyTrackRowHeightPreset(static_cast<track_row_heights::TrackRowHeightPreset>(id - 1));
        };
        addAndMakeVisible(trackRowHeightPresetCombo_);
        refreshTrackRowHeightPresetComboFromLanes();

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
                    // Live MIDI: Monitor / Arm are runtime-only and start OFF in the next project;
                    // a half-finished take belongs to the OLD project and is dropped (never
                    // half-committed). The input configuration itself comes from the file.
                    if (recordingCoordinator_ != nullptr)
                    {
                        recordingCoordinator_->abortMidiTakeForProjectReplace();
                    }
                    if (liveMidiInputCoordinator_ != nullptr)
                    {
                        liveMidiInputCoordinator_->clearRuntimeStateForProjectReplace();
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
                    // Solo: the session reset its transient solo state and adopted the loaded
                    // memories — republish so the engine drops any previous project's view and
                    // the S cells / memory strip repaint (memory buttons all off after load).
                    if (soloCoordinator_ != nullptr)
                    {
                        soloCoordinator_->republishDerivedView();
                    }
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
                // Staged load: centre the progress window over the main window.
                [this]() -> juce::Component* { return getTopLevelComponent(); },
                // v26 row heights for save: preset key + every row's actual effective height.
                [this]() -> std::optional<ProjectFileTrackRowHeightsV1> {
                    ProjectFileTrackRowHeightsV1 rh;
                    rh.presetKey = track_row_heights::persistenceKeyForPreset(
                        trackLanesView.lastChosenTrackRowHeightPreset());
                    rh.perTrackRowHeightPx = trackLanesView.allTrackRowHeightsPxForProjectSave();
                    return rh;
                },
                // v26 row heights after load: always applied — pre-v26 files reset to Medium.
                [this](const ProjectFileV1& loaded) {
                    std::vector<std::pair<TrackId, int>> perTrack;
                    perTrack.reserve(loaded.tracks.size());
                    for (const auto& t : loaded.tracks)
                    {
                        perTrack.emplace_back(t.id, t.rowHeightPx);
                    }
                    trackLanesView.applyTrackRowHeightsFromLoadedProject(loaded.trackRowHeightPreset,
                                                                         perTrack);
                    refreshTrackRowHeightPresetComboFromLanes();
                    appendProjectLoadDiagnosticLine(
                        "load: row heights preset="
                        + track_row_heights::persistenceKeyForPreset(
                            trackLanesView.lastChosenTrackRowHeightPreset())
                        + (loaded.trackRowHeightPreset.isEmpty() ? " (default, no key)" : ""));
                },
            });

        // Stability C5: app-level states that must block a periodic autosave tick. Everything
        // else (save/load/export/undo/redo/delete) runs synchronously on the message thread and
        // cannot overlap the timer; modal prompts are covered inside the coordinator.
        projectIoCoordinator_->setAutosaveBlockReasonProvider([this]() -> juce::String {
            if (anyRecordingInProgress())
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
        addAndMakeVisible(inspectorPanel_);
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
                // The MIDI lanes' running-take preview draws its right edge from this same frame
                // value; only the strip it grew by is invalidated (recording lanes only).
                lastPlayheadFrameDisplaySamples_ = displaySamples;
                if (liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isTakeActive()
                    && instrumentTimelineRowCoordinator_ != nullptr)
                {
                    instrumentTimelineRowCoordinator_->repaintLiveTakePreviewGrowth(displaySamples);
                }
            });
        addAndMakeVisible(*lanePlayheadOverlay_);
        // Vertical arrangement scrollbar: a plain view onto the lanes view's ONE vertical scroll
        // model (the wheel, header + lane layout and this bar share `verticalScrollOffsetPx_`).
        // Range = real content height from the laid-out rows (varying heights included); the bar
        // auto-hides when everything fits. Thumb drags / trough clicks / arrow steps feed back
        // through the same clamp path the wheel uses; model changes (resize, track add /
        // duplicate / delete / undo, project switch, row-height drags) re-sync the bar silently.
        arrangementVerticalScrollBar_.setAutoHide(true);
        arrangementVerticalScrollBar_.setSingleStepSize(32.0);
        arrangementVerticalScrollBar_.addListener(&arrangementVerticalScrollBarListener_);
        trackLanesView.setOnVerticalScrollModelChanged([this] {
            syncArrangementVerticalScrollBarFromLanes();
            // Track add / duplicate / delete / undo / project switch can all change whether every
            // row matches one preset — keep the dropdown status in sync from the same signal.
            refreshTrackRowHeightPresetComboFromLanes();
        });
        // Row-height changes (drag, preset command, load apply): dropdown status + dirty flag.
        // Only user-origin edits mark the project dirty; the load apply passes false.
        trackLanesView.setOnTrackRowHeightsChanged([this](const bool byUserEdit) {
            if (byUserEdit && projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->markProjectDirtyFromEdit();
            }
            refreshTrackRowHeightPresetComboFromLanes();
        });
        // Track colour (palette from a header's icon / number segment): ONE narrow undo step on
        // exactly that track's colour + dirty flag; the coordinator's refresh callback repaints.
        trackLanesView.setOnTrackColourRequested([this](const TrackId tid, const TrackColourKey key) {
            if (undoRedoCoordinator_ != nullptr)
            {
                undoRedoCoordinator_->executeUndoableTrackColourEdit(tid, key);
            }
        });
        // Visual track groups (purely visual; groups spec §7): Create / Rename / Ungroup run as
        // narrow group-metadata undo steps (never a timeline snapshot); collapse / expand follows
        // the display-change policy — project dirty, NO undo entry (like row heights above).
        {
            VisualTrackGroupUiHooks groupHooks;
            groupHooks.createGroup = [this](juce::String name, std::vector<TrackId> members) {
                if (undoRedoCoordinator_ == nullptr)
                {
                    return;
                }
                undoRedoCoordinator_->executeUndoableVisualTrackGroupsEdit(
                    "Create track group",
                    [this, name = std::move(name), members = std::move(members)]() mutable {
                        return session.createVisualTrackGroup(std::move(name), std::move(members))
                            .has_value();
                    });
                trackLanesView.refreshVisualTrackGroupsFromSession();
            };
            groupHooks.renameGroup = [this](const int groupId, juce::String newName) {
                if (undoRedoCoordinator_ == nullptr)
                {
                    return;
                }
                undoRedoCoordinator_->executeUndoableVisualTrackGroupsEdit(
                    "Rename track group",
                    [this, groupId, newName = std::move(newName)]() mutable {
                        session.renameVisualTrackGroup(groupId, std::move(newName));
                        return true; // no-op suppression: the coordinator skips equal before/after
                    });
                trackLanesView.refreshVisualTrackGroupsFromSession();
            };
            groupHooks.ungroup = [this](const int groupId) {
                if (undoRedoCoordinator_ == nullptr)
                {
                    return;
                }
                undoRedoCoordinator_->executeUndoableVisualTrackGroupsEdit(
                    "Ungroup tracks",
                    [this, groupId] {
                        session.removeVisualTrackGroup(groupId);
                        return true;
                    });
                trackLanesView.refreshVisualTrackGroupsFromSession();
            };
            groupHooks.setCollapsed = [this](const int groupId, const bool collapsed) {
                if (session.setVisualTrackGroupCollapsed(groupId, collapsed))
                {
                    if (projectIoCoordinator_ != nullptr)
                    {
                        projectIoCoordinator_->markProjectDirtyFromEdit();
                    }
                    trackLanesView.refreshVisualTrackGroupsFromSession();
                }
            };
            trackLanesView.setVisualTrackGroupUiHooks(std::move(groupHooks));
        }
        addAndMakeVisible(arrangementVerticalScrollBar_);
        syncArrangementVerticalScrollBarFromLanes();
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
                [this] { return anyRecordingInProgress(); },
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
                [this] {
                    instrumentRuntimeCoordinator_->deactivateKeyedInstrumentControllersOnly();
                    // The now-DESELECTED instrument/MIDI headers must repaint immediately —
                    // their paint reads the controller flag lazily.
                    if (instrumentTimelineRowCoordinator_ != nullptr)
                    {
                        instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
                    }
                },
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
        trackLanesEditCoordinator_->setOnMidiInputAssignmentChanged([this] {
            if (liveMidiInputCoordinator_ != nullptr)
            {
                liveMidiInputCoordinator_->refreshDevicesAndRouting();
            }
        });

        // Audio Input selector (Inspector, audio rows): the combo lists the ACTIVE device's
        // enabled physical input channels by name. Message-thread only. The mixer strips read
        // the same two snapshots (`makeAudioInputDeviceSnapshot`, `makeMidiInputSnapshotForTrack`).
        inspectorView_.setAudioInputDeviceSnapshotProvider([this] { return makeAudioInputDeviceSnapshot(); });
        inspectorView_.setMidiInputSnapshotProvider([this](const TrackId tid) { return makeMidiInputSnapshotForTrack(tid); });

        // Mixer window (F3 / Window > Mixer): one instance for the app lifetime, hidden until
        // asked for; its strips are bound to explicit TrackIds through `mixerBindings_` — the
        // Inspector's undoable actions, the headers' button paths and the insert seam.
        buildMixerStripBindings();
        mixerWindow_ = std::make_unique<MixerWindow>(mixerBindings_, &levelMeterHub_, MixerWindow::Shortcuts{
            [this] { invokeToggleMixerWindowFromWindowShortcut(); },
            [this] { invokeSaveProjectFromWindowShortcut(); },
            [this] { invokeUndoFromWindowShortcut(); },
            [this] { invokeRedoFromWindowShortcut(); },
            [this] { invokeRecordToggleFromWindowShortcut(); },
            [this] { invokeJumpToLeftLocatorFromWindowShortcut(); },
            [this] { invokePlayPauseToggleFromWindowShortcut(); },
        });
        restoreMixerLayoutFromSettings();
        mixerWindow_->onBoundsSettled = [this](const juce::Rectangle<int> b) {
            uiLayoutSettings_.setMixerWindowBounds(b);
            uiLayoutSettings_.save();
        };
        mixerWindow_->content().onSectionVisibilityChanged = [this](const mixer_layout::SectionVisibility& v) {
            for (int i = 0; i < mixer_layout::kSectionCount; ++i)
            {
                const auto s = static_cast<mixer_layout::Section>(i);
                uiLayoutSettings_.setMixerSectionShown(mixer_layout::sectionKey(s), v.get(s));
            }
            uiLayoutSettings_.save();
        };
        // Shared section heights: written once per completed divider drag (never per mouse move).
        mixerWindow_->content().onSectionHeightsChanged = [this](const mixer_layout::SectionHeights& h) {
            for (int i = 0; i < mixer_layout::kSectionCount; ++i)
            {
                const auto s = static_cast<mixer_layout::Section>(i);
                if (mixer_layout::isUpperSection(s))
                {
                    uiLayoutSettings_.setMixerSectionHeightPx(mixer_layout::sectionKey(s), h.get(s));
                }
            }
            uiLayoutSettings_.save();
        };
        mixerWindow_->onVisibilityChanged = [this](bool) {
            if (menuBar_ != nullptr)
            {
                menuBar_->repaint(); // the Window > Mixer tick follows the real state
            }
            levelMeterHub_.refreshInterestNow();
        };

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
        arrangementVerticalScrollBar_.removeListener(&arrangementVerticalScrollBarListener_);
        trackLanesView.setOnVerticalScrollModelChanged(nullptr);
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
        if (anyRecordingInProgress() || recordingCoordinator_->isCountInActive())
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

    /// [Message thread] True while the staged project load owns the session (see
    /// `ProjectIoCoordinator::isLoadInProgress`): structural edits, undo / redo, save, transport
    /// start and track creation are refused until finalize released the gates.
    [[nodiscard]] bool isProjectLoadInProgress() const noexcept
    {
        return projectIoCoordinator_ != nullptr && projectIoCoordinator_->isLoadInProgress();
    }

    void invokeUndoFromWindowShortcut() override
    {
        if (undoRedoCoordinator_ != nullptr && !isProjectLoadInProgress())
        {
            undoRedoCoordinator_->invokeUndoFromWindowShortcut();
        }
    }

    void invokeRedoFromWindowShortcut() override
    {
        if (undoRedoCoordinator_ != nullptr && !isProjectLoadInProgress())
        {
            undoRedoCoordinator_->invokeRedoFromWindowShortcut();
        }
    }

    void invokeSaveProjectFromWindowShortcut() override
    {
        if (projectIoCoordinator_ != nullptr)
        {
            projectIoCoordinator_->saveProject(); // refuses by itself while a load is in progress
        }
    }

    // [Message thread] F3 / Window > Mixer: show or hide the one mixer window. Nothing else
    // changes — no transport, recording, plug-in or audio state is touched by the toggle.
    void invokeToggleMixerWindowFromWindowShortcut() override
    {
        if (mixerWindow_ != nullptr)
        {
            mixerWindow_->toggleMixer();
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
            // Faithful to every REAL edit path (P1MC): a musical edit dirties the project.
            // Real MIDI edits mark this through the undo/edit seam; this diagnostic seam must
            // behave identically or the metadata-checkpoint guard would see a clean project.
            if (projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->markProjectDirtyFromEdit();
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
        // P1MC integration plan: observe the REAL dirty flag (metadata-checkpoint guard proof).
        cb.isProjectDirty = [this] {
            return projectIoCoordinator_ != nullptr && projectIoCoordinator_->isProjectDirty();
        };
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

        // --- MIDI-clip parity: import + cross-track moves for a plain TrackKind::Midi row ---
        // Runs the production paths a user reaches from the track header: the parse+append the MIDI
        // import coordinator performs after its guard, and the cross-track clip move the arrangement
        // drag commits. The FileChooser click and the mouse drag themselves stay manual checks.
        hooks.midiTrackParityVerify = [this](juce::String& failReason) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                failReason = "no instrument runtime coordinator";
                return false;
            }
            const TrackId midiTid = stabilityMidiRoutingMidiLowerTid_;
            const TrackId instTid = stabilityMidiRoutingInstTid_;
            InstrumentTrackController* const midiCtl
                = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(midiTid);
            InstrumentTrackController* const instCtl
                = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(instTid);
            if (midiCtl == nullptr || instCtl == nullptr)
            {
                failReason = "clip controller missing for the MIDI row or the instrument row";
                return false;
            }
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int midiIx = (snap != nullptr) ? snap->findTrackIndexById(midiTid) : -1;
            if (midiIx < 0 || snap->getTrack(midiIx).getKind() != TrackKind::Midi)
            {
                failReason = "fixture MIDI row missing or not TrackKind::Midi";
                return false;
            }
            const int midiClipsBefore = (int)midiCtl->getClips().size();
            const int instClipsBefore = (int)instCtl->getClips().size();
            if (midiClipsBefore <= 0 || midiCtl->getClips().front() == nullptr)
            {
                failReason = "fixture MIDI row has no clip to export";
                return false;
            }

            // A) Write a real Standard MIDI File from the MIDI row's own clip, then import it back
            //    onto that same MIDI row: exactly what "Import MIDI file..." does after its guard.
            const double sr = midiCtl->getTimelineSampleRate();
            const juce::File midFile
                = juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("MiniDAWLab-midi-parity-" + juce::String(juce::Time::currentTimeMillis())
                                    + ".mid");
            const InstrumentMidiClipExportResult exported = exportInstrumentMidiClipToMidiFile(
                *midiCtl->getClips().front(),
                snap->getTrack(midiIx).getMidiOutputChannel(),
                midFile,
                sr);
            if (!exported.ok || exported.notesExported <= 0)
            {
                failReason = "could not export a fixture MIDI file: " + exported.errorMessage;
                (void)midFile.deleteFile();
                return false;
            }
            ExperimentalMidiImportResult parsed
                = experimentalImportMidiFile(midFile, kDefaultExperimentalTicksPerQuarter);
            (void)midFile.deleteFile();
            if (!parsed.ok || parsed.notes.empty())
            {
                failReason = "production MIDI parse failed: " + parsed.combinedUserMessageLine();
                return false;
            }
            std::vector<int> importedPitches;
            importedPitches.reserve(parsed.notes.size());
            for (const auto& n : parsed.notes)
            {
                importedPitches.push_back(n.midiNote);
            }
            std::sort(importedPitches.begin(), importedPitches.end());
            const std::int64_t importStart = (std::int64_t)(sr * 4.0);
            const InstrumentMidiClipId importedId = midiCtl->appendImportedTimelineMidiClipAtSamples(
                std::move(parsed.notes), importStart, stabilityMidiParityClipName_);
            if (importedId == 0)
            {
                failReason = "import append onto the TrackKind::Midi row was refused";
                return false;
            }
            if ((int)midiCtl->getClips().size() != midiClipsBefore + 1)
            {
                failReason = "imported clip did not appear on the MIDI row";
                return false;
            }
            stabilityMidiParityImportedPitches_ = importedPitches;

            // Collects the sorted pitches of the clip named like the imported one.
            const auto pitchesOfParityClip = [this](const InstrumentTrackController& ctl) {
                std::vector<int> out;
                for (const auto& cp : ctl.getClips())
                {
                    if (cp == nullptr || cp->name != stabilityMidiParityClipName_)
                    {
                        continue;
                    }
                    for (const auto& n : cp->pattern.timelineNotes)
                    {
                        out.push_back(n.midiNote);
                    }
                }
                std::sort(out.begin(), out.end());
                return out;
            };
            if (pitchesOfParityClip(*midiCtl) != importedPitches)
            {
                failReason = "imported notes did not land intact on the MIDI row";
                return false;
            }

            // B) MIDI row -> instrument row through the production cross-track move.
            midiCtl->setSelectedClipIdsExclusive(importedId);
            if (!instrumentRuntimeCoordinator_->moveInstrumentMidiClipsBetweenTracks(
                    midiTid, instTid, { importedId }, 0))
            {
                failReason = "move MIDI row -> instrument row was refused";
                return false;
            }
            if ((int)midiCtl->getClips().size() != midiClipsBefore
                || (int)instCtl->getClips().size() != instClipsBefore + 1)
            {
                failReason = "clip counts wrong after MIDI row -> instrument row move (source "
                             + juce::String((int)midiCtl->getClips().size()) + ", dest "
                             + juce::String((int)instCtl->getClips().size()) + ")";
                return false;
            }
            if (pitchesOfParityClip(*instCtl) != importedPitches
                || !pitchesOfParityClip(*midiCtl).empty())
            {
                failReason = "notes did not transfer cleanly to the instrument row";
                return false;
            }

            // C) Instrument row -> MIDI row: the same move must work in the other direction.
            const std::vector<InstrumentMidiClipId> backIds = instCtl->getSelectedClipIds();
            if (backIds.empty())
            {
                failReason = "moved clip was not selected on the instrument row";
                return false;
            }
            if (!instrumentRuntimeCoordinator_->moveInstrumentMidiClipsBetweenTracks(
                    instTid, midiTid, backIds, 0))
            {
                failReason = "move instrument row -> MIDI row was refused";
                return false;
            }
            if ((int)midiCtl->getClips().size() != midiClipsBefore + 1
                || (int)instCtl->getClips().size() != instClipsBefore)
            {
                failReason = "clip counts wrong after instrument row -> MIDI row move";
                return false;
            }
            if (pitchesOfParityClip(*midiCtl) != importedPitches
                || !pitchesOfParityClip(*instCtl).empty())
            {
                failReason = "notes did not transfer cleanly back to the MIDI row";
                return false;
            }
            appendStabilityRunLine(
                "  parity: imported " + juce::String((int)importedPitches.size())
                + " notes onto Midi row " + juce::String((juce::int64)midiTid)
                + ", moved to instrument row " + juce::String((juce::int64)instTid) + " and back");
            return true;
        };

        hooks.midiTrackParityVerifyAfterReload = [this](juce::String& failReason) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                failReason = "no instrument runtime coordinator";
                return false;
            }
            InstrumentTrackController* const midiCtl
                = instrumentRuntimeCoordinator_->getMidiContentControllerForTrack(
                    stabilityMidiRoutingMidiLowerTid_);
            if (midiCtl == nullptr)
            {
                failReason = "midi content controller missing after reload";
                return false;
            }
            std::vector<int> pitches;
            int matchingClips = 0;
            for (const auto& cp : midiCtl->getClips())
            {
                if (cp == nullptr || cp->name != stabilityMidiParityClipName_)
                {
                    continue;
                }
                ++matchingClips;
                for (const auto& n : cp->pattern.timelineNotes)
                {
                    pitches.push_back(n.midiNote);
                }
            }
            if (matchingClips != 1)
            {
                failReason = "expected exactly one imported clip on the MIDI row after reload, found "
                             + juce::String(matchingClips);
                return false;
            }
            std::sort(pitches.begin(), pitches.end());
            if (pitches != stabilityMidiParityImportedPitches_)
            {
                failReason = "imported notes on the MIDI row changed across save/reload";
                return false;
            }
            return true;
        };

        // --- Global audio health probe (midi-import-audio and follow-up scenarios) ---
        // Pre-gain scenario: the same Session setter the Inspector's undoable edit ends in, the
        // stored value from the published snapshot, and the engine's device-output peak hold.
        hooks.setTrackPreGainDb = [this](const TrackId tid, const float dB) -> bool {
            return session.setTrackPreGainDb(tid, dB);
        };
        hooks.getTrackPreGainDb = [this](const TrackId tid) -> float {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            return idx >= 0 ? snap->getTrack(idx).getPreGainDb() : std::numeric_limits<float>::quiet_NaN();
        };
        hooks.readOutputPeakHoldAndReset = [this]() -> float {
            return playbackEngine_.readAndResetOutputPeakHoldForDiagnostics();
        };

        // Pre-gain through the REAL Inspector (user-flow reproduction on a real project). Each hook
        // reuses the exact production objects the UI uses; none of them bypasses the Inspector's
        // commit path or the header's mute/activate semantics.
        hooks.activateTrackLikeHeaderClick = [this](const TrackId tid) {
            // Same sequence as an audio header's name-strip click (TrackLanesView → onActivateName):
            // session active track, then the header-activated callback that refreshes the Inspector.
            session.setActiveTrack(tid);
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                instrumentRuntimeCoordinator_->deactivateKeyedInstrumentControllersOnly();
            }
            inspectorView_.refreshFromSession();
            trackLanesView.repaint();
        };
        hooks.inspectorTypePreGainAndReturn = [this](const juce::String& text) {
            inspectorView_.typePreGainTextLikeKeyboardForStabilityTest(text);
        };
        hooks.inspectorPreGainFieldText = [this]() -> juce::String {
            return inspectorView_.getPreGainFieldTextForStabilityTest();
        };
        hooks.inspectorPreGainFieldVisible = [this]() -> bool {
            return inspectorView_.isPreGainFieldVisibleForStabilityTest();
        };
        hooks.setTrackMutedLikeHeader = [this](const TrackId tid, const bool muted) {
            session.setTrackMuted(tid, muted);
            trackLanesView.syncTracksFromSession();
            trackLanesView.repaint();
            inspectorView_.refreshFromSession();
        };
        hooks.describeTrackForDiagnostics = [this](const TrackId tid) -> juce::String {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            if (idx < 0)
            {
                return "track " + juce::String((juce::int64)tid) + ": not in snapshot";
            }
            const Track& tr = snap->getTrack(idx);
            juce::String kind;
            switch (tr.getKind())
            {
                case TrackKind::Audio: kind = "audio"; break;
                case TrackKind::Instrument: kind = "instrument"; break;
                case TrackKind::Group: kind = "group"; break;
                case TrackKind::Master: kind = "master"; break;
                case TrackKind::Midi: kind = "midi"; break;
            }
            juce::String inserts;
            for (const InsertRowView& row : pluginHost_.getInsertRowsForTrack(tid))
            {
                inserts << (row.stage == InsertStage::Pre ? "Pre:" : "Post:") << row.displayName << " ";
            }
            juce::String s;
            s << "track " << juce::String((juce::int64)tid) << " kind=" << kind << " name=\"" << tr.getName()
              << "\" muted=" << (tr.isMuted() ? "YES" : "no") << " off=" << (tr.isTrackOff() ? "YES" : "no")
              << " fader=" << juce::String(tr.getChannelFaderGain(), 3) << " preGainDb=" << juce::String(tr.getPreGainDb(), 2)
              << " pan=" << juce::String(tr.getStereoPan(), 2) << " output=" << juce::String((juce::int64)tr.getRoutedOutputTrackId())
              << " monitor=" << (playbackEngine_.isTrackInputMonitoringEnabled(tid) ? "ON" : "off")
              << " inserts=[" << inserts.trim() << "] chainActiveForAudioThread="
              << (pluginHost_.audioThread_hasActivePluginForTrack(tid) ? "yes" : "no");
            return s;
        };
        hooks.findAudioTrackWithInsertNamed = [this](const juce::String& fragment) -> TrackId {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return kInvalidTrackId;
            }
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& tr = snap->getTrack(i);
                if (tr.getKind() != TrackKind::Audio)
                {
                    continue;
                }
                for (const InsertRowView& row : pluginHost_.getInsertRowsForTrack(tr.getId()))
                {
                    if (row.displayName.containsIgnoreCase(fragment))
                    {
                        return tr.getId();
                    }
                }
            }
            return kInvalidTrackId;
        };
        hooks.setInsertLevelTapTrack = [this](const TrackId tid) {
            pluginHost_.setInsertLevelTapTrackForDiagnostics(tid);
        };
        hooks.readAndResetInsertLevelTap = [this](float& before, float& after, double& rmsBefore, double& rmsAfter,
                                                  std::uint32_t& pre, std::uint32_t& post) {
            const PluginInsertHost::InsertLevelTapSnapshot s = pluginHost_.readAndResetInsertLevelTapForDiagnostics();
            before = s.peakBeforeFirstInsert;
            after = s.peakAfterLastInsert;
            rmsBefore = s.rmsBeforeFirstInsert;
            rmsAfter = s.rmsAfterLastInsert;
            pre = s.preStageBlocks;
            post = s.postStageBlocks;
        };
        hooks.readAndResetInsertLevelTapFull = [this]() -> StabilityRunnerHooks::InsertTapReading {
            const PluginInsertHost::InsertLevelTapSnapshot s = pluginHost_.readAndResetInsertLevelTapForDiagnostics();
            StabilityRunnerHooks::InsertTapReading r;
            r.peakBefore = s.peakBeforeFirstInsert;
            r.peakAfter = s.peakAfterLastInsert;
            r.rmsBefore = s.rmsBeforeFirstInsert;
            r.rmsAfter = s.rmsAfterLastInsert;
            r.dcBefore[0] = s.dcBeforeFirstInsert[0];
            r.dcBefore[1] = s.dcBeforeFirstInsert[1];
            r.preBlocks = s.preStageBlocks;
            r.postBlocks = s.postStageBlocks;
            return r;
        };
        hooks.seekTransportTo = [this](const std::int64_t sample) { transport.requestSeek(sample); };

        // --- Inserts scenario (same entry points as the VST3 picker / Inspector rows) -------------
        hooks.listAllTracks = [this]() -> std::vector<StabilityTrackInfo> {
            std::vector<StabilityTrackInfo> out;
            const auto snap = session.loadSessionSnapshotForAudioThread();
            for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                const Track& tr = snap->getTrack(i);
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
        hooks.addInsertLikePicker = [this](const TrackId tid, const bool pre, const juce::File& vst3) -> juce::Result {
            // Exactly what `Vst3PluginPickerCoordinator` does once the user picked an entry.
            const juce::Result r = pluginHost_.addInsertFromVst3File(tid, pre ? InsertStage::Pre : InsertStage::Post, vst3);
            inspectorView_.refreshFromSession();
            return r;
        };
        hooks.listInsertRows = [this](const TrackId tid) -> std::vector<StabilityInsertRowInfo> {
            std::vector<StabilityInsertRowInfo> out;
            for (const InsertRowView& row : pluginHost_.getInsertRowsForTrack(tid))
            {
                out.push_back(StabilityInsertRowInfo{ row.stage == InsertStage::Pre, row.displayName, row.unavailable });
            }
            return out;
        };

        // --- Header column scenario ---------------------------------------------------------------
        hooks.dragHeaderColumnLikeHandle = [this](const int deltaPx) {
            trackLanesView.simulateHeaderColumnHandleDragForStabilityTest(deltaPx);
        };
        hooks.getHeaderColumnWidthPreference = [this]() -> int { return trackLanesView.getTrackHeaderColumnWidthPx(); };
        hooks.getHeaderColumnEffectiveWidth = [this]() -> int { return trackLanesView.headerColumnWidthPx(); };
        hooks.readPersistedHeaderColumnWidth = [this]() -> std::optional<int> {
            UiLayoutSettingsStore fresh(uiLayoutSettings_.getFile());
            fresh.loadFromFile();
            return fresh.getTrackHeaderColumnWidthPx();
        };
        hooks.verifyHeaderColumnLayout = [this](juce::String& report, juce::String& failReason) -> bool {
            if (!trackLanesView.verifyHeaderColumnLayoutForDiagnostics(report, failReason))
            {
                return false;
            }
            // Siblings laid out by `applyTransportControlsLayout` must sit on the very same boundary.
            const int boundaryX = trackLanesView.getX() + trackLanesView.headerColumnWidthPx();
            report << "  ruler=" << rulerView.getBounds().toString() << " addTrackButton="
                   << addTrackCornerPlusButton_.getBounds().toString();
            if (lanePlayheadOverlay_ != nullptr)
            {
                report << " playheadOverlay=" << lanePlayheadOverlay_->getBounds().toString();
            }
            report << " boundaryX=" << boundaryX << "\n";
            if (rulerView.getX() != boundaryX)
            {
                failReason = "ruler starts at x=" + juce::String(rulerView.getX()) + " but the header boundary is x="
                             + juce::String(boundaryX);
                return false;
            }
            if (lanePlayheadOverlay_ != nullptr && lanePlayheadOverlay_->isVisible()
                && lanePlayheadOverlay_->getX() != boundaryX)
            {
                failReason = "playhead overlay starts at x=" + juce::String(lanePlayheadOverlay_->getX())
                             + " but the header boundary is x=" + juce::String(boundaryX);
                return false;
            }
            if (addTrackCornerPlusButton_.getRight() > boundaryX)
            {
                failReason = "add-track corner button crosses the header boundary";
                return false;
            }
            return true;
        };
        // --- Export levels scenario ----------------------------------------------------------------
        const auto toLevelStats = [](const level_meter::Reading& r) -> StabilityLevelStats {
            StabilityLevelStats s;
            for (int ch = 0; ch < 2; ++ch)
            {
                s.peak[ch] = r.peak[ch];
                s.overs[ch] = r.overs[ch];
                s.rms[ch] = r.rms(ch);
                s.dcOffset[ch] = r.dcOffset(ch);
            }
            s.nonFinite = r.nonFinite;
            s.frames = r.samples;
            s.valid = r.blocks > 0;
            return s;
        };
        // Diagnostics windows: independent of the Inspector panel's 30 Hz UI drains.
        hooks.drainMasterMeter = [this, toLevelStats]() -> StabilityLevelStats {
            return toLevelStats(playbackEngine_.drainMasterOutputLevelsForDiagnostics());
        };
        hooks.setMeteredTrack = [this](const TrackId tid) {
            // Route the Inspector panel's selection too (it would otherwise re-point the tap on
            // its next tick); the scenario activates rows through the header path where needed.
            playbackEngine_.setMeteredTrackForUi(tid);
        };
        hooks.drainTrackMeter = [this, toLevelStats]() -> StabilityLevelStats {
            return toLevelStats(playbackEngine_.drainMeteredTrackLevelsForDiagnostics());
        };
        hooks.runMixdownWithLevelReport = [this](const juce::File& outputFile, const bool mp3, const int bits,
                                                 StabilityLevelStats& statsOut) -> juce::Result {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                return juce::Result::fail("no audio device");
            }
            auto syncUi = [this] {
                if (transportPlayPauseStopController_ != nullptr)
                {
                    transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                }
            };
            mini_daw_audio_mixdown::MixdownExportLevelReport report;
            juce::Result r = juce::Result::ok();
            if (mp3)
            {
                r = mini_daw_audio_mixdown::exportStereoMixdownMp3Blocking(
                    transport, session, playbackEngine_, deviceManager, syncUi, outputFile,
                    /*bitrateKbps*/ 192, /*progressSink*/ nullptr, /*overwriteConfirmed*/ true, &report);
            }
            else
            {
                mini_daw_audio_mixdown::MixdownExportRequest req;
                req.outputFile = outputFile;
                req.sampleRate = device->getCurrentSampleRate();
                req.bits = bits == 16   ? mini_daw_audio_mixdown::MixdownWaveBits::Pcm16
                           : bits == 32 ? mini_daw_audio_mixdown::MixdownWaveBits::IeeeFloat32
                                        : mini_daw_audio_mixdown::MixdownWaveBits::Pcm24;
                req.overwriteConfirmed = true;
                req.levelReportOut = &report;
                r = mini_daw_audio_mixdown::exportStereoMixdownWavBlocking(
                    transport, session, playbackEngine_, deviceManager, syncUi, req);
            }
            for (int ch = 0; ch < 2; ++ch)
            {
                statsOut.peak[ch] = report.peak[ch];
                statsOut.overs[ch] = report.overs[ch];
                statsOut.rms[ch] = report.rms[ch];
                statsOut.dcOffset[ch] = report.dcOffset[ch];
                statsOut.firstSample[ch] = report.firstSample[ch];
                statsOut.lastSample[ch] = report.lastSample[ch];
            }
            statsOut.nonFinite = report.nonFinite;
            statsOut.frames = static_cast<std::uint64_t>(juce::jmax<std::int64_t>(0, report.frames));
            statsOut.valid = report.valid;
            return r;
        };
        hooks.getTrackChannelFaderGain = [this](const TrackId tid) -> float {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            return idx >= 0 ? snap->getTrack(idx).getChannelFaderGain() : 1.0f;
        };
        hooks.setTrackChannelFaderGain = [this](const TrackId tid, const float gain) {
            session.setTrackChannelFaderGain(tid, gain);
            inspectorView_.refreshFromSession();
        };
        hooks.getActiveLoopSpan = [this](std::int64_t& start, std::int64_t& length, double& sampleRate) -> bool {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (snap == nullptr || device == nullptr)
            {
                return false;
            }
            mini_daw_audio_mixdown::ActiveLoopMixdownSpan span;
            if (mini_daw_audio_mixdown::resolveActiveLoopMixdownSpan(transport.readCycleEnabledForUi(),
                                                                     snap->getLeftLocatorSamples(),
                                                                     snap->getRightLocatorSamples(), span)
                    .failed())
            {
                return false;
            }
            start = span.startSample;
            length = span.lengthSamples;
            sampleRate = device->getCurrentSampleRate();
            return true;
        };

        // --- Inspector panel scenario ---------------------------------------------------------------
        hooks.verifyInspectorPanelLayout = [this](juce::String& report, juce::String& failReason) -> bool {
            ChannelStripPanel& panel = inspectorPanel_.channelPanel();
            juce::Viewport& vp = inspectorPanel_.scrollViewport();
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const TrackId active = session.getActiveTrackId();
            const int idx = snap != nullptr ? snap->findTrackIndexById(active) : -1;
            const TrackKind kind = idx >= 0 ? snap->getTrack(idx).getKind() : TrackKind::Audio;
            const bool haveTrack = idx >= 0;
            const bool expectStrip = haveTrack && kind != TrackKind::Midi;           // audio / instrument / group / master
            const bool expectTrackTap = expectStrip && kind != TrackKind::Master;     // master reads the Stereo Out accumulator
            const TrackId expectedMetered = expectTrackTap ? active : kInvalidTrackId;
            report << "inspector column " << inspectorPanel_.getBounds().toString() << " viewport " << vp.getBounds().toString()
                   << " content " << inspectorView_.getBounds().toString() << " panel " << panel.getBounds().toString()
                   << (panel.isVisible() ? " (shown)" : " (hidden)")
                   << " panelMin=" << ChannelStripPanel::minimumHeight() << " panelPref=" << ChannelStripPanel::preferredHeight() << "\n";
            report << "active track " << juce::String((juce::int64)active) << " kind=" << (int)kind << " name=\"" << panel.getNameText()
                   << "\" fader=" << (panel.isFaderVisible() ? "shown" : "hidden") << " outputMeter="
                   << (panel.outputMeter().isVisible() ? (panel.isShowingMaster() ? "shown(StereoOut)" : "shown(track)") : "hidden")
                   << " metered(hub)=" << (expectTrackTap ? (playbackEngine_.isConcurrentlyMetered(active) ? "yes" : "NO") : "n/a")
                   << " faderText=\"" << panel.fader().getValueFieldText() << "\"\n";
            const juce::Rectangle<int> col = inspectorPanel_.getLocalBounds();
            if (!col.contains(vp.getBounds()))
            {
                failReason = "scroll viewport outside the Inspector column";
                return false;
            }
            if (panel.isVisible() != expectStrip)
            {
                failReason = juce::String("channel panel visibility wrong for this row kind (expected ") + (expectStrip ? "shown" : "hidden") + ")";
                return false;
            }
            if (!expectStrip)
            {
                if (vp.getHeight() != col.getHeight())
                {
                    failReason = "scroll area does not take the whole column when no channel panel is shown";
                    return false;
                }
                // With no audio row selected the Inspector reports no meter interest; only a
                // visible mixer may keep rows metered (the hub publishes the union).
                if (!levelMeterHub_.publishedInterest().empty() && (mixerWindow_ == nullptr || !mixerWindow_->isVisible()))
                {
                    failReason = "engine still meters a track although no audio row is selected";
                    return false;
                }
                return true;
            }
            if (!col.contains(panel.getBounds()))
            {
                failReason = "channel panel outside the Inspector column";
                return false;
            }
            if (panel.getBottom() != col.getBottom())
            {
                failReason = "channel panel is not pinned to the bottom of the Inspector column";
                return false;
            }
            if (panel.getHeight() < ChannelStripPanel::minimumHeight())
            {
                failReason = "channel panel below its minimum height (" + juce::String(panel.getHeight()) + ")";
                return false;
            }
            if (vp.getHeight() < InspectorPanel::kMinimumScrollAreaHeightPx
                && col.getHeight() >= InspectorPanel::kMinimumScrollAreaHeightPx + ChannelStripPanel::minimumHeight())
            {
                failReason = "scroll area below its minimum height (" + juce::String(vp.getHeight()) + ")";
                return false;
            }
            if (!panel.isFaderVisible() || !panel.outputMeter().isVisible())
            {
                failReason = "fader or output meter hidden on an audio-carrying row";
                return false;
            }
            if (panel.isShowingMaster() != (kind == TrackKind::Master))
            {
                failReason = "output meter source wrong (master vs track)";
                return false;
            }
            // The hub publishes the Inspector's row to the engine's concurrent meter bank (the
            // master row reads the Stereo Out window and needs no bank slot).
            if (expectedMetered != kInvalidTrackId && !playbackEngine_.isConcurrentlyMetered(expectedMetered))
            {
                failReason = "engine does not meter the selected track " + juce::String((juce::int64)expectedMetered) + " for the Inspector";
                return false;
            }
            if (expectedMetered == kInvalidTrackId && kind == TrackKind::Master && playbackEngine_.isConcurrentlyMetered(active))
            {
                failReason = "the master row must not take a track meter slot";
                return false;
            }
            // Children inside the panel, non-overlapping, and the fader text equals the session gain.
            const juce::Rectangle<int> pl = panel.getLocalBounds();
            for (const juce::Component* c : { static_cast<const juce::Component*>(&panel.fader()),
                                              static_cast<const juce::Component*>(&panel.outputMeter()) })
            {
                if (!pl.contains(c->getBounds()))
                {
                    failReason = "a channel-panel control lies outside the panel: " + c->getBounds().toString() + " vs " + pl.toString();
                    return false;
                }
            }
            if (panel.fader().getBounds().intersects(panel.outputMeter().getBounds()))
            {
                failReason = "fader and output meter overlap";
                return false;
            }
            if (expectStrip && haveTrack)
            {
                const juce::String want = channel_fader_scale::Scale::gainText(snap->getTrack(idx).getChannelFaderGain());
                if (panel.fader().getValueFieldText() != want)
                {
                    failReason = "fader value field shows \"" + panel.fader().getValueFieldText() + "\" but the session gain reads \"" + want + "\"";
                    return false;
                }
            }
            return true;
        };
        hooks.captureInspectorPng = [this](const juce::File& png) -> bool {
            const juce::Image img = inspectorPanel_.createComponentSnapshot(inspectorPanel_.getLocalBounds(), true, 1.0f);
            if (!img.isValid())
            {
                return false;
            }
            juce::FileOutputStream out(png);
            if (!out.openedOk())
            {
                return false;
            }
            juce::PNGImageFormat fmt;
            return fmt.writeImageToStream(img, out);
        };
        hooks.describeInspectorMeters = [this]() -> juce::String {
            ChannelStripPanel& panel = inspectorPanel_.channelPanel();
            const LevelMeterComponent& m = panel.outputMeter();
            return juce::String(panel.isShowingMaster() ? "outputMeter(StereoOut){" : "outputMeter(track){") + "visible="
                   + (m.isVisible() ? "yes" : "no") + " displayedDb=[" + juce::String(m.getDisplayedDb(0), 1) + "," + juce::String(m.getDisplayedDb(1), 1)
                   + "] heldPeak=\"" + m.getPeakText() + "\" latched=" + (m.isOverloadLatched() ? "YES" : "no") + " overs="
                   + juce::String((juce::int64)m.getOverloadSampleCount()) + " dc=" + (m.hasDcTag() ? "YES" : "no") + " ch=" + juce::String(m.getChannelCount()) + "}";
        };
        hooks.isInspectorOutputMeterOverloadLatched = [this]() -> bool {
            return inspectorPanel_.channelPanel().outputMeter().isOverloadLatched();
        };
        hooks.inspectorOutputMeterShowsSignal = [this]() -> bool {
            const LevelMeterComponent& m = inspectorPanel_.channelPanel().outputMeter();
            return m.isVisible() && (m.getDisplayedDb(0) > -50.0 || m.getDisplayedDb(1) > -50.0 || m.getHeldPeakLinear() > 0.003f);
        };
        hooks.resetInspectorOverloadLatches = [this] { inspectorPanel_.channelPanel().outputMeter().resetOverloadLatch(); };
        hooks.inspectorFaderTypeValue = [this](const juce::String& text) {
            inspectorPanel_.channelPanel().refreshFromSession();
            inspectorPanel_.channelPanel().fader().commitTypedValue(text);
        };
        hooks.inspectorFaderResetGesture = [this] {
            inspectorPanel_.channelPanel().refreshFromSession();
            inspectorPanel_.channelPanel().fader().resetToUnityGain();
        };
        hooks.inspectorFaderValueText = [this]() -> juce::String { return inspectorPanel_.channelPanel().fader().getValueFieldText(); };
        hooks.inspectorScrollToBottomAndVerify = [this](juce::String& detail) -> bool {
            juce::Viewport& vp = inspectorPanel_.scrollViewport();
            const int contentH = inspectorView_.getHeight();
            const int maxY = juce::jmax(0, contentH - vp.getViewHeight());
            vp.setViewPosition(0, maxY);
            const int visibleBottom = vp.getViewPositionY() + vp.getViewHeight();
            detail = "content height " + juce::String(contentH) + ", viewport height " + juce::String(vp.getViewHeight()) + ", scrolled to y="
                     + juce::String(vp.getViewPositionY()) + " -> visible bottom " + juce::String(visibleBottom) + " scrollbar="
                     + (vp.isVerticalScrollBarShown() ? "shown" : "hidden");
            return visibleBottom >= contentH;
        };
        hooks.getMainWindowBounds = [this]() -> juce::Rectangle<int> {
            if (auto* tlw = getTopLevelComponent())
            {
                return tlw->getBounds();
            }
            return getBounds();
        };
        hooks.setMainWindowSize = [this](const int w, const int h) {
            if (auto* tlw = getTopLevelComponent())
            {
                tlw->setSize(w, h);
            }
        };

        installMixerStabilityHooks(hooks);
        installPerfProfileStabilityHooks(hooks);
        installLoadProgressAndDuplicateTrackStabilityHooks(hooks);

        hooks.captureArrangementPng = [this](const juce::File& png) -> bool {
            const juce::Image img = createComponentSnapshot(getLocalBounds(), true, 1.0f);
            if (!img.isValid())
            {
                return false;
            }
            juce::FileOutputStream out(png);
            if (!out.openedOk())
            {
                return false;
            }
            juce::PNGImageFormat fmt;
            return fmt.writeImageToStream(img, out);
        };

        hooks.instrumentProcessedBlocks = [this](const TrackId tid) -> std::uint64_t {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                return 0;
            }
            if (ExperimentalInstrumentHost* const h = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid))
            {
                return h->readRtActivitySnapshotForDiagnostics().processOkBlocks;
            }
            return 0;
        };
        hooks.instrumentMaxMidiEventsInOneBlock = [this](const TrackId tid, const bool resetTo0) -> std::uint32_t {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                return 0;
            }
            if (ExperimentalInstrumentHost* const h = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid))
            {
                const std::uint32_t v = h->readRtActivitySnapshotForDiagnostics().maxMidiEventsInOneBlock;
                if (resetTo0)
                {
                    h->resetRtMaxBlockMidiEventsForDiagnostics();
                }
                return v;
            }
            return 0;
        };

        // ---- Live MIDI scenario hooks -------------------------------------------------------
        hooks.liveMidiFixtureSetup = [this](TrackId& inst, TrackId& lower, TrackId& pedal, juce::String& failReason) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr || liveMidiInputCoordinator_ == nullptr)
            {
                failReason = "no instrument runtime / live MIDI coordinator";
                return false;
            }
            const auto instIdOpt = session.appendExperimentalInstrumentShellTrack("LiveMidiDest");
            if (!instIdOpt.has_value())
            {
                failReason = "could not append instrument shell row";
                return false;
            }
            inst = *instIdOpt;
            const auto pr = instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(inst);
            if (pr.first == nullptr || pr.second == nullptr)
            {
                failReason = "could not create destination runtime";
                return false;
            }
            stabilityLiveMidiCaptureSink_.reset();
            pr.first->installMidiDeliveryCaptureSinkForTests(&stabilityLiveMidiCaptureSink_);
            if (!pr.second->bootstrapGrooveAgentShellForSessionTrack(inst))
            {
                failReason = "could not bootstrap destination shell";
                return false;
            }
            const auto addRoutedRow = [this, inst, &failReason](const char* label, const int outputChannel,
                                                                 const int inputFilter, TrackId& outTid) -> bool {
                const auto midiIdOpt = session.addMidiTrack();
                if (!midiIdOpt.has_value())
                {
                    failReason = juce::String(label) + ": could not add Midi track row";
                    return false;
                }
                outTid = *midiIdOpt;
                if (instrumentRuntimeCoordinator_->getOrCreateMidiContentControllerForTrack(outTid) == nullptr
                    || !session.setTrackMidiDestination(outTid, inst) || !session.setTrackMidiOutputChannel(outTid, outputChannel))
                {
                    failReason = juce::String(label) + ": routing setup refused";
                    return false;
                }
                if (InstrumentTrackController* const c = instrumentRuntimeCoordinator_->getMidiContentControllerForTrack(outTid))
                {
                    c->refreshMidiOutputChannelFromSession();
                }
                TrackMidiInputAssignment mi;
                mi.mode = TrackMidiInputMode::AllEnabled;
                mi.channelFilter = inputFilter;
                if (!session.setTrackMidiInputAssignment(outTid, mi))
                {
                    failReason = juce::String(label) + ": setTrackMidiInputAssignment refused";
                    return false;
                }
                return true;
            };
            // The instrument row's MIDI Input is deliberately left at None: the scenario sets it
            // through the Inspector exactly as the user does (the 1.1.10 defect lived there).
            if (!addRoutedRow("Lower", 2, 5, lower) || !addRoutedRow("Pedal", 3, 6, pedal))
            {
                return false;
            }
            syncViewportFromSession();
            trackLanesView.syncTracksFromSession();
            refreshInstrumentUi();
            inspectorView_.refreshFromSession();
            transport.requestSeek(0);
            // Let the coordinator pick the new session up now (not on its next tick) so the
            // routing is published before the scenario injects.
            liveMidiInputCoordinator_->refreshDevicesAndRouting();
            return true;
        };
        hooks.liveMidiDescribeDevices = [this]() -> juce::String {
            juce::String s = "inputs=[";
            for (const auto& d : juce::MidiInput::getAvailableDevices())
            {
                s << "\"" << d.name << "\" ";
            }
            s << "] outputs=[";
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
            {
                s << "\"" << d.name << "\" ";
            }
            s << "]";
            ensureStabilityLiveMidiLoopbackResolved();
            if (stabilityLiveMidiLoopbackOut_ != nullptr)
            {
                s << " loopback=\"" << stabilityLiveMidiLoopbackOut_->getName() << "\"";
            }
            return s;
        };
        hooks.liveMidiInjectUsesRealPort = [this] {
            ensureStabilityLiveMidiLoopbackResolved();
            return stabilityLiveMidiLoopbackOut_ != nullptr;
        };
        hooks.liveMidiInject = [this](const juce::MidiMessage& m) {
            ensureStabilityLiveMidiLoopbackResolved();
            if (stabilityLiveMidiLoopbackOut_ != nullptr)
            {
                stabilityLiveMidiLoopbackOut_->sendMessageNow(m);
                return;
            }
            // No loopback port on this machine: enter at the device-callback boundary from a
            // dedicated thread (like a MIDI driver thread would), with the device-style timestamp.
            if (liveMidiInputCoordinator_ == nullptr)
            {
                return;
            }
            live_midi::LiveMidiInputBus* const bus = &liveMidiInputCoordinator_->busForDiagnostics();
            juce::MidiMessage copy(m);
            std::thread([bus, copy]() mutable {
                copy.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
                bus->deviceThread_push(0, copy);
            }).join();
        };
        hooks.liveMidiSetMonitor = [this](const TrackId tid, const bool on) {
            if (liveMidiInputCoordinator_ != nullptr)
            {
                liveMidiInputCoordinator_->setMonitorEnabled(tid, on);
            }
        };
        hooks.liveMidiSetArm = [this](const TrackId tid, const bool on) {
            if (liveMidiInputCoordinator_ != nullptr)
            {
                liveMidiInputCoordinator_->setRecordArmed(tid, on);
            }
        };
        hooks.liveMidiCapturedNoteCount = [this](const int channel, const int note, const bool noteOn) {
            return stabilityLiveMidiCaptureSink_.countNotes(channel, note, noteOn);
        };
        hooks.liveMidiCapturedCcCount = [this](const int channel, const int controller) {
            return stabilityLiveMidiCaptureSink_.countCc(channel, controller);
        };
        hooks.liveMidiCapturedPitchBendCount = [this](const int channel) {
            return stabilityLiveMidiCaptureSink_.countPitchBend(channel);
        };
        hooks.liveMidiCaptureReset = [this] { stabilityLiveMidiCaptureSink_.reset(); };
        hooks.liveMidiAttachCaptureSink = [this](const TrackId tid, juce::String& failReason) -> bool {
            ExperimentalInstrumentHost* const h
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid) : nullptr;
            if (h == nullptr)
            {
                failReason = "no destination host for track " + juce::String((juce::int64)tid) + " after reload";
                return false;
            }
            stabilityLiveMidiCaptureSink_.reset();
            h->installMidiDeliveryCaptureSinkForTests(&stabilityLiveMidiCaptureSink_);
            return true;
        };
        hooks.recordToggleLikeKey = [this] {
            if (recordingCoordinator_ != nullptr)
            {
                recordingCoordinator_->numpadRecordToggled();
            }
        };
        hooks.isCountInActive = [this] { return recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive(); };
        hooks.isRecordingInProgress = [this] { return anyRecordingInProgress(); };
        hooks.getTransportPlayheadSamples = [this] { return transport.readPlayheadSamplesForUi(); };
        hooks.liveMidiTakeStartSample = [this]() -> std::int64_t {
            return liveMidiInputCoordinator_ != nullptr ? liveMidiInputCoordinator_->takeStartSample() : 0;
        };
        hooks.reportedOutputLatencySamples = [this] { return latencyStore_.getReportedOutputLatencySamples(); };
        hooks.liveMidiSummarizeClips = [this](const TrackId tid) -> StabilityMidiClipSummary {
            StabilityMidiClipSummary s;
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (c == nullptr)
            {
                return s;
            }
            for (const auto& up : c->getClips())
            {
                if (up == nullptr)
                {
                    continue;
                }
                if (s.clipCount == 0)
                {
                    s.firstClipStartSamples = up->startSamples;
                    s.firstClipLengthSamples = up->lengthSamples;
                    s.bpm = up->pattern.bpm;
                    s.ticksPerQuarter = up->pattern.ticksPerQuarter;
                    for (const auto& n : up->pattern.timelineNotes)
                    {
                        s.notes.push_back({ n.midiNote, n.velocity, n.offVelocity, (int)n.channel, n.startTick, n.durationTicks });
                    }
                    for (const auto& p : up->pattern.ccPoints)
                    {
                        s.cc.push_back({ p.startTick, (int)p.controller, (int)p.value, (int)p.channel });
                    }
                    for (const auto& b : up->pattern.pitchBendPoints)
                    {
                        s.pitchBend.push_back({ b.startTick, b.value, (int)b.channel });
                    }
                }
                ++s.clipCount;
            }
            return s;
        };
        hooks.liveMidiExportFirstClip = [this](const TrackId tid, const juce::File& out, int& notes, int& cc, int& pb,
                                               juce::String& failReason) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (c == nullptr || c->getClips().empty() || c->getClips().front() == nullptr)
            {
                failReason = "no clip to export";
                return false;
            }
            int outputChannel = kTrackMidiOutputChannelAny;
            if (const auto snap = session.loadSessionSnapshotForAudioThread())
            {
                const int ix = snap->findTrackIndexById(tid);
                if (ix >= 0)
                {
                    outputChannel = snap->getTrack(ix).getMidiOutputChannel();
                }
            }
            const InstrumentMidiClipExportResult r
                = exportInstrumentMidiClipToMidiFile(*c->getClips().front(), outputChannel, out, 48000.0);
            if (!r.ok)
            {
                failReason = "export failed: " + r.errorMessage;
                return false;
            }
            notes = r.notesExported;
            cc = r.ccEventsExported;
            pb = r.pitchBendEventsExported;
            return true;
        };
        hooks.undoStackSize = [this] { return undoRedoCoordinator_ != nullptr ? (int)undoRedoCoordinator_->undoStackSizeForDiagnostics() : 0; };
        hooks.setCycleEnabled = [this](const bool on) { transport.requestCycleEnabled(on); };
        hooks.isCycleEnabled = [this] { return transport.readCycleEnabledForUi(); };
        hooks.inspectorMidiInputTexts = [this]() -> juce::String {
            inspectorView_.refreshFromSession();
            return "visible=" + juce::String(inspectorView_.isMidiInputComboVisibleForStabilityTest() ? "yes" : "no")
                   + " input=\"" + inspectorView_.getMidiInputComboTextForStabilityTest() + "\" channel="
                   + inspectorView_.getMidiInputChannelComboTextForStabilityTest() + " status=\""
                   + inspectorView_.getMidiInputStatusTextForStabilityTest().replace("\n", " | ") + "\"";
        };
        hooks.selectTrackLikeHeaderClick = hooks.activateTrackLikeHeaderClick;
        hooks.clickHeaderCellLikeMouse = [this](const TrackId tid, const juce::String& cell) -> bool {
            TrackHeaderView* const header = trackLanesView.findInstrumentRowHeaderForStabilityTest(tid);
            if (header == nullptr)
            {
                return false;
            }
            if (cell.equalsIgnoreCase("monitor"))
            {
                return header->clickMonitorCellLikeMouseForStabilityTest();
            }
            if (cell.equalsIgnoreCase("arm"))
            {
                return header->clickArmCellLikeMouseForStabilityTest();
            }
            if (cell.equalsIgnoreCase("mute"))
            {
                return header->clickMuteCellLikeMouseForStabilityTest();
            }
            return header->clickPowerCellLikeMouseForStabilityTest();
        };
        hooks.inspectorChooseMidiInput = [this](const juce::String& text) {
            return inspectorView_.chooseMidiInputByTextForStabilityTest(text);
        };
        hooks.inspectorChooseMidiInputChannel = [this](const int ch) {
            return inspectorView_.chooseMidiInputChannelForStabilityTest(ch);
        };
        hooks.getActiveTrackId = [this] { return session.getActiveTrackId(); };
        hooks.describeTrackMidiInputFromSession = [this](const TrackId tid) -> juce::String {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int ix = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            if (ix < 0)
            {
                return "missing";
            }
            const TrackMidiInputAssignment& mi = snap->getTrack(ix).getMidiInputAssignment();
            juce::String s = mi.mode == TrackMidiInputMode::None ? "none"
                             : mi.mode == TrackMidiInputMode::AllEnabled ? "all"
                                                                          : "device:" + mi.deviceName;
            s << " ch=" << (mi.channelFilter == kTrackMidiInputChannelAll ? juce::String("all") : juce::String(mi.channelFilter));
            return s;
        };
        hooks.describePublishedRouteForTrack = [this](const TrackId tid) -> juce::String {
            if (liveMidiInputCoordinator_ == nullptr)
            {
                return "no coordinator";
            }
            const auto routing = liveMidiInputCoordinator_->busForDiagnostics().currentRouting();
            if (routing == nullptr)
            {
                return "absent (no routing published)";
            }
            for (const auto& r : routing->routes)
            {
                if (r.trackId == tid)
                {
                    return "routed slot=" + juce::String(r.deviceSlot) + " filter=" + juce::String(r.channelFilter) + " monitor="
                           + (r.monitor ? "yes" : "no") + " capture=" + (r.capture ? "yes" : "no");
                }
            }
            return "absent";
        };
        hooks.lastRecordStartRefusal = [this] {
            return recordingCoordinator_ != nullptr ? recordingCoordinator_->getLastRecordStartRefusalForDiagnostics() : juce::String();
        };
        hooks.firstLoadedInstrumentRow = [this]() -> TrackId {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr || instrumentRuntimeCoordinator_ == nullptr)
            {
                return kInvalidTrackId;
            }
            // Prefer a melodic instrument that sounds for any note (VB3-II organ); a drum kit may
            // simply have no sample on the probe note.
            TrackId first = kInvalidTrackId;
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& t = snap->getTrack(i);
                if (t.getKind() != TrackKind::Instrument)
                {
                    continue;
                }
                ExperimentalInstrumentHost* const h = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(t.getId());
                if (h == nullptr || !h->hasInstrument())
                {
                    continue;
                }
                if (t.getName().containsIgnoreCase("VB3"))
                {
                    return t.getId();
                }
                if (first == kInvalidTrackId)
                {
                    first = t.getId();
                }
            }
            return first;
        };
        hooks.liveMidiFirstRealInputDevice = [](juce::String& identifier, juce::String& name) -> bool {
            const auto devices = juce::MidiInput::getAvailableDevices();
            if (devices.isEmpty())
            {
                return false;
            }
            identifier = devices[0].identifier;
            name = devices[0].name;
            return true;
        };
        hooks.liveMidiSetTrackInputDevice = [this](const TrackId tid, const juce::String& identifier, const juce::String& name) {
            TrackMidiInputAssignment mi;
            if (identifier.isEmpty())
            {
                mi.mode = TrackMidiInputMode::AllEnabled;
            }
            else
            {
                mi.mode = TrackMidiInputMode::Device;
                mi.deviceIdentifier = identifier;
                mi.deviceName = name;
            }
            if (const auto snap = session.loadSessionSnapshotForAudioThread())
            {
                const int ix = snap->findTrackIndexById(tid);
                if (ix >= 0)
                {
                    mi.channelFilter = snap->getTrack(ix).getMidiInputAssignment().channelFilter;
                }
            }
            const bool ok = session.setTrackMidiInputAssignment(tid, mi);
            if (liveMidiInputCoordinator_ != nullptr)
            {
                liveMidiInputCoordinator_->refreshDevicesAndRouting();
            }
            inspectorView_.refreshFromSession();
            return ok;
        };
        hooks.liveMidiIsDeviceOpen = [this](const juce::String& identifier, juce::String& detail) -> bool {
            const bool enabled = deviceManager.isMidiInputDeviceEnabled(identifier);
            const int slot = liveMidiInputCoordinator_ != nullptr
                                 ? liveMidiInputCoordinator_->slotForDeviceIdentifierForDiagnostics(identifier)
                                 : -1;
            detail = "managerEnabled=" + juce::String(enabled ? "yes" : "no") + " slot=" + juce::String(slot);
            return enabled && slot >= 0;
        };
        hooks.armAudioTrackForRecording = [this](const TrackId tid) {
            if (tid == kInvalidTrackId)
            {
                recorder_.disarm();
            }
            else
            {
                recorder_.armForRecording(tid);
            }
            trackLanesView.repaint();
        };
        hooks.audioClipCountForTrack = [this](const TrackId tid) -> int {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int ix = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            return ix >= 0 ? snap->getTrack(ix).getNumPlacedClips() : 0;
        };
        // --- MIDI cycle takes / layering scenario ---------------------------------------------
        hooks.setLocatorsSamples = [this](const std::int64_t l, const std::int64_t r) {
            // Locators clamp to the navigable extent (a tiny fixture project may end before `r`):
            // grow it first, the same grow-only session operation the default seeding uses.
            if (session.getArrangementExtentSamples() < r)
            {
                session.setArrangementExtentSamples(r);
                syncViewportFromSession();
            }
            session.setLeftLocatorAtSample(l);
            session.setRightLocatorAtSample(r);
            rulerView.repaint();
            trackLanesView.repaint();
        };
        hooks.getDeviceSampleRate = [this]() -> double {
            if (juce::AudioIODevice* dev = deviceManager.getCurrentAudioDevice())
            {
                return dev->getCurrentSampleRate();
            }
            return 0.0;
        };
        hooks.getCycleWrapCount = [this]() -> std::uint32_t { return transport.readCycleWrapCountForUi(); };
        hooks.liveMidiSummarizeAllClips = [this](const TrackId tid) -> std::vector<StabilityMidiClipSummary> {
            std::vector<StabilityMidiClipSummary> out;
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (c == nullptr)
            {
                return out;
            }
            for (const auto& up : c->getClips())
            {
                if (up == nullptr)
                {
                    continue;
                }
                StabilityMidiClipSummary s;
                s.clipCount = 1;
                s.firstClipStartSamples = up->startSamples;
                s.firstClipLengthSamples = up->lengthSamples;
                s.bpm = up->pattern.bpm;
                s.ticksPerQuarter = up->pattern.ticksPerQuarter;
                for (const auto& n : up->pattern.timelineNotes)
                {
                    s.notes.push_back({ n.midiNote, n.velocity, n.offVelocity, (int)n.channel, n.startTick, n.durationTicks });
                }
                for (const auto& p : up->pattern.ccPoints)
                {
                    s.cc.push_back({ p.startTick, (int)p.controller, (int)p.value, (int)p.channel });
                }
                for (const auto& b : up->pattern.pitchBendPoints)
                {
                    s.pitchBend.push_back({ b.startTick, b.value, (int)b.channel });
                }
                out.push_back(std::move(s));
            }
            return out;
        };
        hooks.deleteTopmostMidiClipLikeUi = [this](const TrackId tid) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (c == nullptr || c->getClips().empty() || c->getClips().back() == nullptr)
            {
                return false;
            }
            const int before = (int)c->getClips().size();
            if (arrangementEventSelectionCoordinator_ != nullptr)
            {
                arrangementEventSelectionCoordinator_->clearAllArrangementEventSelections();
            }
            session.setActiveTrack(tid);
            c->setSelectedClipIdsExclusive(c->getClips().back()->id); // the topmost layer
            invokeDeleteSelectedPlacedClipFromWindowShortcut();        // the Delete-key path
            return (int)c->getClips().size() == before - 1;
        };
        hooks.moveTopmostMidiClipLikeUi = [this](const TrackId tid, const std::int64_t deltaSamples) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (c == nullptr || c->getClips().empty() || c->getClips().back() == nullptr || undoRedoCoordinator_ == nullptr)
            {
                return false;
            }
            c->setSelectedClipIdsExclusive(c->getClips().back()->id);
            bool moved = false;
            undoRedoCoordinator_->executeUndoableInstrumentEdit("Move MIDI clip", [c, deltaSamples, &moved] {
                moved = c->moveSelectedInstrumentMidiClipsByDeltaSamples(deltaSamples);
                return moved;
            });
            return moved;
        };
        hooks.proxySequencerNoteOnsForDestination = [this](const TrackId destination) -> juce::String {
            const auto sessionSnap = session.loadSessionSnapshotForAudioThread();
            if (sessionSnap == nullptr || instrumentRuntimeCoordinator_ == nullptr)
            {
                return {};
            }
            double sr = 48000.0;
            if (juce::AudioIODevice* dev = deviceManager.getCurrentAudioDevice())
            {
                sr = dev->getCurrentSampleRate() > 0.0 ? dev->getCurrentSampleRate() : sr;
            }
            proxy_snapshot::BuildInputs in;
            in.renderConfig.renderSampleRate = sr;
            in.renderConfig.timelineReferenceRate = sr;
            const auto clipsFor = [this](const TrackId tid) {
                std::vector<const InstrumentMidiClip*> v;
                if (InstrumentTrackController* const c = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid))
                {
                    for (const auto& up : c->getClips())
                    {
                        v.push_back(up.get());
                    }
                }
                return v;
            };
            const proxy_snapshot::ProxyRenderSnapshot snap
                = proxy_snapshot::buildProxyRenderSnapshot(*sessionSnap, destination, clipsFor, in);
            proxy_render::ProxyOfflineSequencer seq(snap, sr);
            const std::int64_t total = juce::jmax<std::int64_t>(sessionSnap->getArrangementExtentSamples(), seq.lastEventRenderSample() + 1);
            juce::String out;
            juce::MidiBuffer midi;
            for (std::int64_t pos = 0; pos < total; pos += 512)
            {
                midi.clear();
                seq.emitBlock(pos, 512, midi);
                for (const auto meta : midi)
                {
                    const juce::MidiMessage m = meta.getMessage();
                    if (m.isNoteOn())
                    {
                        out << m.getChannel() << ":" << m.getNoteNumber() << "@" << juce::String((juce::int64)(pos + meta.samplePosition)) << " ";
                    }
                }
            }
            return out.trim();
        };
        hooks.proxySnapshotSourcesText = [this](const TrackId destination) -> juce::String {
            const auto sessionSnap = session.loadSessionSnapshotForAudioThread();
            if (sessionSnap == nullptr || instrumentRuntimeCoordinator_ == nullptr)
            {
                return {};
            }
            proxy_snapshot::BuildInputs in;
            in.renderConfig.renderSampleRate = 48000.0;
            in.renderConfig.timelineReferenceRate = session.timelineSampleRateOr(48000.0);
            const auto clipsFor = [this](const TrackId tid) {
                std::vector<const InstrumentMidiClip*> v;
                InstrumentTrackController* c = instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
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
                            v.push_back(up.get());
                        }
                    }
                }
                return v;
            };
            const proxy_snapshot::ProxyRenderSnapshot snap
                = proxy_snapshot::buildProxyRenderSnapshot(*sessionSnap, destination, clipsFor, in);
            juce::String out;
            const auto describe = [&out](const juce::String& label, const std::vector<proxy_snapshot::SnapshotClip>& clips) {
                int notes = 0;
                for (const auto& c : clips)
                {
                    notes += (int)c.notes.size();
                }
                out << label << " clips=" << (int)clips.size() << " notes=" << notes;
            };
            describe("own", snap.destinationClips);
            for (const auto& s : snap.sources)
            {
                out << " | src " << juce::String((juce::int64)s.trackId) << " ch=" << s.midiOutputChannel << (s.trackOff ? " off" : "") << (s.muted ? " muted" : "") << " ";
                describe("", s.clips);
            }
            return out;
        };
        hooks.proxyPublishedAssetShape = [this](const TrackId tid, std::int64_t& lengthSamples, double& sampleRate) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid) : nullptr;
            const ProjectFileProxyMetadataV20* const meta = c != nullptr ? c->getProxyMetadata() : nullptr;
            if (meta == nullptr || meta->silentGeneration)
            {
                return false;
            }
            lengthSamples = meta->lengthSamples;
            sampleRate = meta->sampleRate;
            return lengthSamples > 0 && sampleRate > 0.0;
        };
        hooks.proxyTrackNoteSpanSeconds = [this](const TrackId tid, double& firstNoteOnSec, double& lastNoteOffSec) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid) : nullptr;
            if (c == nullptr)
            {
                return false;
            }
            const double refRate = session.timelineSampleRateOr(48000.0);
            std::int64_t first = std::numeric_limits<std::int64_t>::max(), last = -1;
            for (const auto& clip : c->getClips())
            {
                if (clip == nullptr)
                {
                    continue;
                }
                const double bpm = clip->pattern.bpm > 0.0 ? clip->pattern.bpm : 110.0;
                const int tpq = clip->pattern.ticksPerQuarter > 0 ? clip->pattern.ticksPerQuarter : 960;
                for (const auto& n : clip->pattern.timelineNotes)
                {
                    const std::int64_t on = clip->timelineAnchorSamples + ticksToRelativeSamples(n.startTick, bpm, tpq, refRate);
                    const std::int64_t off = clip->timelineAnchorSamples
                                             + ticksToRelativeSamples(n.startTick + juce::jmax<std::int64_t>(1, n.durationTicks), bpm, tpq, refRate);
                    first = juce::jmin(first, on);
                    last = juce::jmax(last, off);
                }
            }
            if (last < 0)
            {
                return false;
            }
            firstNoteOnSec = (double)first / refRate;
            lastNoteOffSec = (double)last / refRate;
            return true;
        };
        hooks.proxyDeleteUnpublishedGenerationFiles = [this](const TrackId tid) -> int {
            const juce::File pf = session.getCurrentProjectFile();
            if (pf == juce::File() || proxyRenderEngine_ == nullptr)
            {
                return 0;
            }
            const juce::String published = proxyRenderEngine_->publishedGenerationId(tid);
            const juce::String keep = published.isNotEmpty() ? proxy_store::generationFileName(tid, published) : juce::String();
            const juce::String prefix = "track_" + juce::String((juce::int64)tid) + "_";
            int removed = 0;
            for (const auto& f : proxy_store::proxyDirectory(pf.getParentDirectory()).findChildFiles(juce::File::findFiles, false, "*.wav"))
            {
                if (f.getFileName().startsWith(prefix) && f.getFileName() != keep && f.deleteFile())
                {
                    ++removed;
                }
            }
            return removed;
        };
        hooks.proxySetUpdateModeManual = [this](const TrackId tid) -> bool {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid) : nullptr;
            if (c == nullptr)
            {
                return false;
            }
            const juce::String manual = proxy_policy::proxyUpdateModePersistedString(proxy_policy::ProxyUpdateMode::Manual);
            if (c->getProxyUpdateMode() != manual && !c->setProxyUpdateModeFromUi(manual))
            {
                return false;
            }
            if (proxyUpdatePolicyService_ != nullptr)
            {
                proxyUpdatePolicyService_->tick();
            }
            return true;
        };
        // ---- Proxy render probe (diagnostics) -------------------------------------------------
        hooks.proxyRenderProbeCapture = [this](const TrackId tid, const juce::File& outDir,
                                               const StabilityRunnerHooks::ProxyRenderProbeOptions& options) -> juce::String {
            const juce::File& blobOverride = options.stateBlobOverride;
            const bool readinessEnabled = options.readinessEnabled;
            const bool nonRealtimeIndication = options.nonRealtimeIndication;
            auto& P = proxyRenderProbe_;
            if (proxyRenderEngine_ == nullptr)
            {
                return "ERROR: no proxy render engine";
            }
            juce::String err;
            P.captured = proxyRenderEngine_->captureRequest(tid, err);
            if (P.captured == nullptr)
            {
                return "ERROR: capture failed: " + err;
            }
            juce::String overrideNote;
            if (!readinessEnabled)
            {
                static_cast<proxy_render::AppProxyRenderEngine::CapturedAppRequest&>(*P.captured).request.readiness.enabled = false;
                overrideNote << "identity: readiness verification DISABLED for this probe (pre-1.1.14 behaviour)\n";
            }
            if (!nonRealtimeIndication)
            {
                static_cast<proxy_render::AppProxyRenderEngine::CapturedAppRequest&>(*P.captured).request.nonRealtimeIndication = false;
                overrideNote << "identity: clone prepared WITHOUT the offline (non-realtime) indication (diagnostic)\n";
            }
            if (options.tailPolicyV1)
            {
                static_cast<proxy_render::AppProxyRenderEngine::CapturedAppRequest&>(*P.captured).request.diagnosticAbsolutePeakTail = true;
                overrideNote << "identity: tail judged on the RAW peak (tail policy v1 behaviour, diagnostic)\n";
            }
            if (options.retainFailedArtifact)
            {
                static_cast<proxy_render::AppProxyRenderEngine::CapturedAppRequest&>(*P.captured).request.retainFailedArtifactForDiagnostics = true;
                overrideNote << "identity: a failed render's artifact is retained for analysis (diagnostic)\n";
            }
            if (blobOverride != juce::File())
            {
                // Frozen input for a cross-build comparison: the EXACT bytes a previous run dumped.
                juce::MemoryBlock frozen;
                if (!blobOverride.existsAsFile() || !blobOverride.loadFileAsData(frozen) || frozen.getSize() == 0)
                {
                    return "ERROR: state blob override unreadable: " + blobOverride.getFullPathName();
                }
                static_cast<proxy_render::AppProxyRenderEngine::CapturedAppRequest&>(*P.captured).request.snapshot.pluginStateBlob = frozen;
                overrideNote = "identity: plugin-state blob OVERRIDDEN from " + blobOverride.getFullPathName() + "\n";
            }
            P.destination = tid;
            P.outDir = outDir;
#if JUCE_DEBUG
            P.build = "Debug";
#else
            P.build = "Release";
#endif
            P.renderIndex = 0;
            P.hasResult = false;
            const auto& c = static_cast<const proxy_render::AppProxyRenderEngine::CapturedAppRequest&>(*P.captured);
            const auto& req = c.request;
            const auto& snap = req.snapshot;
            const juce::MemoryBlock canonical = proxy_fingerprint::serializeCanonicalFingerprintBytes(snap);
            const juce::String blobHash = snap.pluginStateBlob.getSize() > 0
                                              ? juce::SHA256(snap.pluginStateBlob.getData(), snap.pluginStateBlob.getSize()).toHexString()
                                              : juce::String("(empty)");
            const juce::String canonicalHash = juce::SHA256(canonical.getData(), canonical.getSize()).toHexString();
            proxy_render::ProxyOfflineSequencer seq(snap, req.renderSampleRate);
            juce::String initialState;
            {
                juce::MidiBuffer init;
                seq.emitInitialControllerState(init);
                for (const auto meta : init)
                {
                    const juce::MidiMessage m = meta.getMessage();
                    if (m.isController())
                    {
                        initialState << " ch" << m.getChannel() << "cc" << m.getControllerNumber() << "=" << m.getControllerValue();
                    }
                    else if (m.isPitchWheel())
                    {
                        initialState << " ch" << m.getChannel() << "pb=" << m.getPitchWheelValue();
                    }
                }
            }
            int destNotes = 0, srcNotes = 0;
            for (const auto& cl : snap.destinationClips) { destNotes += (int)cl.notes.size(); }
            for (const auto& s : snap.sources) { for (const auto& cl : s.clips) { srcNotes += (int)cl.notes.size(); } }
            juce::String t;
            t << "identity: build=" << P.build << " app=" << juce::String(ProjectInfo::versionString) << " destination=" << juce::String((juce::int64)tid) << "\n"
              << "identity: fingerprint=" << req.expectedFingerprint << "\n"
              << "identity: canonical-bytes sha256=" << canonicalHash << " (" << juce::String((juce::int64)canonical.getSize()) << " bytes)\n"
              << "identity: plugin-state blob sha256=" << blobHash << " (" << juce::String((juce::int64)snap.pluginStateBlob.getSize()) << " bytes)"
              << " stateRevision=" << juce::String((juce::int64)snap.stateIdentity.primaryStateRevision) << "\n"
              << "identity: plugin name=\"" << req.pluginDescription.name << "\" version=\"" << req.pluginDescription.version << "\" manufacturer=\""
              << req.pluginDescription.manufacturerName << "\" format=" << req.pluginDescription.pluginFormatName << " uid=" << juce::String(req.pluginDescription.uniqueId)
              << " file=" << req.pluginDescription.fileOrIdentifier << "\n"
              << "identity: renderSampleRate=" << juce::String(req.renderSampleRate) << " renderBlockSize=" << juce::String(req.renderBlockSize)
              << " timelineReferenceRate=" << juce::String(snap.renderConfig.timelineReferenceRate) << " noteOffGateMs=" << juce::String(snap.renderConfig.noteOffGateMs) << "\n"
              << "identity: tail policy v" << juce::String(proxy_render::kTailPolicyVersion) << " threshold=" << juce::String(proxy_render::kTailThresholdDb) << " dBFS window="
              << juce::String(proxy_render::kTailSilenceWindowSec) << " s cap=" << juce::String(proxy_render::kTailMaxSec) << " s | policies latency=v"
              << juce::String(snap.policies.latencyPolicyVersion) << " tail=v" << juce::String(snap.policies.tailPolicyVersion) << " render=v"
              << juce::String(snap.policies.renderPolicyVersion) << " format=v" << juce::String(snap.policies.proxyFormatVersion) << "\n"
              << "identity: content destinationClips=" << juce::String((int)snap.destinationClips.size()) << " notes=" << juce::String(destNotes) << " sources="
              << juce::String((int)snap.sources.size()) << " sourceNotes=" << juce::String(srcNotes) << " lastRelevantEvent(ref)="
              << juce::String((juce::int64)snap.spanAndSilence.lastRelevantEventReferenceSample) << " spanEnd(render)=" << juce::String((juce::int64)seq.lastEventRenderSample())
              << " (" << juce::String((double)seq.lastEventRenderSample() / req.renderSampleRate, 3) << " s)\n"
              << "identity: readiness initial controller state:" << initialState << "\n"
              << "identity: temp artifact=" << req.temporaryWavFile.getFullPathName() << "\n"
              << overrideNote;
            (void)outDir.createDirectory();
            (void)outDir.getChildFile("identity-" + P.build.toLowerCase() + ".txt").replaceWithText(t);
            // Frozen artifacts: the exact state bytes and the canonical snapshot bytes rendered.
            (void)outDir.getChildFile("state-blob-" + P.build.toLowerCase() + ".bin").replaceWithData(snap.pluginStateBlob.getData(), snap.pluginStateBlob.getSize());
            (void)outDir.getChildFile("snapshot-canonical-" + P.build.toLowerCase() + ".bin").replaceWithData(canonical.getData(), canonical.getSize());
            return t;
        };
        hooks.proxyRenderProbePrepare = [this](juce::String& failReason) -> bool {
            auto& P = proxyRenderProbe_;
            if (P.captured == nullptr || proxyRenderEngine_ == nullptr)
            {
                failReason = "nothing captured";
                return false;
            }
            if (P.worker != nullptr || P.prepared != nullptr)
            {
                failReason = "a probe render is still in flight";
                return false;
            }
            proxy_render::ProxyRenderResult fail;
            P.prepareStartMs = juce::Time::getMillisecondCounterHiRes();
            P.prepared = proxyRenderEngine_->prepare(*P.captured, fail);
            P.preparedMs = juce::Time::getMillisecondCounterHiRes();
            if (P.prepared == nullptr)
            {
                failReason = "prepare failed: " + fail.message;
                return false;
            }
            ++P.renderIndex;
            return true;
        };
        hooks.proxyRenderProbeInstanceStateHash = [this]() -> juce::String {
            auto& P = proxyRenderProbe_;
            if (P.prepared == nullptr)
            {
                return "(no prepared instance)";
            }
            auto& job = static_cast<proxy_render::AppProxyRenderEngine::PreparedAppJob&>(*P.prepared);
            if (job.instance_ == nullptr)
            {
                return "(no instance)";
            }
            juce::MemoryBlock mb;
            job.instance_->getStateInformation(mb);
            return "sha256=" + (mb.getSize() > 0 ? juce::SHA256(mb.getData(), mb.getSize()).toHexString() : juce::String("(empty)"))
                   + " (" + juce::String((juce::int64)mb.getSize()) + " bytes) latency=" + juce::String(job.instance_->getLatencySamples())
                   + " +" + juce::String(juce::Time::getMillisecondCounterHiRes() - P.preparedMs, 0) + " ms after prepare";
        };
        hooks.proxyRenderProbeStartWorker = [this](juce::String& failReason) -> bool {
            auto& P = proxyRenderProbe_;
            if (P.prepared == nullptr)
            {
                failReason = "no prepared instance";
                return false;
            }
            P.progressMs.store(0);
            P.worker = std::make_unique<ProxyRenderProbeState::Worker>(*P.prepared, P.progressMs);
            P.worker->startThread(juce::Thread::Priority::low);
            return true;
        };
        hooks.proxyRenderProbeIsDone = [this]() -> bool {
            auto& P = proxyRenderProbe_;
            return P.worker != nullptr && P.worker->done.load(std::memory_order_acquire);
        };
        hooks.proxyRenderProbeProgressMs = [this]() -> std::int64_t { return proxyRenderProbe_.progressMs.load(std::memory_order_relaxed); };
        hooks.proxyRenderProbeFinish = [this](const juce::String& label) -> juce::String {
            auto& P = proxyRenderProbe_;
            if (P.worker == nullptr)
            {
                return "ERROR: no worker";
            }
            P.worker->stopThread(30000);
            const proxy_render::ProxyRenderResult r = P.worker->result;
            const double workerStartMs = P.worker->startedMs, workerEndMs = P.worker->endedMs;
            P.worker.reset();
            P.prepared.reset(); // isolated instance teardown (message thread)
            P.lastResult = r;
            P.hasResult = true;
            juce::String t;
            t << label << ": status=" << proxy_render::toString(r.status) << " reason=" << proxy_render::toString(r.failureReason)
              << (r.message.isNotEmpty() ? " msg=\"" + r.message + "\"" : juce::String()) << "\n"
              << label << ": spanEnd=" << juce::String(r.spanEndRenderSamples) << " rendered=" << juce::String(r.renderedLengthSamples)
              << " (" << juce::String((double)r.renderedLengthSamples / juce::jmax(1.0, r.renderSampleRate), 3) << " s) tail=" << juce::String(r.tailLengthSamples)
              << " (" << juce::String((double)r.tailLengthSamples / juce::jmax(1.0, r.renderSampleRate), 3) << " s) tailCompleted=" << (r.tailCompleted ? "yes" : "no")
              << " rawPeak=" << juce::String(proxy_probe::linearToDb(r.maxPeakLinear), 1) << " dBFS residualPeak="
              << juce::String(proxy_probe::linearToDb(r.maxResidualPeakLinear), 1) << " dBFS dcAtEnd L/R=" << juce::String(r.dcEstimateAtEnd[0], 4)
              << "/" << juce::String(r.dcEstimateAtEnd[1], 4) << " first L/R=" << juce::String(r.firstSample[0], 4) << "/" << juce::String(r.firstSample[1], 4)
              << " last L/R=" << juce::String(r.lastSample[0], 4) << "/" << juce::String(r.lastSample[1], 4)
              << " blocks=" << juce::String((juce::int64)r.blocksProcessed)
              << " blocksWithMidi=" << juce::String((juce::int64)r.blocksWithMidi) << " midiEvents=" << juce::String((juce::int64)r.midi.totalEvents)
              << " latency=" << juce::String(r.pluginLatencySamplesAtStart) << "/" << juce::String(r.pluginLatencySamplesAtEnd) << "\n"
              << label << ": timing prepare=" << juce::String(P.preparedMs - P.prepareStartMs, 0) << " ms, prepare->firstBlock="
              << juce::String(workerStartMs - P.preparedMs, 0) << " ms, render wall=" << juce::String(r.wallMs, 0) << " ms ("
              << juce::String(r.wallMs > 0.0 ? (double)r.renderedLengthSamples / juce::jmax(1.0, r.renderSampleRate) * 1000.0 / r.wallMs : 0.0, 1)
              << "x realtime), worker total=" << juce::String(workerEndMs - workerStartMs, 0) << " ms\n"
              << label << ": readiness " << (r.readiness.attempted ? (r.readiness.verified ? "VERIFIED" : "unverified") : "not attempted")
              << " passes=" << r.readiness.passes << " stimulus=" << r.readiness.stimulusNotes << " sounding=" << r.readiness.soundingNotes
              << " wait=" << juce::String(r.readiness.waitMs, 0) << " ms blocks=" << juce::String((juce::int64)r.readiness.blocksProcessed)
              << " flushResidual=" << juce::String(r.readiness.flushResidualDb, 1) << " dBFS flushRaw=" << juce::String(r.readiness.flushRawPeakDb, 1)
              << " dBFS flushSilent=" << (r.readiness.flushReachedSilence ? "yes" : "no") << " parkedDc L/R="
              << juce::String(r.readiness.parkedDcAtRenderStart[0], 4) << "/" << juce::String(r.readiness.parkedDcAtRenderStart[1], 4)
              << " | " << r.readiness.note << "\n";
            for (const auto& line : juce::StringArray::fromLines(r.readiness.trace))
            {
                if (line.isNotEmpty())
                {
                    t << label << ": readiness trace " << line << "\n";
                }
            }
            if (r.temporaryWavFile.existsAsFile())
            {
                const juce::File kept = P.outDir.getChildFile(label + ".wav");
                (void)kept.deleteFile();
                const bool copied = r.temporaryWavFile.copyFileTo(kept);
                const proxy_probe::WavAnalysis a = proxy_probe::analyzeRenderedWav(kept, r.spanEndRenderSamples);
                t << label << ": artifact " << (copied ? kept.getFullPathName() : juce::String("COPY FAILED")) << "\n"
                  << label << ": " << proxy_probe::summarizeAnalysis(a) << "\n";
                if (r.status != proxy_render::ProxyRenderStatus::Succeeded)
                {
                    (void)r.temporaryWavFile.deleteFile(); // retained only for this copy; never left in the project folder
                    P.lastResult.temporaryWavFile = juce::File();
                }
                juce::String report = t;
                report << "\nper-second profile (peak / rms dBFS, both channels folded):\n" << proxy_probe::formatPerSecondProfile(a);
                (void)P.outDir.getChildFile(label + ".txt").replaceWithText(report);
            }
            else
            {
                t << label << ": no artifact (" << (r.status == proxy_render::ProxyRenderStatus::Succeeded ? "unexpected" : "expected for this status") << ")\n";
                (void)P.outDir.getChildFile(label + ".txt").replaceWithText(t);
            }
            return t;
        };
        hooks.proxyRenderProbePublish = [this]() -> juce::String {
            auto& P = proxyRenderProbe_;
            if (!P.hasResult || P.captured == nullptr || proxyRenderEngine_ == nullptr)
            {
                return "publish: nothing to publish";
            }
            const juce::File finalBefore = [this, &P]() -> juce::File {
                const juce::File pf = session.getCurrentProjectFile();
                return pf == juce::File() ? juce::File()
                                          : proxy_store::proxyDirectory(pf.getParentDirectory())
                                                .getChildFile(proxy_store::generationFileName(P.destination, P.lastResult.expectedFingerprint));
            }();
            const bool existedBefore = finalBefore.existsAsFile();
            const juce::int64 sizeBefore = existedBefore ? finalBefore.getSize() : 0;
            const auto fileSha16 = [](const juce::File& f) -> juce::String {
                juce::FileInputStream in(f);
                return in.openedOk() ? juce::SHA256(in).toHexString().substring(0, 16) : juce::String("?");
            };
            const juce::String canonicalHashBefore = existedBefore ? fileSha16(finalBefore) : juce::String();
            juce::String err;
            const bool ok = proxyRenderEngine_->publish(P.destination, *P.captured, P.lastResult, err);
            juce::String t;
            t << "publish: " << (ok ? "OK" : "FAILED: " + err) << " | canonical generation file existed before=" << (existedBefore ? "yes" : "no")
              << (existedBefore ? " size=" + juce::String(sizeBefore) + " sha256=" + canonicalHashBefore : juce::String())
              << " exists after=" << (finalBefore.existsAsFile() ? "yes" : "no")
              << (finalBefore.existsAsFile() ? " size=" + juce::String(finalBefore.getSize()) + " sha256=" + fileSha16(finalBefore) : juce::String())
              << "\n";
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(P.destination) : nullptr;
            const ProjectFileProxyMetadataV20* const meta = c != nullptr ? c->getProxyMetadata() : nullptr;
            if (meta != nullptr)
            {
                const juce::File asset = proxy_store::resolveProxyRelativePath(session.getCurrentProjectFolder(), meta->relativePath);
                const auto check = proxy_store::validatePublishedAsset(session.getCurrentProjectFolder(), *meta);
                t << "publish: metadata now generationId=" << meta->generationId.substring(0, 23) << " lengthSamples=" << juce::String((juce::int64)meta->lengthSamples)
                  << " relativePath=" << meta->relativePath << " asset=" << (asset.existsAsFile() ? juce::String(asset.getSize()) + " bytes" : juce::String("missing"))
                  << " assetCheck=" << (check.ok ? "ok" : "FAILED: " + check.error) << "\n";
            }
            else
            {
                t << "publish: no metadata on the destination\n";
            }
            t << "publish: destination=" << proxy_render::toString(proxyRenderScheduler_.destinationState(P.destination)) << "\n";
            return t;
        };
        hooks.liveMidiTakePreviewGeometry = [this](const TrackId tid, int& x0, int& x1, float& originX, std::int64_t& visStart,
                                                   double& spp) -> bool {
            if (instrumentTimelineRowCoordinator_ == nullptr)
            {
                return false;
            }
            const auto span = instrumentTimelineRowCoordinator_->liveTakePreviewPixelSpanForDiagnostics(tid);
            if (!span.has_value())
            {
                return false;
            }
            x0 = span->first;
            x1 = span->second;
            originX = 0.0f; // the lane maps samples in its own local x (content band starts at x = 0)
            visStart = timelineViewport_.getVisibleStartSamples();
            spp = timelineViewport_.getSamplesPerPixel();
            return true;
        };
        hooks.playheadDisplaySamples = [this]() -> double {
            return std::isfinite(lastPlayheadFrameDisplaySamples_) ? lastPlayheadFrameDisplaySamples_
                                                                   : (double)transport.readPlayheadSamplesForUi();
        };
        hooks.zoomTimelineLikeWheel = [this](const double factor) {
            const double w = (double)rulerView.getWidth();
            const std::int64_t arr = session.getArrangementExtentSamples();
            if (w <= 0.0 || arr <= 0 || factor <= 0.0)
            {
                return;
            }
            const double sppMax = juce::jmax(1.0, (double)arr / w);
            // Zoom around the playhead's current column (like a wheel gesture over it), so the
            // region under test stays on screen.
            const double spp = timelineViewport_.getSamplesPerPixel();
            const double headX = spp > 0.0
                                     ? ((double)transport.readPlayheadSamplesForUi() - (double)timelineViewport_.getVisibleStartSamples()) / spp
                                     : w * 0.5;
            timelineViewport_.zoomAroundSample(1.0 / factor, juce::jlimit(0.0, w, headX), w, arr, 1.0, sppMax);
        };
        hooks.panTimelineBySamples = [this](const std::int64_t delta) {
            const double w = (double)rulerView.getWidth();
            const std::int64_t arr = session.getArrangementExtentSamples();
            if (w <= 0.0 || arr <= 0)
            {
                return;
            }
            timelineViewport_.panBySamples(delta, w, arr);
        };
        // --- Proxy + recording, shared stop boundary, extent growth, schema-1 proxies ---------
        hooks.proxyForcePrimaryUnavailable = [this](const TrackId tid, const bool unavailable) {
            if (proxyPlaybackCoordinator_ != nullptr)
            {
                proxyPlaybackCoordinator_->setPrimaryForcedUnavailableForTests(tid, unavailable);
                proxyPlaybackCoordinator_->refreshDestination(tid);
            }
        };
        hooks.proxyRuntimeStateName = [this](const TrackId tid) -> juce::String {
            return proxyPlaybackCoordinator_ != nullptr
                       ? juce::String(proxy_playback::proxyPlaybackSourceStateName(proxyPlaybackCoordinator_->runtimeStateForTrack(tid)))
                       : juce::String("n/a");
        };
        hooks.proxyDestinationStateName = [this](const TrackId tid) -> juce::String {
            switch (proxyRenderScheduler_.destinationState(tid))
            {
                case proxy_render::ProxyDestinationState::Absent: return "Absent";
                case proxy_render::ProxyDestinationState::Current: return "Current";
                case proxy_render::ProxyDestinationState::Stale: return "Stale";
                case proxy_render::ProxyDestinationState::Rendering: return "Rendering";
                case proxy_render::ProxyDestinationState::Failed: return "Failed";
            }
            return "?";
        };
        hooks.proxyIsLiveRecordingOverrideActive = [this](const TrackId tid) {
            return proxyPlaybackCoordinator_ != nullptr && proxyPlaybackCoordinator_->isLiveRecordingOverrideActive(tid);
        };
        hooks.proxyIsLiveMonitorOverrideActive = [this](const TrackId tid) {
            return proxyPlaybackCoordinator_ != nullptr && proxyPlaybackCoordinator_->isLiveMonitorOverrideActive(tid);
        };
        hooks.proxyGenerationInfo = [this](const TrackId tid) -> juce::String {
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid) : nullptr;
            const auto* meta = c != nullptr ? c->getProxyMetadata() : nullptr;
            return meta == nullptr ? juce::String()
                                   : meta->generationId + " schema=" + juce::String(meta->fingerprintSchemaVersion)
                                         + " pub=" + juce::String((juce::int64)meta->primaryStateRevisionAtPublish)
                                         + " save=" + juce::String((juce::int64)meta->primaryStateRevisionAtSave);
        };
        hooks.proxyCopySecondaryConfigFromTrack = [this](const TrackId target, const TrackId from, juce::String& failReason) -> bool {
            InstrumentTrackController* const dst
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(target) : nullptr;
            InstrumentTrackController* const src
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(from) : nullptr;
            if (dst == nullptr || src == nullptr || !src->hasSecondaryInstrument())
            {
                failReason = "source track has no Secondary to copy";
                return false;
            }
            (void)dst->setSecondaryInstrumentFromUi(src->getSecondaryDescriptor(), src->getSecondaryPluginBundlePath());
            instrumentRuntimeCoordinator_->noteSecondaryConfigurationChanged(target);
            if (proxyPlaybackCoordinator_ != nullptr)
            {
                proxyPlaybackCoordinator_->refreshDestination(target);
            }
            return true;
        };
        hooks.liveMidiAttachCaptureSinkToSecondary = [this](const TrackId tid, juce::String& failReason) -> bool {
            ExperimentalInstrumentHost* const h
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getSecondaryInstrumentHostForTrack(tid) : nullptr;
            if (h == nullptr)
            {
                failReason = "no Secondary host for track " + juce::String((juce::int64)tid);
                return false;
            }
            stabilityLiveMidiCaptureSink_.reset();
            h->installMidiDeliveryCaptureSinkForTests(&stabilityLiveMidiCaptureSink_);
            return true;
        };
        hooks.midiInputStatusTextForTrack = [this](const TrackId tid) -> juce::String {
            return liveMidiInputCoordinator_ != nullptr ? liveMidiInputCoordinator_->describeInputStatus(tid) : juce::String();
        };
        hooks.addMidiSourceRowRoutedTo = [this](const TrackId destination, const int pitch, const int noteCount,
                                                juce::String& failReason) -> TrackId {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                failReason = "no instrument runtime coordinator";
                return kInvalidTrackId;
            }
            const auto midiIdOpt = session.addMidiTrack();
            if (!midiIdOpt.has_value())
            {
                failReason = "could not add a Midi track row";
                return kInvalidTrackId;
            }
            InstrumentTrackController* const midiCtl
                = instrumentRuntimeCoordinator_->getOrCreateMidiContentControllerForTrack(*midiIdOpt);
            if (midiCtl == nullptr)
            {
                failReason = "could not create the midi content controller";
                return kInvalidTrackId;
            }
            if (!session.setTrackMidiDestination(*midiIdOpt, destination))
            {
                failReason = "setTrackMidiDestination refused";
                return kInvalidTrackId;
            }
            midiCtl->refreshMidiOutputChannelFromSession();
            std::vector<TimelineMidiNote> notes;
            for (int q = 0; q < noteCount; ++q)
            {
                TimelineMidiNote n;
                n.midiNote = pitch;
                n.velocity = 100;
                n.channel = 1;
                n.startTick = static_cast<std::int64_t>(q) * kDefaultExperimentalTicksPerQuarter;
                n.durationTicks = kDefaultExperimentalTicksPerQuarter / 2;
                notes.push_back(n);
            }
            if (midiCtl->appendImportedTimelineMidiClipAtSamples(notes, 0, "RoutedSource") == 0)
            {
                failReason = "could not create the routed source clip";
                return kInvalidTrackId;
            }
            // Same post-add sync as the UI add-track menu (routing plan indices shifted; new
            // timeline row attached).
            syncViewportFromSession();
            trackLanesView.syncTracksFromSession();
            refreshInstrumentUi();
            inspectorView_.refreshFromSession();
            return *midiIdOpt;
        };
        hooks.proxyRenderNow = [this](const TrackId tid) -> bool {
            // The Inspector path: update mode -> Manual (temp project only), then "Render now".
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid) : nullptr;
            if (c == nullptr || proxyUpdatePolicyService_ == nullptr)
            {
                return false;
            }
            if (!c->setProxyUpdateModeFromUi(proxy_policy::proxyUpdateModePersistedString(proxy_policy::ProxyUpdateMode::Manual)))
            {
                return false;
            }
            proxyUpdatePolicyService_->tick();
            return proxyUpdatePolicyService_->renderNow(tid);
        };
        hooks.proxyJobStatusText = [this](const TrackId tid) -> juce::String {
            const proxy_render::ProxyJobStatus st = proxyRenderScheduler_.jobStatus(tid);
            juce::String s = st.exists ? juce::String("phase=") + proxy_render::toString(st.phase) + " expected=" + st.expectedFingerprint.substring(0, 23)
                                             + " msg=\"" + st.message + "\" renderedMs=" + juce::String((juce::int64)st.progressRenderedMs)
                                       : juce::String("no job");
            if (proxyRenderEngine_ != nullptr)
            {
                const proxy_render::ProxyCurrentIdentity now = proxyRenderEngine_->currentIdentity(tid);
                s << " | identity now=" << now.expectedFingerprint.substring(0, 23) << " comparable=" << now.publishedComparableFingerprint.substring(0, 23)
                  << " rev=" << juce::String((juce::int64)now.primarySemanticRevision)
                  << " (job rev=" << juce::String((juce::int64)st.primarySemanticRevision) << ")"
                  << " published=" << proxyRenderEngine_->publishedGenerationId(tid).substring(0, 23);
            }
            return s;
        };
        hooks.recordingPlacementOffsetSamples = [this]() -> std::int64_t { return latencyStore_.getCurrentRecordingOffsetSamples(); };
        hooks.getStoredArrangementExtentSamples = [this]() -> std::int64_t { return session.getStoredArrangementExtentSamples(); };
        hooks.getArrangementExtentSamples = [this]() -> std::int64_t { return session.getArrangementExtentSamples(); };
        hooks.lastRecordRunBoundaries = [this]() -> juce::String {
            if (recordingCoordinator_ == nullptr)
            {
                return {};
            }
            const RecordRunBoundaries b = recordingCoordinator_->getLastRunBoundariesForDiagnostics();
            return "start tl=" + juce::String((juce::int64)b.startTimelineSample) + " mono=" + juce::String((juce::int64)b.startMonoSample)
                   + " wrap=" + juce::String((int)b.startWrapSerial) + " | stop tl=" + juce::String((juce::int64)b.stopTimelineSample)
                   + " mono=" + juce::String((juce::int64)b.stopMonoSample) + " wrap=" + juce::String((int)b.stopWrapSerial)
                   + " | frames=" + juce::String((juce::int64)(b.stopMonoSample - b.startMonoSample))
                   + " acked=" + (b.acknowledgedByEngine ? "yes" : "no");
        };
        hooks.deviceBlockSizeSamples = [this]() -> int {
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            return dev != nullptr ? dev->getCurrentBufferSizeSamples() : 0;
        };
        hooks.closeAudioDeviceForTest = [this]() -> bool {
            if (deviceManager.getCurrentAudioDevice() == nullptr)
            {
                return false;
            }
            deviceManager.closeAudioDevice();
            return deviceManager.getCurrentAudioDevice() == nullptr;
        };
        hooks.restartAudioDeviceForTest = [this](juce::String& failReason) -> bool {
            deviceManager.restartLastAudioDevice();
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            if (dev == nullptr || !dev->isOpen())
            {
                failReason = "audio device did not restart";
                return false;
            }
            return true;
        };
        hooks.audioClipWindowsForTrack = [this](const TrackId tid) {
            std::vector<std::pair<std::int64_t, std::int64_t>> out;
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int ix = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            if (ix < 0)
            {
                return out;
            }
            const Track& t = snap->getTrack(ix);
            for (int i = 0; i < t.getNumPlacedClips(); ++i)
            {
                const PlacedClip& c = t.getPlacedClip(i);
                out.emplace_back(c.getStartSample(), c.getEffectiveLengthSamples());
            }
            return out;
        };
        hooks.verifyLiveMidiHeaderCells = [this](const TrackId tid, juce::String& report, juce::String& failReason) -> bool {
            const TrackHeaderView* const header = trackLanesView.findInstrumentRowHeaderForDiagnostics(tid);
            if (header == nullptr)
            {
                failReason = "no header view for track " + juce::String((juce::int64)tid);
                return false;
            }
            const auto bounds = header->getLocalBounds();
            const auto monitor = header->getMonitorButtonBounds();
            const auto arm = header->getArmButtonBounds();
            report = "track " + juce::String((juce::int64)tid) + " header " + bounds.toString() + " monitor=" + monitor.toString()
                     + " arm=" + arm.toString();
            if (monitor.isEmpty() || arm.isEmpty())
            {
                failReason = "Monitor / Arm cell missing on a live-MIDI row";
                return false;
            }
            if (!bounds.contains(monitor) || !bounds.contains(arm))
            {
                failReason = "Monitor / Arm cell clipped by the header bounds";
                return false;
            }
            return true;
        };

        hooks.audioHealthProbeBegin = [this] {
            (void)playbackEngine_.readAndResetOutputPeakHoldForDiagnostics();
            stabilityAudioProbeCallbackBaseline_ = playbackEngine_.readAudioCallbackEnterCountForDiagnostics();
            stabilityAudioProbeAdvancedBaseline_ = transport.readAdvancedSamplesTotalForDiagnostics();
            stabilityAudioProbeHostBlocksBaseline_.clear();
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                instrumentRuntimeCoordinator_->forEachInstrumentController(
                    [this](const TrackId tid, InstrumentTrackController&) {
                        if (ExperimentalInstrumentHost* const h
                            = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid))
                        {
                            stabilityAudioProbeHostBlocksBaseline_[tid]
                                = h->readRtActivitySnapshotForDiagnostics().processOkBlocks;
                        }
                    });
            }
        };
        hooks.audioHealthProbeVerify = [this](const juce::String& label, juce::String& failReason) -> bool {
            const std::uint64_t callbacksDelta = playbackEngine_.readAudioCallbackEnterCountForDiagnostics()
                                                 - stabilityAudioProbeCallbackBaseline_;
            const float peak = playbackEngine_.readAndResetOutputPeakHoldForDiagnostics();
            // Monotonic consumed-sample counter: immune to cycle wraps and seeks, unlike the playhead.
            const std::uint64_t advancedDelta = transport.readAdvancedSamplesTotalForDiagnostics()
                                                - stabilityAudioProbeAdvancedBaseline_;
            const bool playing = transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
            // -80 dBFS: well below any real programme material, well above float noise.
            constexpr float kAudibleFloor = 1.0e-4f;
            juce::String hostsLine;
            std::map<TrackId, std::uint64_t> hostDeltas;
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                instrumentRuntimeCoordinator_->forEachInstrumentController(
                    [&](const TrackId tid, InstrumentTrackController&) {
                        ExperimentalInstrumentHost* const h
                            = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
                        if (h == nullptr)
                        {
                            return;
                        }
                        const auto s = h->readRtActivitySnapshotForDiagnostics();
                        const auto itB = stabilityAudioProbeHostBlocksBaseline_.find(tid);
                        const std::uint64_t delta
                            = itB != stabilityAudioProbeHostBlocksBaseline_.end() ? s.processOkBlocks - itB->second
                                                                                  : s.processOkBlocks;
                        hostDeltas[tid] = delta;
                        hostsLine << " inst[" << juce::String((juce::int64)tid) << "]{blocks+="
                                  << juce::String((juce::int64)delta) << " lastPeak="
                                  << juce::String(s.lastProcessedBlockPeak, 4)
                                  << " loaded=" << (h->hasInstrument() ? "yes" : "no") << "}";
                    });
            }
            appendStabilityRunLine("  audio-health[" + label + "]: callbacks+=" + juce::String((juce::int64)callbacksDelta)
                                   + " advanced+=" + juce::String((juce::int64)advancedDelta)
                                   + " outputPeak=" + juce::String(peak, 6)
                                   + " playhead=" + juce::String((juce::int64)transport.readPlayheadSamplesForUi())
                                   + " intent=" + (playing ? "playing" : "not-playing")
                                   + " engine={" + playbackEngine_.describeAudioCallbackStateForDiagnostics() + "}"
                                   + hostsLine);
            if (callbacksDelta == 0)
            {
                failReason = label + ": audio callback stopped (no callbacks during the window)";
                return false;
            }
            if (playing && advancedDelta == 0)
            {
                failReason = label + ": callback runs but the transport did not advance";
                return false;
            }
            if (!std::isfinite(peak))
            {
                failReason = label + ": device output contained NaN/inf";
                return false;
            }
            if (peak < kAudibleFloor)
            {
                failReason = label + ": device output silent (peak " + juce::String(peak, 6) + ")";
                return false;
            }
            // An instrument that rendered in the previous window must still be rendering now:
            // "audio tracks play but every instrument went quiet" is exactly the failure a
            // device-output peak alone cannot see.
            for (const auto& [tid, prev] : stabilityAudioProbeHostBlocksPrevWindow_)
            {
                const auto it = hostDeltas.find(tid);
                if (prev > 0 && it != hostDeltas.end() && it->second == 0)
                {
                    failReason = label + ": instrument track " + juce::String((juce::int64)tid)
                                 + " stopped processing blocks (rendered " + juce::String((juce::int64)prev)
                                 + " in the previous window)";
                    return false;
                }
            }
            stabilityAudioProbeHostBlocksPrevWindow_ = std::move(hostDeltas);
            return true;
        };
        hooks.addMidiTrackLikeUi = [this]() -> std::optional<TrackId> { return addMidiTrackFromUi(); };
        hooks.isMidiEditorOpen = [this]() -> bool {
            return midiEditorPresenter_ != nullptr && midiEditorPresenter_->midiEditorWindow() != nullptr
                   && midiEditorPresenter_->midiEditorWindow()->isVisible();
        };
        hooks.clipCountOnTrack = [this](const TrackId tid) -> int {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                return 0;
            }
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
            return c != nullptr ? (int)c->getClips().size() : 0;
        };
        hooks.selectFirstClipOnTrack = [this](const TrackId tid) -> InstrumentMidiClipId {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                return 0;
            }
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
            if (c == nullptr || c->getClips().empty() || c->getClips().front() == nullptr)
            {
                return 0;
            }
            const InstrumentMidiClipId id = c->getClips().front()->id;
            c->setSelectedClipIdsExclusive(id);
            return id;
        };
        hooks.openMidiEditorOnFirstClipOfTrack = [this](const TrackId tid) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr || midiEditorPresenter_ == nullptr)
            {
                return false;
            }
            InstrumentTrackController* const c
                = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
            if (c == nullptr || c->getClips().empty() || c->getClips().front() == nullptr)
            {
                return false;
            }
            midiEditorPresenter_->openMidiEditorForInstrumentClip(tid, c->getClips().front()->id);
            return midiEditorPresenter_->midiEditorWindow() != nullptr;
        };
        hooks.copyThenPasteSelectedMidiClipLikeUi = [this] {
            if (clipPasteboardController_ != nullptr)
            {
                clipPasteboardController_->invokeCopySelectedClipFromWindowShortcut();
                clipPasteboardController_->invokePasteClipFromWindowShortcut();
            }
        };
        hooks.moveMidiClipCrossTrackLikeUi
            = [this](const TrackId src, const TrackId dst, const InstrumentMidiClipId clipId) -> bool {
            if (instrumentRuntimeCoordinator_ == nullptr || undoRedoCoordinator_ == nullptr)
            {
                return false;
            }
            bool moved = false;
            // Same wrapping the arrangement drag commits: the reconcile hook fires inside this call.
            undoRedoCoordinator_->executeUndoableInstrumentEdit(
                "Move MIDI clip", [this, src, dst, clipId, &moved]() -> bool {
                    moved = instrumentRuntimeCoordinator_->moveInstrumentMidiClipsBetweenTracks(
                        src, dst, { clipId }, 0);
                    if (moved)
                    {
                        trackLanesView.repaint();
                        inspectorView_.refreshFromSession();
                    }
                    return moved;
                });
            return moved;
        };
        hooks.fixtureInstrumentTrackId = [this]() -> TrackId { return stabilityMidiRoutingInstTid_; };
        hooks.fixtureMidiTrackId = [this]() -> TrackId { return stabilityMidiRoutingMidiLowerTid_; };
        hooks.refreshInstrumentEditorUi = [this] {
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->refreshInstrumentUiIfOpen();
            }
        };
        hooks.importMidiFileOntoTrack
            = [this](const TrackId tid, const juce::File& midi, juce::String& failReason) -> bool {
            if (instrumentMidiImportCoordinator_ == nullptr)
            {
                failReason = "no MIDI import coordinator";
                return false;
            }
            const auto outcome = instrumentMidiImportCoordinator_->importMidiFileOntoTrackNow(tid, midi);
            appendStabilityRunLine("  import: ok=" + juce::String(outcome.ok ? "yes" : "no")
                                   + " notes=" + juce::String(outcome.notesParsed)
                                   + " clipId=" + juce::String((juce::int64)outcome.createdClipId)
                                   + (outcome.userMessage.isNotEmpty() ? " msg=\"" + outcome.userMessage + "\""
                                                                       : juce::String{}));
            if (!outcome.ok)
            {
                failReason = "MIDI import did not create a clip: " + outcome.userMessage;
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
            inspectorPanel_,
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
            &arrangementVerticalScrollBar_,
            &soloMemoryStrip_,
            &trackRowHeightPresetCombo_,
        });
    }

    /// Dropdown status <- lanes heights: Small/Medium/Large when ALL rows exactly match that
    /// preset, otherwise no selection (the combo then shows its "Custom" status text). Silent
    /// sync — never re-fires the preset command.
    void refreshTrackRowHeightPresetComboFromLanes()
    {
        namespace trh = track_row_heights;
        const std::optional<trh::TrackRowHeightPreset> status
            = trackLanesView.uniformTrackRowHeightPresetStatus();
        const int wantedId = status.has_value() ? trackRowHeightComboIdForPreset(*status) : 0;
        if (trackRowHeightPresetCombo_.getSelectedId() != wantedId)
        {
            trackRowHeightPresetCombo_.setSelectedId(wantedId, juce::dontSendNotification);
        }
    }

    /// Scrollbar <- lanes model, without notification (the bar never drives layout from here).
    void syncArrangementVerticalScrollBarFromLanes()
    {
        const TrackLanesView::VerticalScrollModel m = trackLanesView.verticalScrollModel();
        arrangementVerticalScrollBar_.setRangeLimits(0.0, (double) juce::jmax(m.contentHeightPx, m.viewportHeightPx),
                                                     juce::dontSendNotification);
        arrangementVerticalScrollBar_.setCurrentRange((double) m.offsetPx, (double) m.viewportHeightPx,
                                                      juce::dontSendNotification);
        // Page-wise trough clicks move by most of a viewport (one row of overlap for orientation).
        arrangementVerticalScrollBar_.setSingleStepSize((double) juce::jmax(16, trackLanesView.defaultRowHeightPx() / 2));
    }

    /// Diagnostics / scenario hook: the bar's current geometry vs the lanes' model.
    [[nodiscard]] juce::String arrangementVerticalScrollBarDiagnostics() const
    {
        const TrackLanesView::VerticalScrollModel m = trackLanesView.verticalScrollModel();
        juce::String s;
        s << "scrollbar visible=" << (arrangementVerticalScrollBar_.isVisible() ? 1 : 0)
          << " bounds=" << arrangementVerticalScrollBar_.getBounds().toString()
          << " rangeStart=" << (int) std::lround(arrangementVerticalScrollBar_.getCurrentRangeStart())
          << " rangeSize=" << (int) std::lround(arrangementVerticalScrollBar_.getCurrentRangeSize())
          << " limit=" << (int) std::lround(arrangementVerticalScrollBar_.getMaximumRangeLimit())
          << " | lanes content=" << m.contentHeightPx << " viewport=" << m.viewportHeightPx
          << " offset=" << m.offsetPx << " max=" << m.maxOffsetPx()
          << " lanesBounds=" << trackLanesView.getBounds().toString();
        return s;
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
        // Device/channel-set changes alter which inputs the Inspector's Audio Input combo can
        // offer (and whether the current assignment shows "(unavailable)").
        inspectorView_.refreshFromSession();
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
            anyRecordingInProgress()
            || (recordingCoordinator_ != nullptr && recordingCoordinator_->isCountInActive()));
        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->tickStructuralEditBlockedHeaderStripRepaint(
                trackLanesView.isStructuralTimelineEditBlocked());
            // Live MIDI take: the growing REC region follows the playhead on this 10 Hz tick
            // (never at playhead-stripe cadence — see the buffered-lane rule).
            if (liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isTakeActive())
            {
                instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
            }
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
    /// [Message thread] "Add MIDI Track" (transport add menu and stability scenarios): appends the
    /// TrackKind::Midi row, gives it its plugin-less MIDI content controller — which is what
    /// creates the row's MIDI event lane and lets the engine publish it as a MIDI source — and
    /// runs the same post-add UI sync as every other add-track entry.
    std::optional<TrackId> addMidiTrackFromUi()
    {
        if (isProjectLoadInProgress())
        {
            return std::nullopt; // the staged load owns the session until finalize
        }
        const std::optional<TrackId> newMidiId = session.addMidiTrack();
        if (newMidiId.has_value() && instrumentRuntimeCoordinator_ != nullptr)
        {
            (void)instrumentRuntimeCoordinator_->getOrCreateMidiContentControllerForTrack(*newMidiId);
        }
        syncViewportFromSession();
        trackLanesView.syncTracksFromSession();
        refreshInstrumentUi();
        inspectorView_.refreshFromSession();
        return newMidiId;
    }

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

    // --- Mixer bindings (explicit TrackId; the same production paths the Inspector / headers use) ----

    /// [Message thread] The ACTIVE audio device's input channels (Inspector + mixer Audio Input).
    [[nodiscard]] InspectorAudioInputDeviceSnapshot makeAudioInputDeviceSnapshot() const
    {
        InspectorAudioInputDeviceSnapshot snap;
        if (juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice())
        {
            snap.deviceAvailable = true;
            snap.physicalInputNames = dev->getInputChannelNames();
            snap.activeInputChannels = dev->getActiveInputChannels();
        }
        return snap;
    }

    /// [Message thread] MIDI devices + status line for a row (Inspector + mixer MIDI Input).
    [[nodiscard]] InspectorMidiInputSnapshot makeMidiInputSnapshotForTrack(const TrackId tid) const
    {
        InspectorMidiInputSnapshot snap;
        if (liveMidiInputCoordinator_ == nullptr)
        {
            return snap;
        }
        for (const auto& d : liveMidiInputCoordinator_->availableDevicesFor(tid))
        {
            snap.devices.push_back({ d.identifier, d.name, d.present });
        }
        snap.statusLine = liveMidiInputCoordinator_->describeInputStatus(tid);
        return snap;
    }

    [[nodiscard]] TrackKind trackKindOf(const TrackId tid) const
    {
        const auto snap = session.loadSessionSnapshotForAudioThread();
        const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
        return idx >= 0 ? snap->getTrack(idx).getKind() : TrackKind::Audio;
    }

    /// [Message thread] Repaint everything that mirrors a row's mute / power / monitor / arm /
    /// activation after a mixer button (headers, instrument rows, Inspector).
    void refreshRowChromeAfterMixerAction()
    {
        trackLanesView.syncTracksFromSession();
        trackLanesView.repaint();
        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
        }
        inspectorView_.refreshFromSession();
    }

    /// [Message thread] Activate a row exactly like its header's name click (audio headers:
    /// session active + keyed instrument controllers deactivated; instrument / MIDI rows: that
    /// row's controller active exclusively), then refresh the Inspector and the arrangement.
    void activateTrackLikeHeaderClickForMixer(const TrackId tid)
    {
        if (instrumentRuntimeCoordinator_ == nullptr)
        {
            session.setActiveTrack(tid);
            inspectorView_.refreshFromSession();
            trackLanesView.repaint();
            return;
        }
        if (instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) != nullptr)
        {
            instrumentRuntimeCoordinator_->deactivateAllKeyedAndStagingControllers();
            session.setActiveTrack(tid);
            instrumentRuntimeCoordinator_->setKeyedInstrumentControllersActiveExclusive(tid);
        }
        else
        {
            session.setActiveTrack(tid);
            instrumentRuntimeCoordinator_->deactivateKeyedInstrumentControllersOnly();
        }
        refreshRowChromeAfterMixerAction();
    }

    /// ONE solo seam for every consumer (arrangement headers, instrument/MIDI rows, mixer strips):
    /// display state from the coordinator's published picture, S clicks into its command path.
    [[nodiscard]] SoloUiHooks makeSoloUiHooks()
    {
        SoloUiHooks hooks;
        hooks.displayState = [this](const TrackId tid) {
            TrackSoloDisplayState st;
            if (soloCoordinator_ == nullptr)
            {
                return st;
            }
            st.soloed = soloCoordinator_->isTrackExplicitlySoloed(tid);
            st.muteLocked = soloCoordinator_->isMuteChangeLocked();
            if (st.muteLocked
                && soloCoordinator_->audioDecisionForTrack(tid) == SoloTrackAudioDecision::ForcedSilent)
            {
                // Silenced BY SOLO and not stored-muted ⇒ the distinct dimmed M tint. A
                // stored-muted row keeps its normal mute colours (plus the lock).
                bool storedMuted = false;
                if (const auto snap = session.loadSessionSnapshotForAudioThread())
                {
                    const int idx = snap->findTrackIndexById(tid);
                    storedMuted = idx >= 0 && snap->getTrack(idx).isMuted();
                }
                st.soloSilenced = !storedMuted;
            }
            return st;
        };
        hooks.toggleSolo = [this](const TrackId tid) {
            if (soloCoordinator_ != nullptr)
            {
                juce::ignoreUnused(soloCoordinator_->toggleTrackSolo(tid));
            }
        };
        return hooks;
    }

    /// Repaint everything that shows solo state. The mixer strips follow on their own 10 Hz
    /// refresh poll (same discipline as every other mixer state change).
    void refreshAllSoloUi()
    {
        trackLanesView.repaint();
        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
        }
        soloMemoryStrip_.repaint();
        inspectorView_.refreshFromSession();
    }

    void buildMixerStripBindings()
    {
        MixerStripBindings& b = mixerBindings_;
        b.loadSnapshot = [this] { return session.loadSessionSnapshotForAudioThread(); };
        b.activeTrackId = [this] { return session.getActiveTrackId(); };
        b.audioInputDeviceSnapshot = [this] { return makeAudioInputDeviceSnapshot(); };
        b.midiInputSnapshot = [this](const TrackId tid) { return makeMidiInputSnapshotForTrack(tid); };
        b.deviceOutputDescription = [this]() -> juce::String {
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            if (dev == nullptr)
            {
                return "(no audio device)";
            }
            const juce::StringArray names = dev->getOutputChannelNames();
            const juce::BigInteger active = dev->getActiveOutputChannels();
            juce::String chans;
            for (int p = active.findNextSetBit(0); p >= 0; p = active.findNextSetBit(p + 1))
            {
                if (chans.isNotEmpty())
                {
                    chans << " + ";
                }
                chans << (p < names.size() && names[p].isNotEmpty() ? names[p] : "Out " + juce::String(p + 1));
            }
            return dev->getName() + (chans.isNotEmpty() ? ": " + chans : juce::String());
        };
        b.activateTrack = [this](const TrackId tid) { activateTrackLikeHeaderClickForMixer(tid); };
        if (trackLanesEditCoordinator_ != nullptr)
        {
            b.edits = trackLanesEditCoordinator_->trackEditActions();
        }
        // Direct channel writes — the same setters the Inspector channel panel / pan field use.
        b.setChannelFaderGain = [this](const TrackId tid, const float g) { session.setTrackChannelFaderGain(tid, g); };
        b.setStereoPan = [this](const TrackId tid, const float p) { session.setTrackStereoPan(tid, p); };

        // Mute / Power: instrument and MIDI rows go through their controller (it mirrors the
        // flag into the session and republishes its render snapshot — the header's path); other
        // rows write the session directly like the audio / group / master headers.
        b.toggleMute = [this](const TrackId tid) {
            InstrumentTrackController* const ctl
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (ctl != nullptr)
            {
                ctl->setMuted(!ctl->isMuted());
            }
            else if (const auto snap = session.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    session.setTrackMuted(tid, !snap->getTrack(idx).isMuted());
                }
            }
            activateTrackLikeHeaderClickForMixer(tid);
        };
        // Solo: the same seam the arrangement headers use (S cell + locked-M rendering).
        b.solo = makeSoloUiHooks();
        b.isPowerInteractable = [this] { return !trackLanesView.isStructuralTimelineEditBlocked(); };
        b.togglePower = [this](const TrackId tid) -> bool {
            if (trackLanesView.isStructuralTimelineEditBlocked())
            {
                return false; // never while playing, recording or counting in (header rule)
            }
            InstrumentTrackController* const ctl
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid) : nullptr;
            if (ctl != nullptr)
            {
                ctl->setPowerOn(!ctl->isPowerOn());
            }
            else if (const auto snap = session.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx < 0 || snap->getTrack(idx).getKind() == TrackKind::Master)
                {
                    return false;
                }
                session.setTrackOff(tid, !snap->getTrack(idx).isTrackOff());
            }
            activateTrackLikeHeaderClickForMixer(tid);
            return true;
        };
        // Monitor / Arm: audio rows = engine input monitoring + RecorderService arm; Instrument /
        // Midi rows = the live-MIDI coordinator's runtime flags. Runtime only, never undoable.
        b.monitorAvailable = [this](const TrackId tid) {
            const TrackKind k = trackKindOf(tid);
            return k == TrackKind::Audio || (trackKindAcceptsLiveMidiInput(k) && liveMidiInputCoordinator_ != nullptr);
        };
        b.isMonitorOn = [this](const TrackId tid) {
            if (trackKindOf(tid) == TrackKind::Audio)
            {
                return playbackEngine_.isTrackInputMonitoringEnabled(tid);
            }
            return liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isMonitorEnabled(tid);
        };
        b.toggleMonitor = [this](const TrackId tid) {
            if (trackKindOf(tid) == TrackKind::Audio)
            {
                playbackEngine_.setTrackInputMonitoringEnabled(tid, !playbackEngine_.isTrackInputMonitoringEnabled(tid));
            }
            else if (liveMidiInputCoordinator_ != nullptr)
            {
                liveMidiInputCoordinator_->setMonitorEnabled(tid, !liveMidiInputCoordinator_->isMonitorEnabled(tid));
            }
            activateTrackLikeHeaderClickForMixer(tid);
        };
        b.armAvailable = [this](const TrackId tid) {
            const TrackKind k = trackKindOf(tid);
            return trackKindAcceptsRecordArm(k) || (trackKindAcceptsLiveMidiInput(k) && liveMidiInputCoordinator_ != nullptr);
        };
        b.isRecordArmed = [this](const TrackId tid) {
            if (trackKindOf(tid) == TrackKind::Audio)
            {
                return recorder_.getArmedTrackId() == tid;
            }
            return liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isRecordArmed(tid);
        };
        b.toggleRecordArm = [this](const TrackId tid) {
            if (trackKindOf(tid) == TrackKind::Audio)
            {
                if (recorder_.getArmedTrackId() == tid)
                {
                    recorder_.disarm();
                }
                else
                {
                    recorder_.armForRecording(tid);
                }
            }
            else if (liveMidiInputCoordinator_ != nullptr)
            {
                liveMidiInputCoordinator_->setRecordArmed(tid, !liveMidiInputCoordinator_->isRecordArmed(tid));
            }
            activateTrackLikeHeaderClickForMixer(tid);
        };
        b.instrumentEditorAvailable = [this](const TrackId tid) {
            ExperimentalInstrumentHost* const h
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid) : nullptr;
            return h != nullptr && h->hasInstrument();
        };
        b.openInstrumentEditor = [this](const TrackId tid) {
            if (ExperimentalInstrumentHost* const h
                = instrumentRuntimeCoordinator_ != nullptr ? instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid) : nullptr)
            {
                h->openNativeEditor();
            }
        };
        b.instrumentAlternativesAvailable = [this](const TrackId tid) { return trackKindOf(tid) == TrackKind::Instrument; };
        b.showInstrumentAlternatives = [this](const TrackId tid, const juce::Rectangle<int> anchor) {
            instrument_alternatives_popup::show(tid, anchor, instrumentProxyUiHost_, instrumentSecondaryUiHost_);
        };
        if (vst3PluginPickerCoordinator_ != nullptr)
        {
            // The picker is anchored at the mixer content once the window exists (set below).
            b.inserts = PluginHostUiBindings::makeInsertSeam(pluginHost_, *vst3PluginPickerCoordinator_, nullptr);
        }
    }

    /// [Message thread] Restore the mixer's machine-local layout (bounds clamped to a display,
    /// section flags) from `ui-layout.xml`; absent values keep the defaults.
    void restoreMixerLayoutFromSettings()
    {
        if (mixerWindow_ == nullptr)
        {
            return;
        }
        if (const auto bounds = uiLayoutSettings_.getMixerWindowBounds())
        {
            mixerWindow_->applyStoredBounds(*bounds);
        }
        mixer_layout::SectionVisibility v;
        mixer_layout::SectionHeights heights; // defaults for older files without heights
        for (int i = 0; i < mixer_layout::kSectionCount; ++i)
        {
            const auto s = static_cast<mixer_layout::Section>(i);
            if (const auto shown = uiLayoutSettings_.getMixerSectionShown(mixer_layout::sectionKey(s)))
            {
                v.set(s, *shown);
            }
            if (mixer_layout::isUpperSection(s))
            {
                if (const auto px = uiLayoutSettings_.getMixerSectionHeightPx(mixer_layout::sectionKey(s)))
                {
                    heights.set(s, *px); // clamped to the section's usable range by `set`
                }
            }
        }
        mixerWindow_->content().setSectionVisibility(v);
        mixerWindow_->content().setSectionHeights(heights);
        // The VST3 picker anchors its chooser at the mixer content when an insert is added there.
        if (vst3PluginPickerCoordinator_ != nullptr)
        {
            mixerBindings_.inserts = PluginHostUiBindings::makeInsertSeam(pluginHost_, *vst3PluginPickerCoordinator_, &mixerWindow_->content());
        }
    }

    /// [Message thread] `--stability-mixer` hooks: every one drives the REAL window / strips.
    /// [Message thread] `--stability-load-progress` + `--stability-duplicate-track` hooks. Every
    /// hook drives the production path the user's control ends in (staged loader, header-menu
    /// duplicate command, the Session setters, the real ScrollBar) or reads existing diagnostics.
    void installLoadProgressAndDuplicateTrackStabilityHooks(StabilityRunnerHooks& hooks)
    {
        auto& L = hooks.load;
        L.beginInteractiveLoad = [this](const juce::File& f) {
            if (projectIoCoordinator_ != nullptr)
            {
                projectIoCoordinator_->loadProjectFromFile(f, ProjectIoCoordinator::LoadDrive::Interactive);
            }
        };
        L.isLoadInProgress = [this] { return isProjectLoadInProgress(); };
        L.progressText = [this]() -> juce::String {
            return projectIoCoordinator_ != nullptr ? projectIoCoordinator_->loadProgressTextForDiagnostics() : juce::String();
        };
        L.captureProgressWindowPng = [this](const juce::File& png) -> bool {
            return projectIoCoordinator_ != nullptr && projectIoCoordinator_->captureLoadProgressWindowPng(png);
        };
        L.togglePlayLikeButton = [this] {
            if (transportPlayPauseStopController_ != nullptr)
            {
                transportPlayPauseStopController_->togglePlayPauseFromUi();
            }
        };
        L.isTransportPlaying = [this] { return transport.readPlaybackIntentForUi() == PlaybackIntent::Playing; };
        L.addAudioTrackLikeUi = [this]() -> std::optional<TrackId> {
            // Same guard + calls as the "+" corner menu's "Add Audio Track".
            if (isProjectLoadInProgress())
            {
                return std::nullopt;
            }
            session.addTrack();
            syncViewportFromSession();
            trackLanesView.syncTracksFromSession();
            inspectorView_.refreshFromSession();
            return session.getActiveTrackId();
        };
        L.isStructuralEditBlocked = [this] { return trackLanesView.isStructuralTimelineEditBlocked(); };
        L.tailProjectLoadDiagnosticLog = [](const int maxLines) -> juce::StringArray {
            const juce::File log = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                       .getChildFile("MiniDAWLab")
                                       .getChildFile("project-load-diag.log");
            juce::StringArray all;
            if (log.existsAsFile())
            {
                all.addLines(log.loadFileAsString());
            }
            juce::StringArray tail;
            for (int i = juce::jmax(0, all.size() - maxLines); i < all.size(); ++i)
            {
                tail.add(all[i]);
            }
            return tail;
        };

        auto& D = hooks.dup;
        D.duplicateTrackLikeMenu = [this](const TrackId tid) -> TrackId {
            if (trackLanesEditCoordinator_ == nullptr)
            {
                return kInvalidTrackId;
            }
            const std::optional<TrackId> id = trackLanesEditCoordinator_->duplicateTrack(tid);
            return id.has_value() ? *id : kInvalidTrackId;
        };
        const auto describeRow = [this](const TrackId tid, const bool withIdentity) -> juce::String {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
            if (idx < 0)
            {
                return "track " + juce::String((juce::int64) tid) + ": not in snapshot";
            }
            const Track& tr = snap->getTrack(idx);
            juce::String kind;
            switch (tr.getKind())
            {
                case TrackKind::Audio: kind = "audio"; break;
                case TrackKind::Instrument: kind = "instrument"; break;
                case TrackKind::Group: kind = "group"; break;
                case TrackKind::Master: kind = "master"; break;
                case TrackKind::Midi: kind = "midi"; break;
            }
            juce::String s;
            if (withIdentity)
            {
                s << "id=" << juce::String((juce::int64) tid) << " index=" << idx << " name=\"" << tr.getName() << "\" ";
            }
            s << "kind=" << kind << " fader=" << juce::String(tr.getChannelFaderGain(), 4)
              << " preGainDb=" << juce::String(tr.getPreGainDb(), 2) << " pan=" << juce::String(tr.getStereoPan(), 3)
              << " muted=" << (tr.isMuted() ? 1 : 0) << " off=" << (tr.isTrackOff() ? 1 : 0)
              << " output=" << juce::String((juce::int64) tr.getRoutedOutputTrackId())
              << " midiTo=" << juce::String((juce::int64) tr.getMidiDestinationTrackId())
              << " midiOutCh=" << tr.getMidiOutputChannel();
            s << " sends=[";
            for (const TrackSend& snd : tr.getSends())
            {
                s << "(slot" << snd.uiSlotIndex << " dest=" << juce::String((juce::int64) snd.destTrackId)
                  << " amt=" << juce::String(snd.amountLinear, 4) << " on=" << (snd.enabled ? 1 : 0) << ")";
            }
            s << "]";
            const TrackInputAssignment& in = tr.getInputAssignment();
            s << " input=(kind=" << (int) in.kind << " chA=" << in.physicalChannelA << " chB=" << in.physicalChannelB << ")";
            const TrackMidiInputAssignment& min = tr.getMidiInputAssignment();
            s << " midiInput=(mode=" << (int) min.mode << " ch=" << min.channelFilter << " dev=\"" << min.deviceName << "\")";
            s << " clips=[";
            for (const PlacedClip& c : tr.getPlacedClips())
            {
                s << "(";
                if (withIdentity)
                {
                    s << "id=" << juce::String((juce::int64) c.getId()) << " ";
                }
                s << "start=" << juce::String((juce::int64) c.getStartSample())
                  << " len=" << juce::String((juce::int64) c.getEffectiveLengthSamples())
                  << " leftTrim=" << juce::String((juce::int64) c.getLeftTrimSamples())
                  << " window=" << juce::String((juce::int64) c.getMaterialWindowStartSamples()) << ".."
                  << juce::String((juce::int64) c.getMaterialWindowEndExclusiveSamples())
                  << " material=0x" << (c.getMaterial() != nullptr ? juce::String::toHexString((juce::int64) (std::intptr_t) c.getMaterial().get()) : juce::String("0"))
                  << ")";
            }
            s << "]";
            return s;
        };
        D.describeSessionRow = [describeRow](const TrackId tid) { return describeRow(tid, true); };
        D.describeSessionRowWithoutIdentity = [describeRow](const TrackId tid) { return describeRow(tid, false); };
        D.insertChainDigest = [this](const TrackId tid) -> juce::String {
            if (!pluginHost_.hasAnyInsertOnTrack(tid))
            {
                return {};
            }
            // "identity=<sha> state=<sha> sizes=<n,n,..> slots=N": identity covers stage / order /
            // path / identifier (must match on a copy); the state digest is byte-exact (retained
            // placeholder bytes must match; a LIVE plug-in may legitimately re-serialize its state
            // after a restore, so the runner compares that part only for placeholders).
            const PluginTrackChain chain = pluginHost_.exportChain(tid);
            juce::MemoryOutputStream identityInput;
            juce::MemoryOutputStream stateInput;
            juce::String sizes;
            int i = 0;
            for (const PluginInsertDescriptor& slot : chain.slots)
            {
                identityInput << i << "|" << (slot.stage == InsertStage::Pre ? "pre" : "post") << "|"
                              << (slot.occupied ? 1 : 0) << "|" << slot.vst3AbsolutePath << "|" << slot.pluginIdentifier << "\n";
                stateInput << i << "|" << (int) slot.opaqueState.getSize() << "|";
                stateInput.write(slot.opaqueState.getData(), slot.opaqueState.getSize());
                stateInput << "\n";
                sizes << (i > 0 ? "," : "") << (int) slot.opaqueState.getSize();
                ++i;
            }
            const juce::MemoryBlock idBytes = identityInput.getMemoryBlock();
            const juce::MemoryBlock stBytes = stateInput.getMemoryBlock();
            return "identity=" + juce::SHA256(idBytes.getData(), idBytes.getSize()).toHexString().substring(0, 16)
                   + " state=" + juce::SHA256(stBytes.getData(), stBytes.getSize()).toHexString().substring(0, 16)
                   + " sizes=" + sizes + " slots=" + juce::String((int) chain.slots.size());
        };
        D.insertInstancePointers = [this](const TrackId tid) -> juce::String {
            juce::String s;
            for (const auto& [chainTid, ptrs] : pluginHost_.exportChainInstancePointersForDiagnostics())
            {
                if (chainTid != tid)
                {
                    continue;
                }
                for (const void* p : ptrs)
                {
                    s << (s.isEmpty() ? "" : " ") << "0x" << juce::String::toHexString((juce::int64) (std::intptr_t) p);
                }
            }
            return s;
        };
        D.setInsertParam0 = [this](const TrackId tid, const int i, const float v) -> bool {
            juce::AudioPluginInstance* const inst = pluginHost_.liveInstanceAtChainIndexForDiagnostics(tid, i);
            if (inst == nullptr || inst->getParameters().isEmpty())
            {
                return false;
            }
            inst->getParameters()[0]->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, v));
            return true;
        };
        D.getInsertParam0 = [this](const TrackId tid, const int i) -> float {
            juce::AudioPluginInstance* const inst = pluginHost_.liveInstanceAtChainIndexForDiagnostics(tid, i);
            if (inst == nullptr || inst->getParameters().isEmpty())
            {
                return std::numeric_limits<float>::quiet_NaN();
            }
            return inst->getParameters()[0]->getValue();
        };
        D.insertParam0Name = [this](const TrackId tid, const int i) -> juce::String {
            juce::AudioPluginInstance* const inst = pluginHost_.liveInstanceAtChainIndexForDiagnostics(tid, i);
            if (inst == nullptr || inst->getParameters().isEmpty())
            {
                return {};
            }
            return inst->getParameters()[0]->getName(64) + " (" + juce::String(inst->getParameters().size()) + " params)";
        };
        D.addUnavailablePlaceholderInsert = [this](const TrackId tid, juce::String& failReason) -> bool {
            PluginTrackChain chain = pluginHost_.hasAnyInsertOnTrack(tid) ? pluginHost_.exportChain(tid) : PluginTrackChain{};
            PluginInsertDescriptor slot;
            slot.stage = InsertStage::Post;
            slot.occupied = true;
            slot.vst3AbsolutePath = "C:\\Program Files\\Common Files\\VST3\\DAL-NotInstalled-Test.vst3";
            slot.pluginIdentifier = "VST3-NotInstalled Test FX-0badc0de-4242"; // "<format>-<name>-<hash>-<uid>"
            const char payload[] = "dal-placeholder-state-bytes-1234567890";
            slot.opaqueState.replaceAll(payload, sizeof(payload));
            chain.slots.push_back(slot);
            pluginHost_.importChain(tid, chain);
            inspectorView_.refreshFromSession();
            for (const InsertRowView& row : pluginHost_.getInsertRowsForTrack(tid))
            {
                if (row.unavailable && row.displayName.contains("NotInstalled"))
                {
                    return true;
                }
            }
            failReason = "placeholder row not present after importChain";
            return false;
        };
        D.instrumentRuntimeIdentity = [this](const TrackId tid) -> juce::String {
            if (instrumentRuntimeCoordinator_ == nullptr)
            {
                return "none";
            }
            InstrumentTrackController* const ctl = instrumentRuntimeCoordinator_->getMidiClipControllerForTrack(tid);
            if (ctl == nullptr || !ctl->hasInstrumentTrack())
            {
                return "none";
            }
            const ExperimentalInstrumentHost* const host = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
            const ProjectFileExperimentalInstrumentTrackV1 row = ctl->buildExperimentalInstrumentProjectBlock();
            int notes = 0;
            for (const auto& c : row.clips)
            {
                notes += (int) c.timelineNotes.size();
            }
            juce::String s;
            s << "host=0x" << juce::String::toHexString((juce::int64) (std::intptr_t) host) << " kind=" << row.instrumentKind
              << " name=\"" << row.name << "\" loaded=" << (row.pluginWasLoadedOnSave ? 1 : 0)
              << " descriptor=" << (row.hasGenericVst3Descriptor ? row.genericVst3Descriptor.fileOrIdentifier : juce::String("-"))
              << " clips=" << (int) row.clips.size() << " notes=" << notes
              << " stateLen=" << row.pluginStateBase64.length()
              << " stateSha=" << (row.pluginStateBase64.isNotEmpty()
                                      ? juce::SHA256(row.pluginStateBase64.toRawUTF8(), (size_t) row.pluginStateBase64.getNumBytesAsUTF8()).toHexString().substring(0, 16)
                                      : juce::String("-"))
              << " secondary=" << (row.hasSecondary ? 1 : 0) << " proxy=" << (row.hasProxy ? 1 : 0)
              << " proxyGen=\"" << (row.hasProxy ? row.proxy.generationId : juce::String()) << "\""
              << " updateMode=" << row.proxyUpdateMode;
            return s;
        };
        D.setRoutedOutput = [this](const TrackId tid, const TrackId dest) -> bool {
            const bool ok = session.setTrackRoutedOutput(tid, dest);
            inspectorView_.refreshFromSession();
            return ok;
        };
        D.insertSend = [this](const TrackId tid, const int slot, const TrackId dest, const float amount) -> bool {
            const bool ok = session.insertTrackSend(tid, slot, dest, amount);
            inspectorView_.refreshFromSession();
            return ok;
        };
        D.isTrackArmed = [this](const TrackId tid) -> bool {
            return recorder_.getArmedTrackId() == tid
                   || (liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isRecordArmed(tid));
        };
        D.isTrackMonitored = [this](const TrackId tid) -> bool {
            return playbackEngine_.isTrackInputMonitoringEnabled(tid)
                   || (liveMidiInputCoordinator_ != nullptr && liveMidiInputCoordinator_->isMonitorEnabled(tid));
        };
        D.openHeaderMenuCaptureAndDismiss = [this](const TrackId tid, const juce::File& png, juce::String& detail) -> bool {
            juce::PopupMenu::dismissAllActiveMenus();
            if (!trackLanesView.openHeaderContextMenuForStabilityTest(tid))
            {
                detail = "header of track " + juce::String((juce::int64) tid) + " not laid out / no menu";
                return false;
            }
            // The popup is a JUCE desktop window of its own; snapshot every desktop component that
            // is not one of DAL's own top-level windows.
            juce::Component* menuWindow = nullptr;
            juce::Desktop& desktop = juce::Desktop::getInstance();
            for (int i = desktop.getNumComponents() - 1; i >= 0; --i)
            {
                juce::Component* const c = desktop.getComponent(i);
                if (c == nullptr || !c->isVisible() || c == getTopLevelComponent()
                    || dynamic_cast<juce::ResizableWindow*>(c) != nullptr)
                {
                    continue;
                }
                menuWindow = c;
                break;
            }
            if (menuWindow == nullptr)
            {
                detail = "no popup window found on the desktop";
                juce::PopupMenu::dismissAllActiveMenus();
                return false;
            }
            detail = "popup=" + menuWindow->getScreenBounds().toString();
            const juce::Image img = menuWindow->createComponentSnapshot(menuWindow->getLocalBounds(), true, 1.0f);
            bool ok = false;
            if (img.isValid())
            {
                png.getParentDirectory().createDirectory();
                juce::FileOutputStream out(png);
                if (out.openedOk())
                {
                    juce::PNGImageFormat fmt;
                    ok = fmt.writeImageToStream(img, out);
                }
            }
            juce::PopupMenu::dismissAllActiveMenus();
            return ok;
        };
        D.scrollBarDiagnostics = [this] { return arrangementVerticalScrollBarDiagnostics(); };
        D.verifyVerticalLayout = [this](juce::String& report, juce::String& failReason) -> bool {
            if (!trackLanesView.verifyVerticalScrollLayoutForDiagnostics(report, failReason))
            {
                return false;
            }
            const TrackLanesView::VerticalScrollModel m = trackLanesView.verticalScrollModel();
            const bool barShown = arrangementVerticalScrollBar_.isVisible();
            report << arrangementVerticalScrollBarDiagnostics() << "\n";
            if (barShown == m.everythingFits())
            {
                failReason = juce::String("scrollbar ") + (barShown ? "shown" : "hidden") + " although content "
                             + (m.everythingFits() ? "fits" : "exceeds") + " the viewport";
                return false;
            }
            if (!barShown)
            {
                return true;
            }
            if ((int) std::lround(arrangementVerticalScrollBar_.getMaximumRangeLimit()) != m.contentHeightPx
                || (int) std::lround(arrangementVerticalScrollBar_.getCurrentRangeSize()) != m.viewportHeightPx
                || (int) std::lround(arrangementVerticalScrollBar_.getCurrentRangeStart()) != m.offsetPx)
            {
                failReason = "scrollbar range does not mirror the lanes model";
                return false;
            }
            const juce::Rectangle<int> bar = arrangementVerticalScrollBar_.getBounds();
            const juce::Rectangle<int> lanes = trackLanesView.getBounds();
            if (bar.getX() < lanes.getRight() || bar.getY() != lanes.getY() + TrackLanesView::kArrangementTimelineHeaderGutterPx
                || bar.getBottom() != lanes.getBottom())
            {
                failReason = "scrollbar column " + bar.toString() + " does not sit right of the rows " + lanes.toString();
                return false;
            }
            if (rulerView.getRight() != lanes.getRight())
            {
                failReason = "ruler right edge " + juce::String(rulerView.getRight()) + " differs from lanes right edge "
                             + juce::String(lanes.getRight());
                return false;
            }
            return true;
        };
        D.scrollBarDragTo = [this](const int offsetPx) {
            arrangementVerticalScrollBar_.setCurrentRangeStart((double) offsetPx, juce::sendNotificationSync);
        };
        D.scrollBarPageClick = [this](const int pages) {
            arrangementVerticalScrollBar_.moveScrollbarInPages(pages, juce::sendNotificationSync);
        };
        D.wheelLanes = [this](const int notches) {
            juce::MouseWheelDetails wheel;
            wheel.deltaX = 0.0f;
            wheel.deltaY = -0.1f * (float) notches; // JUCE wheel notch ≈ 0.1; negative = scroll down
            wheel.isReversed = false;
            wheel.isSmooth = false;
            wheel.isInertial = false;
            const juce::Point<float> pos = trackLanesView.getLocalBounds().getCentre().toFloat();
            const juce::Time now = juce::Time::getCurrentTime();
            const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(),
                                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                     juce::MouseInputSource::defaultTiltY, &trackLanesView, &trackLanesView, now, pos, now, 0, false);
            trackLanesView.mouseWheelMove(e, wheel);
        };
        D.lanesScrollModel = [this]() -> juce::String {
            const TrackLanesView::VerticalScrollModel m = trackLanesView.verticalScrollModel();
            return "content=" + juce::String(m.contentHeightPx) + " viewport=" + juce::String(m.viewportHeightPx)
                   + " offset=" + juce::String(m.offsetPx) + " max=" + juce::String(m.maxOffsetPx());
        };
        D.visibleRowIndexForTrack = [this](const TrackId tid) { return trackLanesView.visibleRowIndexForTrackForDiagnostics(tid); };
        D.rowTopOffsetPxForTrack = [this](const TrackId tid) { return trackLanesView.rowTopOffsetPxForTrackForDiagnostics(tid); };
        D.setRowHeightPx = [this](const TrackId tid, const int px) { trackLanesView.setTrackRowHeightPxForStabilityTest(tid, px); };
        D.sessionTrackOrder = [this]() -> juce::String {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            juce::String s;
            if (snap == nullptr)
            {
                return s;
            }
            for (int i = 0; i < snap->getNumTracks(); ++i)
            {
                const Track& tr = snap->getTrack(i);
                s << (i > 0 ? "|" : "") << juce::String((juce::int64) tr.getId()) << ":" << tr.getName();
            }
            return s;
        };
    }

    /// [Message thread] `--stability-perf-profile` hooks: runtime description, device buffer
    /// request, loaded-vs-processed inventory, the opt-in audio profiler and the engine's load
    /// window. Diagnostics only — every reader here is the engine's / hosts' existing relaxed
    /// counter or the profiler's message-thread snapshot.
    void installPerfProfileStabilityHooks(StabilityRunnerHooks& hooks)
    {
        auto& P = hooks.perf;
        P.describeRuntime = [this]() -> juce::String {
            juce::String s;
            s << "exe=" << juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName()
              << " version=" << (juce::JUCEApplication::getInstance() != nullptr
                                     ? juce::JUCEApplication::getInstance()->getApplicationVersion()
                                     : juce::String("?"))
#if defined(NDEBUG)
              << " config=Release"
#else
              << " config=Debug"
#endif
              << " | cpu=\"" << juce::SystemStats::getCpuModel() << "\" physicalCores="
              << juce::SystemStats::getNumPhysicalCpus() << " logicalCores=" << juce::SystemStats::getNumCpus()
              << " ram=" << juce::SystemStats::getMemorySizeInMegabytes() << " MB"
              << " | " << mini_daw::describeActiveAudioDeviceOneLine(deviceManager);
            if (juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice())
            {
                s << " | offeredBufferSizes=";
                const juce::Array<int> sizes = dev->getAvailableBufferSizes();
                for (int i = 0; i < sizes.size(); ++i)
                {
                    s << (i > 0 ? "," : "") << sizes[i];
                }
                s << " default=" << dev->getDefaultBufferSize();
            }
            return s;
        };
        P.requestDeviceBufferSize = [this](const int requested, juce::String& detail) -> int {
            juce::AudioIODevice* const before = deviceManager.getCurrentAudioDevice();
            if (before == nullptr)
            {
                detail = "no active device";
                return 0;
            }
            const int was = before->getCurrentBufferSizeSamples();
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.bufferSize = requested;
            const juce::String err = deviceManager.setAudioDeviceSetup(setup, true);
            juce::AudioIODevice* const after = deviceManager.getCurrentAudioDevice();
            const int now = after != nullptr ? after->getCurrentBufferSizeSamples() : 0;
            detail = "was " + juce::String(was) + (err.isNotEmpty() ? " error: " + err : juce::String(" ok"))
                     + (now == requested ? " (applied)" : " (driver kept its own size)");
            return now;
        };
        P.describeProjectLoad = [this]() -> juce::String {
            juce::String s;
            const auto snap = session.loadSessionSnapshotForAudioThread();
            int audio = 0, instrument = 0, midi = 0, group = 0, master = 0, muted = 0, off = 0;
            for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                const Track& t = snap->getTrack(i);
                switch (t.getKind())
                {
                    case TrackKind::Audio: ++audio; break;
                    case TrackKind::Instrument: ++instrument; break;
                    case TrackKind::Midi: ++midi; break;
                    case TrackKind::Group: ++group; break;
                    case TrackKind::Master: ++master; break;
                }
                muted += t.isMuted() ? 1 : 0;
                off += t.isTrackOff() ? 1 : 0;
            }
            s << "tracks: audio=" << audio << " instrument=" << instrument << " midi=" << midi << " group=" << group
              << " master=" << master << " (muted=" << muted << " off=" << off << ")";
            int hostsLoaded = 0, hostsEmpty = 0, proxySelected = 0, secondaryLoaded = 0, registryEntries = 0;
            juce::String proxyStates;
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                for (const auto& [tid, hostPtr, ctlPtr] : instrumentRuntimeCoordinator_->exportKeyedRuntimePointersForDiagnostics())
                {
                    juce::ignoreUnused(hostPtr, ctlPtr);
                    ++registryEntries;
                    ExperimentalInstrumentHost* const h = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
                    if (h != nullptr && h->hasInstrument()) { ++hostsLoaded; } else { ++hostsEmpty; }
                    if (ExperimentalInstrumentHost* const sec = instrumentRuntimeCoordinator_->getSecondaryInstrumentHostForTrack(tid);
                        sec != nullptr && sec->hasInstrument())
                    {
                        ++secondaryLoaded;
                    }
                    if (proxyPlaybackCoordinator_ != nullptr)
                    {
                        const auto view = proxyPlaybackCoordinator_->publishedViewForTrack(tid);
                        if (view != nullptr && view->useProxy)
                        {
                            ++proxySelected;
                        }
                        const juce::String st = proxy_playback::proxyPlaybackSourceStateName(
                            proxyPlaybackCoordinator_->runtimeStateForTrack(tid));
                        proxyStates << (proxyStates.isEmpty() ? "" : ",") << juce::String((juce::int64)tid) << ":" << st;
                    }
                }
            }
            int insertInstances = 0, insertChains = 0;
            for (const auto& [tid, ptrs] : pluginHost_.exportChainInstancePointersForDiagnostics())
            {
                juce::ignoreUnused(tid);
                ++insertChains;
                insertInstances += (int)ptrs.size();
            }
            s << " | instrument registry entries=" << registryEntries << " primaryLoaded=" << hostsLoaded
              << " primaryEmpty=" << hostsEmpty << " proxySelected=" << proxySelected
              << " secondaryLoaded(notProcessedWhilePlaying)=" << secondaryLoaded
              << " | insert chains=" << insertChains << " instances=" << insertInstances
              << " | proxy states: " << proxyStates;
            return s;
        };
        P.setProfilerEnabled = [](const bool on) { audio_profiler::AudioThreadProfiler::get().setEnabled(on); };
        P.resetMeasurementWindows = [this] {
            (void)playbackEngine_.snapshotAudioCallbackLoadAndReset();
            (void)audio_profiler::AudioThreadProfiler::get().snapshotAndReset();
            (void)playbackEngine_.readAndResetOutputPeakHoldForDiagnostics();
            (void)playbackEngine_.drainMasterOutputLevelsForDiagnostics();
        };
        P.audioLoadText = [this]() -> juce::String {
            const auto a = playbackEngine_.snapshotAudioCallbackLoadAndReset();
            const double budgetMs = (a.sampleRate > 0.0 && a.lastBlockSamples > 0)
                                        ? 1000.0 * (double)a.lastBlockSamples / a.sampleRate
                                        : 0.0;
            juce::String s;
            s << "blocks=" << (juce::int64)a.blocks << " block=" << a.lastBlockSamples << " sr=" << juce::String(a.sampleRate, 0)
              << " budgetMs=" << juce::String(budgetMs, 3)
              << " ms(min/mean/max)=" << juce::String(a.minMs, 3) << "/" << juce::String(a.meanMs, 3) << "/" << juce::String(a.maxMs, 3)
              << " budget%(mean/max)=" << juce::String(a.meanBudgetPercent, 1) << "/" << juce::String(a.maxBudgetPercent, 1)
              << " nearOverruns(>70%)=" << (int)a.nearOverruns << " overruns(>=100%)=" << (int)a.overruns;
            return s;
        };
        P.profilerReportText = [this](const int topN) -> juce::String {
            using audio_profiler::AudioThreadProfiler;
            using audio_profiler::Category;
            const AudioThreadProfiler::Report r = AudioThreadProfiler::get().snapshotAndReset();
            const auto snap = session.loadSessionSnapshotForAudioThread();
            const auto trackLabel = [&snap](const TrackId tid) -> juce::String {
                const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
                return idx >= 0 ? snap->getTrack(idx).getName() : juce::String("?");
            };
            const double blocks = (double)juce::jmax<std::uint64_t>(1, r.blocks);
            juce::String s;
            s << "blocks=" << (juce::int64)r.blocks << " callback ms(mean/max)=" << juce::String(r.totalSumMs / blocks, 3) << "/"
              << juce::String(r.totalMaxMs, 3) << "\n";
            for (int c = 0; c < AudioThreadProfiler::kCategories; ++c)
            {
                const auto cat = static_cast<Category>(c);
                s << "category " << audio_profiler::categoryName(cat)
                  << ": perBlock ms(mean/max)=" << juce::String(r.categorySumMs[(size_t)c] / blocks, 3) << "/"
                  << juce::String(r.categoryMaxMs[(size_t)c], 3)
                  << " callsPerBlock=" << juce::String((double)r.categoryCalls[(size_t)c] / blocks, 1)
                  << " shareOfCallback=" << juce::String(r.totalSumMs > 0.0 ? 100.0 * r.categorySumMs[(size_t)c] / r.totalSumMs : 0.0, 1)
                  << "%\n";
            }
            s << "remainder (DAL work outside plug-in / proxy calls): perBlock ms(mean/max)="
              << juce::String(r.remainderSumMs / blocks, 3) << "/" << juce::String(r.remainderMaxMs, 3)
              << " shareOfCallback=" << juce::String(r.totalSumMs > 0.0 ? 100.0 * r.remainderSumMs / r.totalSumMs : 0.0, 1) << "%\n";
            for (int p = 0; p < AudioThreadProfiler::kPhases; ++p)
            {
                s << "phase " << audio_profiler::phaseName(static_cast<audio_profiler::Phase>(p))
                  << ": perBlock ms(mean/max)=" << juce::String(r.phaseSumMs[(size_t)p] / blocks, 3) << "/"
                  << juce::String(r.phaseMaxMs[(size_t)p], 3) << "\n";
            }
            s << "worst block: total=" << juce::String(r.worstTotalMs, 3) << " ms (" << r.worstBlockSamples << " samples):"
              << " instrument-plugin CPU " << juce::String(r.worstCategoryMs[0], 3)
              << " | proxy-mix " << juce::String(r.worstCategoryMs[1], 3)
              << " | insert-plugin " << juce::String(r.worstCategoryMs[2], 3)
              << " | generation-section wall " << juce::String(r.worstParallelWallMs, 3)
              << " | remainder " << juce::String(r.worstRemainderMs, 3) << "\n";
            const double parBlocks = (double)juce::jmax<std::uint64_t>(1, r.parallelBlocks);
            s << "generation section (dispatch -> join): wall perBlock ms(mean/max)=" << juce::String(r.parallelWallSumMs / blocks, 3)
              << "/" << juce::String(r.parallelWallMaxMs, 3)
              << " callbackIdleWait ms(mean/max)=" << juce::String(r.parallelWaitSumMs / blocks, 3) << "/" << juce::String(r.parallelWaitMaxMs, 3)
              << " jobsPerBlock=" << juce::String((double)r.parallelJobs / blocks, 1)
              << " parallelBlocks=" << (juce::int64)r.parallelBlocks << " of " << (juce::int64)r.blocks
              << " effectiveParallelism(instrumentCPU/sectionWall)="
              << juce::String(r.parallelWallSumMs > 0.0 ? r.categorySumMs[0] / r.parallelWallSumMs : 0.0, 2)
              << " (parallel blocks only: " << juce::String(r.parallelBlocks > 0 ? (r.categorySumMs[0] / blocks) / (r.parallelWallSumMs / parBlocks) : 0.0, 2) << ")\n";
            s << "NOTE: category instrument-plugin is SUMMED CPU time across render threads; the callback's elapsed time for that work is the generation-section wall.\n";
            s << "callback start intervals: n=" << (juce::int64)r.intervals
              << " ms(mean/max)=" << juce::String(r.intervals > 0 ? r.intervalSumMs / (double)r.intervals : 0.0, 3) << "/"
              << juce::String(r.intervalMaxMs, 3) << " lateStarts(>1.25x period)=" << (int)r.lateStarts << "\n";
            std::vector<AudioThreadProfiler::InstanceReport> inst = r.instances;
            std::sort(inst.begin(), inst.end(), [](const auto& a, const auto& b) { return a.totalMs() > b.totalMs(); });
            int shown = 0;
            double shownSum = 0.0;
            int active = 0;
            for (const auto& ir : inst)
            {
                if (ir.totalCalls() > 0)
                {
                    ++active;
                }
            }
            s << "instances with calls=" << active << " of registered " << (int)inst.size() << "; top " << topN << " by summed ms:\n";
            for (const auto& ir : inst)
            {
                if (shown >= topN || ir.totalCalls() == 0)
                {
                    break;
                }
                ++shown;
                shownSum += ir.totalMs();
                const int c = ir.calls[0] > 0 ? 0 : (ir.calls[1] > 0 ? 1 : 2);
                s << "  #" << shown << " track " << (juce::int64)ir.trackId << " \"" << trackLabel(ir.trackId) << "\""
                  << (ir.category == Category::InsertPlugin ? " insert slot " + juce::String((juce::int64)ir.slotId) : juce::String(""))
                  << " plugin \"" << ir.name << "\" mode=" << audio_profiler::categoryName(static_cast<Category>(c))
                  << " calls=" << (juce::int64)ir.totalCalls()
                  << " meanPerCall=" << juce::String(1000.0 * ir.totalMs() / (double)ir.totalCalls(), 1) << " us"
                  << " maxCall=" << juce::String(juce::jmax(ir.maxMs[0], juce::jmax(ir.maxMs[1], ir.maxMs[2])), 3) << " ms"
                  << " perBlock=" << juce::String(ir.totalMs() / blocks, 3) << " ms"
                  << " share=" << juce::String(r.totalSumMs > 0.0 ? 100.0 * ir.totalMs() / r.totalSumMs : 0.0, 1) << "%\n";
            }
            s << "top " << shown << " together = " << juce::String(r.totalSumMs > 0.0 ? 100.0 * shownSum / r.totalSumMs : 0.0, 1)
              << "% of callback time\n";
            // Per plug-in family totals (same name across instances) — where the budget goes by product.
            std::map<juce::String, std::pair<int, double>> families;
            for (const auto& ir : inst)
            {
                if (ir.totalCalls() == 0)
                {
                    continue;
                }
                auto& f = families[ir.name];
                f.first += 1;
                f.second += ir.totalMs();
            }
            std::vector<std::pair<juce::String, std::pair<int, double>>> fam(families.begin(), families.end());
            std::sort(fam.begin(), fam.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
            s << "by plug-in (instances, perBlock ms, share):";
            for (const auto& f : fam)
            {
                s << " [" << f.first << " x" << f.second.first << " " << juce::String(f.second.second / blocks, 3) << " ms "
                  << juce::String(r.totalSumMs > 0.0 ? 100.0 * f.second.second / r.totalSumMs : 0.0, 1) << "%]";
            }
            return s;
        };
        P.proxyUnderrunTotal = [this](int& proxySelectedCount) -> std::int64_t {
            proxySelectedCount = 0;
            std::int64_t total = 0;
            if (instrumentRuntimeCoordinator_ == nullptr || proxyPlaybackCoordinator_ == nullptr)
            {
                return 0;
            }
            for (const auto& [tid, hostPtr, ctlPtr] : instrumentRuntimeCoordinator_->exportKeyedRuntimePointersForDiagnostics())
            {
                juce::ignoreUnused(hostPtr, ctlPtr);
                const auto view = proxyPlaybackCoordinator_->publishedViewForTrack(tid);
                if (view == nullptr)
                {
                    continue;
                }
                if (view->useProxy)
                {
                    ++proxySelectedCount;
                }
                if (view->reader != nullptr)
                {
                    total += (std::int64_t)view->reader->underrunCount();
                }
            }
            return total;
        };
        P.renderPoolText = [this]() -> juce::String {
            const auto st = playbackEngine_.instrumentRenderPoolStats();
            juce::String s;
            s << "workers=" << playbackEngine_.instrumentRenderWorkerCount()
              << " (threads incl. callback=" << (playbackEngine_.instrumentRenderWorkerCount() + 1) << ")"
              << " serialForced=" << (playbackEngine_.isInstrumentRenderSerialForDiagnostics() ? "yes" : "no")
              << " parallelBlocks=" << (juce::int64)st.parallelBlocks << " serialBlocks=" << (juce::int64)st.serialBlocks
              << " jobsByCallback=" << (juce::int64)st.jobsRunByCallback << " jobsByWorkers=" << (juce::int64)st.jobsRunByWorkers
              << " joinKernelWaits=" << (juce::int64)st.joinWaits
              << " lastDispatchWallMs=" << juce::String(st.lastDispatchWallMs, 3)
              << " lastJoinWaitMs=" << juce::String(st.lastJoinWaitMs, 3)
              << " (cumulative since start)";
            return s;
        };
        P.setGenerationSerial = [this](const bool serial) { playbackEngine_.setInstrumentRenderSerialForDiagnostics(serial); };
        P.readAheadText = [this]() -> juce::String {
            // Diagnostics only (docs/READAHEAD_PROTOTYPE.md §10): cumulative renderer counters plus a
            // message-thread poll of which audio rows the renderer owns right now. The row-state
            // reads are relaxed atomics; a row adopted or released during the walk may be listed
            // one way or the other — fine for a log line, never used for any decision.
            readahead::ReadAheadRenderer* const ra = playbackEngine_.experimentalReadAhead();
            if (ra == nullptr)
            {
                return "disabled (no --experimental-readahead)";
            }
            const auto c = ra->countersSnapshot();
            juce::String s;
            s << "depth=" << ra->depthBlocks()
              << " counters(cumulative): adopted=" << (juce::int64)c.adopted
              << " produced=" << (juce::int64)c.producedSegments
              << " consumed=" << (juce::int64)c.consumedSegments
              << " missed=" << (juce::int64)c.missedSegments
              << " staleDiscarded=" << (juce::int64)c.staleDiscarded
              << " drainReleases=" << (juce::int64)c.drainReleases
              << " discardResets=" << (juce::int64)c.discardResets
              << " missAbandons=" << (juce::int64)c.missAbandons;
            const auto snap = session.loadSessionSnapshotForAudioThread();
            int audioRows = 0, owned = 0;
            juce::String rows;
            for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                const Track& t = snap->getTrack(i);
                if (t.getKind() != TrackKind::Audio)
                {
                    continue;
                }
                ++audioRows;
                if (!ra->audioThread_isOwnedForRender(t.getId()))
                {
                    continue;
                }
                ++owned;
                rows << (rows.isEmpty() ? "" : "; ") << "#" << i << " id=" << (juce::int64)t.getId()
                     << " \"" << t.getName() << "\" inserts=[";
                bool first = true;
                for (const InsertRowView& r : pluginHost_.getInsertRowsForTrack(t.getId()))
                {
                    rows << (first ? "" : ", ") << (r.stage == InsertStage::Pre ? "Pre:" : "Post:") << r.displayName
                         << (r.unavailable ? "(unavailable)" : "");
                    first = false;
                }
                rows << "]";
            }
            s << " | ownedNow=" << owned << " of audioRows=" << audioRows
              << " (cap " << readahead::ReadAheadRenderer::kMaxRows << ")";
            if (rows.isNotEmpty())
            {
                s << " | " << rows;
            }
            return s;
        };
        P.readAheadCounters = [this]() -> StabilityRunnerHooks::PerfHooks::ReadAheadCountersView {
            StabilityRunnerHooks::PerfHooks::ReadAheadCountersView v;
            readahead::ReadAheadRenderer* const ra = playbackEngine_.experimentalReadAhead();
            if (ra == nullptr)
            {
                return v;
            }
            const auto c = ra->countersSnapshot();
            v.enabled = true;
            v.depth = ra->depthBlocks();
            v.adopted = c.adopted;
            v.produced = c.producedSegments;
            v.consumed = c.consumedSegments;
            v.missed = c.missedSegments;
            v.staleDiscarded = c.staleDiscarded;
            v.drainReleases = c.drainReleases;
            v.discardResets = c.discardResets;
            v.missAbandons = c.missAbandons;
            return v;
        };
        P.readAheadOwnedTrackIds = [this]() -> std::vector<TrackId> {
            std::vector<TrackId> ids;
            readahead::ReadAheadRenderer* const ra = playbackEngine_.experimentalReadAhead();
            const auto snap = session.loadSessionSnapshotForAudioThread();
            for (int i = 0; ra != nullptr && snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                const Track& t = snap->getTrack(i);
                if (t.getKind() == TrackKind::Audio && ra->audioThread_isOwnedForRender(t.getId()))
                {
                    ids.push_back(t.getId());
                }
            }
            return ids;
        };
        P.instrumentActivityText = [this]() -> juce::String {
            int live = 0, proxied = 0, idle = 0;
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                for (const auto& [tid, hostPtr, ctlPtr] : instrumentRuntimeCoordinator_->exportKeyedRuntimePointersForDiagnostics())
                {
                    juce::ignoreUnused(hostPtr, ctlPtr);
                    ExperimentalInstrumentHost* const h = instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
                    if (h == nullptr)
                    {
                        continue;
                    }
                    const bool l = h->readRtActivitySnapshotForDiagnostics().processOkBlocks > 0;
                    const bool p = h->getProxyBlocksMixedCountRelaxed() > 0;
                    live += l ? 1 : 0;
                    proxied += p ? 1 : 0;
                    idle += (!l && !p) ? 1 : 0;
                }
            }
            return "hosts with live processBlock blocks=" + juce::String(live) + " hosts with proxy-mixed blocks="
                   + juce::String(proxied) + " hosts with neither=" + juce::String(idle)
                   + " (cumulative since device start; a host can appear in both after a source switch)";
        };
    }

    void installMixerStabilityHooks(StabilityRunnerHooks& hooks)
    {
        using track_strip_glyphs::StripButtonKind;
        auto& M = hooks.mixer;
        const auto strip = [this](const TrackId tid) -> MixerChannelStrip* {
            return mixerWindow_ != nullptr ? mixerWindow_->content().stripForTrack(tid) : nullptr;
        };
        const auto buttonKind = [](const juce::String& name, StripButtonKind& out) -> bool {
            if (name == "power") { out = StripButtonKind::Power; return true; }
            if (name == "mute") { out = StripButtonKind::Mute; return true; }
            if (name == "monitor") { out = StripButtonKind::Monitor; return true; }
            if (name == "arm") { out = StripButtonKind::Arm; return true; }
            if (name == "editor") { out = StripButtonKind::InstrumentEditor; return true; }
            if (name == "alternatives") { out = StripButtonKind::Alternatives; return true; }
            return false;
        };
        const auto sectionFromKey = [](const juce::String& key, mixer_layout::Section& out) -> bool {
            for (int i = 0; i < mixer_layout::kSectionCount; ++i)
            {
                const auto s = static_cast<mixer_layout::Section>(i);
                if (key == mixer_layout::sectionKey(s))
                {
                    out = s;
                    return true;
                }
            }
            return false;
        };
        M.toggleLikeF3 = [this] { invokeToggleMixerWindowFromWindowShortcut(); };
        M.isVisible = [this] { return mixerWindow_ != nullptr && mixerWindow_->isVisible(); };
        M.addGroupTrackLikeUi = [this]() -> TrackId {
            // Same sequence as the add-track menu's "Group" entry.
            session.addGroupTrack();
            syncViewportFromSession();
            trackLanesView.syncTracksFromSession();
            inspectorView_.refreshFromSession();
            const auto snap = session.loadSessionSnapshotForAudioThread();
            TrackId newest = kInvalidTrackId;
            for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                const Track& t = snap->getTrack(i);
                if (t.getKind() == TrackKind::Group && t.getId() > newest)
                {
                    newest = t.getId();
                }
            }
            return newest;
        };
        M.windowInstanceCount = [] {
            int n = 0;
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents(); ++i)
            {
                if (auto* dw = dynamic_cast<juce::DocumentWindow*>(desktop.getComponent(i)))
                {
                    n += dw->getName() == "Mixer" ? 1 : 0;
                }
            }
            return n;
        };
        M.capturePng = [this](const juce::File& png) -> bool {
            if (mixerWindow_ == nullptr)
            {
                return false;
            }
            MixerContentComponent& c = mixerWindow_->content();
            const juce::Image img = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
            if (!img.isValid())
            {
                return false;
            }
            (void)png.deleteFile();
            juce::FileOutputStream out(png);
            if (!out.openedOk())
            {
                return false;
            }
            juce::PNGImageFormat fmt;
            return fmt.writeImageToStream(img, out);
        };
        M.verifyLayout = [this](juce::String& report, juce::String& failReason) -> bool {
            if (mixerWindow_ == nullptr)
            {
                failReason = "no mixer window";
                return false;
            }
            report << "  mixer window " << mixerWindow_->getBounds().toString() << " visible=" << (mixerWindow_->isVisible() ? "yes" : "no") << "\n";
            return mixerWindow_->content().verifyLayout(report, failReason);
        };
        M.stripOrder = [this] { return mixerWindow_ != nullptr ? mixerWindow_->content().stripOrder() : std::vector<TrackId>{}; };
        M.stripKindTexts = [this]() -> juce::String {
            juce::String s;
            if (mixerWindow_ == nullptr)
            {
                return s;
            }
            for (const TrackId id : mixerWindow_->content().stripOrder())
            {
                if (auto* st = mixerWindow_->content().stripForTrack(id))
                {
                    s << juce::String((juce::int64)id) << ":" << st->kindText() << "(" << st->nameText() << ") ";
                }
            }
            if (auto* m = mixerWindow_->content().masterStrip())
            {
                s << "| master " << juce::String((juce::int64)m->trackId()) << ":" << m->kindText();
            }
            return s;
        };
        M.masterStripScreenBounds = [this] { return mixerWindow_ != nullptr ? mixerWindow_->content().masterStripScreenBounds() : juce::Rectangle<int>(); };
        M.scrollStripsToRight = [this]() -> int {
            if (mixerWindow_ == nullptr)
            {
                return 0;
            }
            juce::Viewport& vp = mixerWindow_->content().stripsViewport();
            const int maxX = juce::jmax(0, vp.getViewedComponent()->getWidth() - vp.getViewWidth());
            vp.setViewPosition(maxX, vp.getViewPositionY());
            return vp.getViewPositionX();
        };
        M.clickSectionToggle = [this, sectionFromKey](const juce::String& key) {
            mixer_layout::Section s;
            if (mixerWindow_ != nullptr && sectionFromKey(key, s))
            {
                mixerWindow_->content().clickSectionToggleForTest(s);
            }
        };
        M.sectionShown = [this, sectionFromKey](const juce::String& key) -> bool {
            mixer_layout::Section s;
            return mixerWindow_ != nullptr && sectionFromKey(key, s) && mixerWindow_->content().sectionVisibility().get(s);
        };
        M.stripFaderType = [strip](const TrackId tid, const juce::String& text) -> bool {
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr)
            {
                return false;
            }
            s->fader().commitTypedValue(text);
            return true;
        };
        M.stripFaderValueText = [strip](const TrackId tid) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->fader().getValueFieldText() : juce::String();
        };
        M.stripPanSet = [strip](const TrackId tid, const float pan) -> bool {
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr)
            {
                return false;
            }
            s->pan().setPan(pan, juce::sendNotificationSync);
            return true;
        };
        M.stripChooseRouting = [strip](const TrackId tid, const int row, const juce::String& text) -> bool {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->chooseRoutingByText(row, text);
        };
        M.stripRoutingText = [strip](const TrackId tid, const int row) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->routingText(row) : juce::String();
        };
        M.stripRoutingCaption = [strip](const TrackId tid, const int row) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->routingCaption(row) : juce::String();
        };
        M.stripChooseSendDestination = [strip](const TrackId tid, const int row, const juce::String& text) -> bool {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->chooseSendDestinationByText(row, text);
        };
        M.stripSendDestinationText = [strip](const TrackId tid, const int row) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->sendDestinationText(row) : juce::String();
        };
        M.stripCommitSendAmount = [strip](const TrackId tid, const int row, const juce::String& text) -> bool {
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr)
            {
                return false;
            }
            s->commitSendAmountText(row, text);
            return true;
        };
        M.stripSendAmountText = [strip](const TrackId tid, const int row) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->sendAmountText(row) : juce::String();
        };
        M.stripCommitPreGain = [strip](const TrackId tid, const juce::String& text) -> bool {
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr)
            {
                return false;
            }
            s->commitPreGainText(text);
            return true;
        };
        M.stripPreGainText = [strip](const TrackId tid) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->preGainText() : juce::String();
        };
        M.stripInsertRowsText = [strip](const TrackId tid) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            juce::String out;
            if (s == nullptr)
            {
                return out;
            }
            for (const InsertStage st : { InsertStage::Pre, InsertStage::Post })
            {
                for (int r = 0; r < s->insertRowCount(st); ++r)
                {
                    out << (st == InsertStage::Pre ? "Pre:" : "Post:") << s->insertRowText(st, r) << " ";
                }
            }
            return out;
        };
        M.stripInsertRowCount = [strip](const TrackId tid, const bool preStage) -> int {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->insertRowCount(preStage ? InsertStage::Pre : InsertStage::Post) : 0;
        };
        M.stripVisibleInsertRowCount = [strip](const TrackId tid, const bool preStage) -> int {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->visibleInsertRowCount(preStage ? InsertStage::Pre : InsertStage::Post) : 0;
        };
        M.stripInsertListScrollable = [strip](const TrackId tid, const bool preStage) -> bool {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->isInsertListScrollable(preStage ? InsertStage::Pre : InsertStage::Post);
        };
        M.stripScrollInsertListToRow = [strip](const TrackId tid, const bool preStage, const int row) -> bool {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->scrollInsertListToRow(preStage ? InsertStage::Pre : InsertStage::Post, row);
        };
        M.stripInsertListAndAddBounds = [strip](const TrackId tid, const bool preStage) -> juce::String {
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr)
            {
                return {};
            }
            const InsertStage st = preStage ? InsertStage::Pre : InsertStage::Post;
            return "list " + s->insertListBounds(st).toString() + " add " + s->insertAddButtonBounds(st).toString();
        };
        M.dividerCount = [this] { return mixerWindow_ != nullptr ? (int)mixerWindow_->content().currentDividers().size() : 0; };
        M.resetSectionHeights = [this] {
            if (mixerWindow_ != nullptr)
            {
                mixerWindow_->content().setSectionHeights(mixer_layout::SectionHeights{});
                if (mixerWindow_->content().onSectionHeightsChanged)
                {
                    mixerWindow_->content().onSectionHeightsChanged(mixerWindow_->content().sectionHeights());
                }
            }
        };
        M.dragDivider = [this](const int index, const int deltaY) {
            return mixerWindow_ != nullptr && mixerWindow_->content().dragDividerForTest(index, deltaY);
        };
        M.sectionHeightsText = [this]() -> juce::String {
            juce::String s;
            if (mixerWindow_ == nullptr)
            {
                return s;
            }
            const auto& h = mixerWindow_->content().sectionHeights();
            for (int i = 0; i < mixer_layout::kSectionCount; ++i)
            {
                const auto sec = static_cast<mixer_layout::Section>(i);
                if (mixer_layout::isUpperSection(sec))
                {
                    s << mixer_layout::sectionKey(sec) << "=" << h.get(sec) << " ";
                }
            }
            s << "strip=" << mixerWindow_->content().currentStripHeight();
            return s;
        };
        M.stripPanValue = [strip](const TrackId tid) -> float {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr ? s->pan().getPan() : 0.0f;
        };
        M.inspectorPanValue = [this] { return inspectorView_.getPanValueForStabilityTest(); };
        M.stripPanDragLikeMouse = [strip](const TrackId tid, const float targetPan) -> bool {
            // Handler-level mouse gesture on the strip's OWN pan control: press on the stick, drag
            // to the x that maps to `targetPan`, release — the same code path a real pointer runs.
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr || !s->pan().isVisible())
            {
                return false;
            }
            InspectorPanControl& pan = s->pan();
            const juce::Rectangle<int> hit = pan.getMarkerHitRectForTest();
            if (hit.isEmpty())
            {
                return false;
            }
            const auto now = juce::Time::getCurrentTime();
            const auto& src = juce::Desktop::getInstance().getMainMouseSource();
            const juce::Point<float> down = hit.getCentre().toFloat();
            const juce::MouseEvent downEvent(src, down, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &pan, &pan, now, down, now, 1, false);
            pan.mouseDown(downEvent);
            const juce::Point<float> to(pan.xForPanForTest(targetPan), down.y);
            const juce::MouseEvent dragEvent(src, to, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &pan, &pan, now, down, now, 1, true);
            pan.mouseDrag(dragEvent);
            const juce::MouseEvent upEvent(src, to, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &pan, &pan, now, down, now, 1, false);
            pan.mouseUp(upEvent);
            return true;
        };
        M.stripInsertMenuAction = [strip](const TrackId tid, const bool preStage, const int row, const int actionId) -> bool {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->performInsertRowAction(preStage ? InsertStage::Pre : InsertStage::Post, row, actionId);
        };
        M.stripClickButton = [strip, buttonKind](const TrackId tid, const juce::String& button) -> bool {
            StripButtonKind kind;
            MixerChannelStrip* const s = strip(tid);
            if (s == nullptr || !buttonKind(button, kind) || !s->baseButtonVisible(kind))
            {
                return false;
            }
            s->clickBaseButtonForTest(kind);
            return true;
        };
        M.stripButtonActive = [strip, buttonKind](const TrackId tid, const juce::String& button) -> bool {
            StripButtonKind kind;
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && buttonKind(button, kind) && s->baseButtonActive(kind);
        };
        M.stripButtonVisible = [strip, buttonKind](const TrackId tid, const juce::String& button) -> bool {
            StripButtonKind kind;
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && buttonKind(button, kind) && s->baseButtonVisible(kind);
        };
        M.stripMeterHeldPeak = [strip](const TrackId tid) -> float {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->meter().isVisible() ? s->meter().getHeldPeakLinear() : 0.0f;
        };
        M.stripMeterOverloadLatched = [strip](const TrackId tid) -> bool {
            MixerChannelStrip* const s = strip(tid);
            return s != nullptr && s->meter().isOverloadLatched();
        };
        M.stripClickMeter = [strip](const TrackId tid) {
            if (MixerChannelStrip* const s = strip(tid))
            {
                s->meter().resetOverloadLatch(); // same as a lamp click: notifies the hub
            }
        };
        M.inspectorMeterHeldPeak = [this] { return inspectorPanel_.channelPanel().outputMeter().getHeldPeakLinear(); };
        M.inspectorMeterOverloadLatched = [this] { return inspectorPanel_.channelPanel().outputMeter().isOverloadLatched(); };
        M.meterHubInterest = [this] { return levelMeterHub_.publishedInterest(); };
        M.windowBounds = [this] { return mixerWindow_ != nullptr ? mixerWindow_->getBounds() : juce::Rectangle<int>(); };
        M.setWindowBounds = [this](const juce::Rectangle<int> b) {
            if (mixerWindow_ != nullptr)
            {
                mixerWindow_->setBounds(b);
            }
        };
        M.audioRuntimeFingerprint = [this]() -> juce::String {
            juce::String s;
            s << "intent=" << static_cast<int>(transport.readPlaybackIntentForUi());
            const auto load = playbackEngine_.snapshotAudioCallbackLoadAndReset();
            juce::ignoreUnused(load); // the window is reset so the NEXT read measures the toggling period
            s << " chains=";
            for (const auto& [tid, ptrs] : pluginHost_.exportChainInstancePointersForDiagnostics())
            {
                s << juce::String((juce::int64)tid) << ":" << juce::String((int)ptrs.size()) << ",";
            }
            if (instrumentRuntimeCoordinator_ != nullptr)
            {
                s << " instruments=";
                for (const auto& [tid, host, ctl] : instrumentRuntimeCoordinator_->exportKeyedRuntimePointersForDiagnostics())
                {
                    s << juce::String((juce::int64)tid) << ":" << juce::String::toHexString((juce::pointer_sized_int)host) << "/"
                      << juce::String::toHexString((juce::pointer_sized_int)ctl) << ",";
                }
            }
            return s;
        };
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

    /// "A take is running" for every edit/undo/autosave guard: the audio recorder OR a live MIDI
    /// take (a MIDI-only take never starts the audio recorder). Safe during construction.
    [[nodiscard]] bool anyRecordingInProgress() const noexcept
    {
        return recorder_.isRecording()
               || (recordingCoordinator_ != nullptr && recordingCoordinator_->isMidiTakeActive());
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
    /// Diagnostics (`--stability-proxy-render-probe`): one captured request rendered repeatedly
    /// through the production engine seam with the artifacts kept. Test mode only; the worker is
    /// joined and the isolated instance torn down on the message thread in `finish`.
    struct ProxyRenderProbeState
    {
        TrackId destination = kInvalidTrackId;
        juce::File outDir;
        juce::String build;
        std::unique_ptr<proxy_render::ProxyCapturedRequest> captured;
        std::unique_ptr<proxy_render::ProxyPreparedJob> prepared;
        struct Worker final : juce::Thread
        {
            Worker(proxy_render::ProxyPreparedJob& j, std::atomic<std::int64_t>& p)
                : juce::Thread("ProxyRenderProbeWorker"), job(j), progress(p) {}
            ~Worker() override { stopThread(30000); }
            void run() override
            {
                startedMs = juce::Time::getMillisecondCounterHiRes();
                result = job.render(token, {}, progress);
                endedMs = juce::Time::getMillisecondCounterHiRes();
                done.store(true, std::memory_order_release);
            }
            proxy_render::ProxyPreparedJob& job;
            std::atomic<std::int64_t>& progress;
            proxy_render::ProxyRenderCancellationToken token;
            proxy_render::ProxyRenderResult result;
            std::atomic<bool> done{ false };
            double startedMs = 0.0, endedMs = 0.0;
        };
        std::unique_ptr<Worker> worker;
        std::atomic<std::int64_t> progressMs{ 0 };
        proxy_render::ProxyRenderResult lastResult;
        bool hasResult = false;
        double prepareStartMs = 0.0, preparedMs = 0.0;
        int renderIndex = 0;
    };
    ProxyRenderProbeState proxyRenderProbe_;
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
    /// Live MIDI input (devices, Monitor/Arm runtime flags, routing, take capture). Declared after
    /// the instrument runtime so it is destroyed first: its destructor detaches the engine bus
    /// and unregisters the device callbacks while hosts and controllers still exist.
    std::unique_ptr<LiveMidiInputCoordinator> liveMidiInputCoordinator_;

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
    /// Solo commands + derived-view publish (S buttons, memory strip, mute lock, proxy refusal).
    /// Declared after the undo/proxy coordinators it talks to (all access is null-checked).
    std::unique_ptr<SoloCoordinator> soloCoordinator_;
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
    /// Spec §3: the four Solo-memory buttons above the track-header column (main window only).
    SoloMemoryStrip soloMemoryStrip_;
    /// Shared track heights: one-shot Small/Medium/Large commands; shows "Custom" (status only,
    /// never a selectable item) while row heights are mixed. Placed between the Solo memory strip
    /// and the edit tool strip by `applyTransportControlsLayout`.
    /// Item id = `TrackRowHeightPreset` value + 1 (Micro 1, Mini 2, Small 3, Medium 4, Large 5).
    [[nodiscard]] static constexpr int trackRowHeightComboIdForPreset(const track_row_heights::TrackRowHeightPreset p) noexcept
    {
        return static_cast<int>(p) + 1;
    }
    juce::ComboBox trackRowHeightPresetCombo_;
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
    /// Vertical arrangement scrollbar (see the ctor comment); bound to the lanes' scroll model.
    juce::ScrollBar arrangementVerticalScrollBar_{ true };
    struct ArrangementVerticalScrollBarListener final : juce::ScrollBar::Listener
    {
        explicit ArrangementVerticalScrollBarListener(TransportControlsContent& o) noexcept : owner(o) {}
        void scrollBarMoved(juce::ScrollBar*, const double newRangeStart) override
        {
            // Thumb drag / trough page click / arrow step: ONE clamp + layout path, same as the wheel.
            owner.trackLanesView.scrollVerticallyToOffsetPx((int) std::lround(newRangeStart));
        }
        TransportControlsContent& owner;
    } arrangementVerticalScrollBarListener_{ *this };
    /// The overlay's most recent per-frame display position (session samples; NaN before the
    /// first frame). The MIDI lanes' running-take preview reads it so its right edge and the
    /// playhead line come from one value — the overlay stays the only clock sampler.
    double lastPlayheadFrameDisplaySamples_ = std::numeric_limits<double>::quiet_NaN();
    /// Level-meter hub (30 Hz drain of the engine's per-row accumulators, fan-out to the Inspector
    /// channel panel and the mixer strips). Declared BEFORE the views that subscribe to it so it
    /// outlives them (they unsubscribe in their destructors).
    LevelMeterHub levelMeterHub_;
    /// Inspector column: scrollable `InspectorView` + fixed `ChannelStripPanel`. `inspectorView_`
    /// is a reference into the panel so the many existing call sites stay unchanged.
    InspectorPanel inspectorPanel_;
    InspectorView& inspectorView_;
    /// P2: shared seams for the track-header "Instrument alternatives" popup (wired once in the
    /// ctor; the popup copies them per launch, so an open callout stays safe across relaunches).
    InstrumentProxyUiHost instrumentProxyUiHost_;
    InstrumentSecondaryUiHost instrumentSecondaryUiHost_;
    /// App-wide tooltip host (required for TooltipClient texts, e.g. the header alternatives cell).
    juce::TooltipWindow tooltipWindow_{ nullptr, 700 };
    collapsible_side_strip::ResizeSplitter inspectorResizeSplitter_;
    collapsible_side_strip::CollapsedKnob inspectorCollapsedKnob_;
    int inspectorCurrentWidth_ = kInspectorDefaultW;
    /// App-wide (machine-local) layout preferences: the shared track-header column width. Loaded in
    /// the ctor before the first layout; written once per completed boundary drag.
    UiLayoutSettingsStore uiLayoutSettings_{ UiLayoutSettingsStore::defaultFile() };

    /// Destroyed before `trackLanesView` / `instrumentRuntimeCoordinator_` reverse dtors run (non-owning refs).
    std::unique_ptr<ArrangementEventSelectionCoordinator> arrangementEventSelectionCoordinator_;

    /// Declared after `trackLanesView`, `rulerView`, `inspectorView_` so it is destroyed before
    /// them in reverse-declaration order, while every UI object it borrows is still alive. See
    /// `TrackLanesEditCoordinator` ctor — it stores `&` to those views.
    std::unique_ptr<TrackLanesEditCoordinator> trackLanesEditCoordinator_;

    /// Mixer window + the explicit-TrackId bindings its strips call. Declared LAST: the bindings
    /// hold copies of the edit coordinator's actions and lambdas into every coordinator above,
    /// so the window (and its strips, which also unsubscribe from the hub) must die first.
    MixerStripBindings mixerBindings_;
    std::unique_ptr<MixerWindow> mixerWindow_;

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

    /// Live-MIDI scenario: event-level record of everything the fixture destination received at
    /// its processing boundary (RT-safe: fixed array + atomic count; read after the fact).
    struct StabilityLiveMidiCaptureSink final : ExperimentalInstrumentHost::MidiDeliveryCaptureSink
    {
        struct Rec
        {
            std::uint8_t status = 0;
            std::uint8_t d1 = 0;
            std::uint8_t d2 = 0;
            int offset = 0;
        };
        static constexpr int kCapacity = 8192;
        std::array<Rec, (size_t)kCapacity> recs {};
        std::atomic<int> count { 0 };

        void reset() noexcept { count.store(0, std::memory_order_relaxed); }
        void onMidiBlockDelivered(const juce::MidiBuffer& merged, int) override
        {
            for (const auto meta : merged)
            {
                const int n = count.load(std::memory_order_relaxed);
                if (n >= kCapacity)
                {
                    return;
                }
                const juce::MidiMessage m = meta.getMessage();
                if (m.getRawDataSize() < 1)
                {
                    continue;
                }
                Rec r;
                r.status = m.getRawData()[0];
                r.d1 = m.getRawDataSize() > 1 ? m.getRawData()[1] : (std::uint8_t)0;
                r.d2 = m.getRawDataSize() > 2 ? m.getRawData()[2] : (std::uint8_t)0;
                r.offset = meta.samplePosition;
                recs[(size_t)n] = r;
                count.store(n + 1, std::memory_order_release);
            }
        }
        [[nodiscard]] int countNotes(const int channel, const int note, const bool noteOn) const noexcept
        {
            int total = 0;
            const int n = juce::jmin(kCapacity, count.load(std::memory_order_acquire));
            for (int i = 0; i < n; ++i)
            {
                const Rec& r = recs[(size_t)i];
                const int ch = (r.status & 0x0f) + 1;
                const int st = r.status & 0xf0;
                if (ch != channel || (int)r.d1 != note)
                {
                    continue;
                }
                const bool isOn = st == 0x90 && r.d2 > 0;
                const bool isOff = st == 0x80 || (st == 0x90 && r.d2 == 0);
                if ((noteOn && isOn) || (!noteOn && isOff))
                {
                    ++total;
                }
            }
            return total;
        }
        [[nodiscard]] int countCc(const int channel, const int controller) const noexcept
        {
            int total = 0;
            const int n = juce::jmin(kCapacity, count.load(std::memory_order_acquire));
            for (int i = 0; i < n; ++i)
            {
                const Rec& r = recs[(size_t)i];
                if ((r.status & 0xf0) == 0xb0 && ((r.status & 0x0f) + 1) == channel && (int)r.d1 == controller)
                {
                    ++total;
                }
            }
            return total;
        }
        [[nodiscard]] int countPitchBend(const int channel) const noexcept
        {
            int total = 0;
            const int n = juce::jmin(kCapacity, count.load(std::memory_order_acquire));
            for (int i = 0; i < n; ++i)
            {
                const Rec& r = recs[(size_t)i];
                if ((r.status & 0xf0) == 0xe0 && ((r.status & 0x0f) + 1) == channel)
                {
                    ++total;
                }
            }
            return total;
        }
    };
    StabilityLiveMidiCaptureSink stabilityLiveMidiCaptureSink_;
    /// Live-MIDI scenario: a real loopback output (an output whose name matches an input) when
    /// one exists on this machine; null otherwise (then injection enters at the bus boundary).
    std::unique_ptr<juce::MidiOutput> stabilityLiveMidiLoopbackOut_;
    bool stabilityLiveMidiLoopbackResolved_ = false;
    void ensureStabilityLiveMidiLoopbackResolved()
    {
        if (stabilityLiveMidiLoopbackResolved_)
        {
            return;
        }
        stabilityLiveMidiLoopbackResolved_ = true;
        const auto inputs = juce::MidiInput::getAvailableDevices();
        for (const auto& out : juce::MidiOutput::getAvailableDevices())
        {
            // Only a dedicated virtual loopback is used (loopMIDI-style names), never a hardware
            // port of the user's interface.
            const bool loopbackName = out.name.containsIgnoreCase("loop") || out.name.containsIgnoreCase("virtual");
            bool hasMatchingInput = false;
            for (const auto& in : inputs)
            {
                hasMatchingInput = hasMatchingInput || in.name == out.name;
            }
            if (loopbackName && hasMatchingInput)
            {
                stabilityLiveMidiLoopbackOut_ = juce::MidiOutput::openDevice(out.identifier);
                if (stabilityLiveMidiLoopbackOut_ != nullptr)
                {
                    // Make sure the matching input is one the fixture listens to ("All").
                    break;
                }
            }
        }
    }
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
    /// midi-track-parity scenario: the imported clip is identified by this name across moves and
    /// save/reload, and its sorted pitches are the expected content at every hop.
    const juce::String stabilityMidiParityClipName_{ "ParityImport" };
    std::vector<int> stabilityMidiParityImportedPitches_;
    /// Audio health probe baselines (`audioHealthProbeBegin` / `audioHealthProbeVerify`).
    std::uint64_t stabilityAudioProbeCallbackBaseline_ = 0;
    std::uint64_t stabilityAudioProbeAdvancedBaseline_ = 0;
    std::map<TrackId, std::uint64_t> stabilityAudioProbeHostBlocksBaseline_;
    std::map<TrackId, std::uint64_t> stabilityAudioProbeHostBlocksPrevWindow_;

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
