#include <JuceHeader.h>

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
#include "app/MidiEditorPresenter.h"
#include "app/PluginHostUiBindings.h"
#include "app/ProjectIoCoordinator.h"
#include "app/RecordingCoordinator.h"
#include "app/TrackLanesEditCoordinator.h"
#include "app/TransportLayoutHelper.h"
#include "app/TransportPlayPauseStopController.h"
#include "app/UndoRedoCoordinator.h"
#include "app/ShortcutDiagnostics.h"
#include "app/TransportControlsFactory.h"
#include "app/TransportControlsShortcutTarget.h"
#include "app/Vst3PluginPickerCoordinator.h"
#include "app/InstrumentMusicalUndoSnapshot.h"
#include "app/InstrumentRuntimeCoordinator.h"
#include "app/InstrumentTimelineRowCoordinator.h"

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/InsertSlotId.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/PlayheadOverlay.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/InspectorView.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/LatencySettingsStore.h"
#include "ui/LatencySettingsView.h"
#include "ui/experimental/ExperimentalMidiEditorWindow.h"

#include "io/AudioWaveformCache.h"
#include "io/ProjectFile.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

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

    void clearExperimentalInstrumentRuntimesPreserveBridgeOnly() noexcept;

public:
    TransportControlsContent(Transport& transportIn,
                             Session& sessionIn,
                             PluginInsertHost& pluginInsertHostIn,
                             juce::AudioDeviceManager& deviceManagerIn,
                             RecorderService& recorderIn,
                             CountInClickOutput& countInClicksIn,
                             LatencySettingsStore& latencyStoreIn,
                             PlaybackEngine& playbackEngineIn)
        : transport(transportIn)
        , session(sessionIn)
        , pluginHost_(pluginInsertHostIn)
        , deviceManager(deviceManagerIn)
        , recorder_(recorderIn)
        , countInClicks_(countInClicksIn)
        , latencyStore_(latencyStoreIn)
        , playbackEngine_(playbackEngineIn)
        , timelineViewport_()
        , audioWaveformCache_()
        , rulerView(
              sessionIn,
              transportIn,
              deviceManagerIn,
              timelineViewport_,
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
            playPauseButton,
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
                                return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
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
        clipPasteCallbacks.syncViewportFromSession = [this] { syncViewportFromSession(); };
        clipPasteboardController_
            = std::make_unique<ClipPasteboardController>(
                session,
                transport,
                trackLanesView,
                rulerView,
                inspectorView_,
                std::move(clipPasteCallbacks));

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
                    return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
                },
                [this](TrackId tid) { return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid); },
                [this](const juce::String& lab, std::function<bool()> m) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->executeUndoableInstrumentEdit(lab, std::move(m));
                    }
                },
                [this](const juce::String& lab,
                      std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical) {
                    if (undoRedoCoordinator_ != nullptr)
                    {
                        undoRedoCoordinator_->commitInstrumentMusicalUndoPair(lab, std::move(beforeMusical));
                    }
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
                                return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
                            } });
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
            });

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
        setWantsKeyboardFocus(true);
        audioWaveformCache_.setOnPyramidReady([this](const AudioClip*) { trackLanesView.repaint(); });
        timelineViewport_.setOnVisibleRangeChanged([this] {
            rulerView.repaint();
            trackLanesView.repaint();
            instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
        });
        addClipButton.onClick = [this] { addClipAtPlayheadClicked(); };
        addTrackButton.onClick = [this] {
            juce::PopupMenu menu;
            menu.addItem(1, "Add Audio Track");
            juce::PopupMenu instrMenu;
            instrMenu.addItem(100, "Groove Agent SE");
            instrMenu.addItem(
                juce::PopupMenu::Item("HALion Sonic (not validated yet)").setEnabled(false));
            menu.addSubMenu("Add Instrument Track", instrMenu);
            juce::Component::SafePointer<mini_daw_app_transport::TransportControlsContent> safeThis(this);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&addTrackButton),
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
                    if (result == 100)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            safeThis->addInstrumentTrackCoordinator_->addGrooveAgentInstrumentTrackFromMenu();
                        }
                    }
                });
        };
        projectIoCoordinator_ = std::make_unique<ProjectIoCoordinator>(
            transport,
            session,
            deviceManager,
            pluginHost_,
            ProjectIoCoordinator::Callbacks{
                [this](TrackId tid) {
                    return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
                },
                [this] {
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->snapshotOpenClipViewportFromRollIfOpen();
                    }
                },
                [this] { clearExperimentalInstrumentRuntimesPreserveBridgeOnly(); },
                [this](TrackId tid) { return instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid); },
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
                    syncViewportFromSession();
                    if (midiEditorPresenter_ != nullptr)
                    {
                        midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
                    }
                    trackLanesView.syncTracksFromSession();
                    inspectorView_.refreshFromSession();
                    rulerView.repaint();
                    trackLanesView.repaint();
                    refreshInstrumentUi();
                    resized();
                },
            });
        saveProjectButton.onClick = [this] { projectIoCoordinator_->saveProject(); };
        loadProjectButton.onClick = [this] { projectIoCoordinator_->loadProject(); };
        playPauseButton.onClick = [this] { transportPlayPauseStopController_->togglePlayPauseFromUi(); };
        // Stop: "playback off + playhead to start" when idle; if recording, finalize/commit first
        // so RecorderService is never left recording while transport is Stopped.
        stopButton.onClick = [this] { transportPlayPauseStopController_->stopOrSeekFromStopButton(); };
        audioSettingsButton.onClick = [this] { showAudioSettingsDialog(); };
        helpButton.onClick = [this] { showHelpMenuPopup(); };

        constexpr int kEditToolRadioGroup = 90421;
        pointerToolButton_.setClickingTogglesState(true);
        pointerToolButton_.setToggleState(true, juce::dontSendNotification);
        pointerToolButton_.setRadioGroupId(kEditToolRadioGroup);
        splitToolButton_.setClickingTogglesState(true);
        splitToolButton_.setRadioGroupId(kEditToolRadioGroup);
        pointerToolButton_.onClick = [this] {
            currentEditTool_ = EditTool::Pointer;
            trackLanesView.repaint();
        };
        splitToolButton_.onClick = [this] {
            currentEditTool_ = EditTool::Split;
            trackLanesView.repaint();
        };

        addAndMakeVisible(addClipButton);
        addAndMakeVisible(addTrackButton);
        addAndMakeVisible(saveProjectButton);
        addAndMakeVisible(loadProjectButton);
        addAndMakeVisible(playPauseButton);
        addAndMakeVisible(stopButton);
        addAndMakeVisible(audioSettingsButton);
        addAndMakeVisible(helpButton);
        addAndMakeVisible(pointerToolButton_);
        addAndMakeVisible(splitToolButton_);
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
        addAndMakeVisible(inspectorView_);
        addAndMakeVisible(inspectorResizeSplitter_);
        addAndMakeVisible(rulerView);
        addAndMakeVisible(trackLanesView);
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
        lanePlayheadOverlay_ = std::make_unique<PlayheadOverlay>(session, transport, timelineViewport_);
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
                [this] { syncViewportFromSession(); },
                [this](TrackId tid) { return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid); },
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
                },
                [this] { refreshInstrumentUi(); },
                [this] { return instrumentRuntimeCoordinator_->hasAnyKeyedInstrumentControllerActive(); },
                [this] { instrumentRuntimeCoordinator_->deactivateKeyedInstrumentControllersOnly(); },
            });
        trackLanesEditCoordinator_->install();

        deviceManager.addChangeListener(this);
        transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
        startTimerHz(10);
        syncViewportFromSession();
        if (midiEditorPresenter_ != nullptr)
        {
            midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
        }
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
    }

    ~TransportControlsContent() override
    {
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
            return;
        }
        const std::int64_t L = session.getLeftLocatorSamples();
        const std::int64_t R = session.getRightLocatorSamples();
        if (R > L && R > 0)
        {
            transport.requestSeek(L);
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->notifyMidiEditorExternalTransportSeekIfOpen(L);
            }
            return;
        }
        juce::Logger::writeToLog("[Shortcut] numpad1 ignored: no valid locator range");
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

    // [Message thread] Layout: one row of buttons, fixed-height time ruler, then event lane.
    void resized() override
    {
        instrumentRuntimeCoordinator_->syncAllKeyedAndStagingShellWithHostState();
        applyTransportControlsLayout(TransportLayoutRefs{
            *this,
            rulerView,
            trackLanesView,
            inspectorView_,
            addClipButton,
            addTrackButton,
            saveProjectButton,
            loadProjectButton,
            playPauseButton,
            stopButton,
            audioSettingsButton,
            helpButton,
            pointerToolButton_,
            splitToolButton_,
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
        mini_daw_app_dialogs::showHelpMenuPopup(helpButton, this, []() {
            mini_daw_app_dialogs::showUndoBehaviorDialog();
        });
    }

    void timerCallback() override
    {
        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->tickStructuralEditBlockedHeaderStripRepaint(
                trackLanesView.isStructuralTimelineEditBlocked());
        }
        transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
        inspectorView_.refreshFromSession();
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
    }

    // Delegates to `AudioClipImportCoordinator` (file dialog + project Audio/ import + placement
    // at the transport playhead snapshot on the message thread).
    void addClipAtPlayheadClicked()
    {
        if (audioClipImportCoordinator_ != nullptr)
        {
            audioClipImportCoordinator_->addClipAtPlayheadClicked();
        }
    }

    void refreshInstrumentUi()
    {
        instrumentTimelineRowCoordinator_->syncInstrumentTimelineRowAttachmentToSession();
        instrumentRuntimeCoordinator_->updateExperimentalPlaybackBridgeAfterRegistryChange();
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

    Transport& transport;
    Session& session;
    PluginInsertHost& pluginHost_;
    juce::AudioDeviceManager& deviceManager;
    RecorderService& recorder_;
    CountInClickOutput& countInClicks_;
    LatencySettingsStore& latencyStore_;
    PlaybackEngine& playbackEngine_;

    std::unique_ptr<InstrumentRuntimeCoordinator> instrumentRuntimeCoordinator_;
    /// Listed after IRC: reverse member destruction runs this dtor first while `instrumentRuntimeCoordinator_` still exists.
    std::unique_ptr<AddInstrumentTrackCoordinator> addInstrumentTrackCoordinator_;

    /// When Audio Settings is open; auto-clears when the dialog-owned view is destroyed.
    juce::Component::SafePointer<LatencySettingsView> audioLatencySettingsWeak_;
    /// Count-in / recording line (no always-visible audio device debug; use Audio...).
    juce::Label countInStatusLabel_;
    std::unique_ptr<RecordingCoordinator> recordingCoordinator_;
    std::unique_ptr<TransportPlayPauseStopController> transportPlayPauseStopController_;
    std::unique_ptr<UndoRedoCoordinator> undoRedoCoordinator_;
    std::unique_ptr<ClipPasteboardController> clipPasteboardController_;
    std::unique_ptr<AudioClipImportCoordinator> audioClipImportCoordinator_;
    std::unique_ptr<InstrumentMidiImportCoordinator> instrumentMidiImportCoordinator_;
    std::unique_ptr<Vst3PluginPickerCoordinator> vst3PluginPickerCoordinator_;
    std::unique_ptr<ExperimentalMidiEditorWindow> midiEditorWindow_;
    std::unique_ptr<MidiEditorPresenter> midiEditorPresenter_;
    std::unique_ptr<InstrumentTimelineRowCoordinator> instrumentTimelineRowCoordinator_;
    std::unique_ptr<ProjectIoCoordinator> projectIoCoordinator_;

    EditTool currentEditTool_ = EditTool::Pointer;

    juce::TextButton addClipButton{ "Add clip..." };
    juce::TextButton addTrackButton{ "Add track" };
    juce::TextButton saveProjectButton{ "Save Project..." };
    juce::TextButton loadProjectButton{ "Load Project..." };
    juce::TextButton playPauseButton{ "Play" };
    juce::TextButton stopButton{ "Stop" };
    juce::TextButton audioSettingsButton{ "Audio..." };
    juce::TextButton helpButton{ "Help..." };
    juce::TextButton pointerToolButton_{ "Pointer" };
    juce::TextButton splitToolButton_{ "Split" };

    juce::Label keyDiagLabel_;
    std::unique_ptr<juce::Label> shortcutDiagLabel_;

    /// UI-only: shared x–span for ruler and lanes; never stored in `Session` (see `PHASE_PLAN`).
    TimelineViewportModel timelineViewport_;
    AudioWaveformCache audioWaveformCache_;
    TimelineRulerView rulerView;
    TrackLanesView trackLanesView;
    std::unique_ptr<PlayheadOverlay> lanePlayheadOverlay_;
    InspectorView inspectorView_;
    collapsible_side_strip::ResizeSplitter inspectorResizeSplitter_;
    collapsible_side_strip::CollapsedKnob inspectorCollapsedKnob_;
    int inspectorCurrentWidth_ = kInspectorDefaultW;

    /// Declared LAST among data members (after `trackLanesView`, `rulerView`, `inspectorView_`)
    /// so it is destroyed FIRST in reverse-declaration order, while every UI object it borrows
    /// is still alive. See `TrackLanesEditCoordinator` ctor — it stores `&` to those views.
    std::unique_ptr<TrackLanesEditCoordinator> trackLanesEditCoordinator_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportControlsContent)
};

} // namespace mini_daw_app_transport

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
    if (midiEditorPresenter_ != nullptr)
    {
        midiEditorPresenter_->resetWindowAndBooking();
    }
    trackLanesView.syncInstrumentTimelineAttachments({});
    instrumentTimelineRowCoordinator_->clearInstrumentTimelineLanesAndHeaders();
    instrumentRuntimeCoordinator_->clearRuntimesPreserveBridgeOnly();
}

CreatedTransportUiForMainWindow createTransportUiForMainWindow(
    Transport& transport,
    Session& session,
    PluginInsertHost& pluginInsertHost,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorderService,
    CountInClickOutput& countInClicks,
    LatencySettingsStore& latencyStore,
    PlaybackEngine& playbackEngine)
{
    auto component = std::make_unique<mini_daw_app_transport::TransportControlsContent>(
        transport,
        session,
        pluginInsertHost,
        deviceManager,
        recorderService,
        countInClicks,
        latencyStore,
        playbackEngine);

    TransportControlsShortcutTarget* const shortcutTarget =
        static_cast<TransportControlsShortcutTarget*>(component.get());

    return {std::move(component), shortcutTarget};
}
