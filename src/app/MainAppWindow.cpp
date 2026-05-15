#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/AudioClipImportCoordinator.h"
#include "app/ClipPasteboardController.h"
#include "app/MainAppDialogs.h"
#include "app/MidiEditorPresenter.h"
#include "app/ProjectIoCoordinator.h"
#include "app/RecordingCoordinator.h"
#include "app/TrackLanesEditCoordinator.h"
#include "app/UndoRedoCoordinator.h"
#include "app/ShortcutDiagnostics.h"
#include "app/TransportControlsFactory.h"
#include "app/TransportControlsShortcutTarget.h"
#include "app/Vst3PluginPickerCoordinator.h"
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

    [[nodiscard]] TrackId canonicalInstrumentLaneTrackIdFromSession() const noexcept;

    [[nodiscard]] bool anyHeldExperimentalHostShowsGrooveAgentLoaded() const noexcept;

    [[nodiscard]] ExperimentalInstrumentHost* getInstrumentHostForTrack(TrackId tid) const noexcept;

    [[nodiscard]] InstrumentTrackController* getInstrumentControllerForTrack(TrackId tid) const noexcept;

    [[nodiscard]] std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*> getOrCreateInstrumentRuntimeForTrack(
        TrackId tid);

    [[nodiscard]] std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
                    getExperimentRuntimePairForGrooveAdds();

    void promoteInstrumentStagingIntoRegistryBoundTo(TrackId tid);

    void removeInstrumentRuntimeForTrack(TrackId tid) noexcept;

    void clearExperimentalInstrumentRuntimesPreserveBridgeOnly() noexcept;

    void experimentalBeginAudioBlockAllHosts(std::int64_t numSamples) noexcept;

    void prepareExperimentalInstrumentHostsForDevice(double sampleRate, int blockSamples) noexcept;

    void releaseExperimentalInstrumentHostsDeviceResources() noexcept;

    void updateExperimentalPlaybackBridgeAfterRegistryChange();

    [[nodiscard]] juce::String proposeNextGrooveAgentInstrumentTrackDisplayName() const;
    [[nodiscard]] bool tryCloneGrooveAgentFromAnyExistingInto(ExperimentalInstrumentHost& dest) noexcept;

    [[nodiscard]] std::vector<ProjectFileExperimentalInstrumentTrackV1>
    buildSortedInstrumentMusicalUndoSnapshot() const;
    static void stableSortInstrumentMusicalUndoVector(std::vector<ProjectFileExperimentalInstrumentTrackV1>& v);
    void applyInstrumentMusicalUndoVectorToAllKeyedControllers(
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept;

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
                [this]() { updatePlayPauseButtonFromTransport(); },
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
                [this] { return buildSortedInstrumentMusicalUndoSnapshot(); },
                [](std::vector<ProjectFileExperimentalInstrumentTrackV1>& v) {
                    TransportControlsContent::stableSortInstrumentMusicalUndoVector(v);
                },
                [this](const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) {
                    applyInstrumentMusicalUndoVectorToAllKeyedControllers(tracks);
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
            });

        midiEditorPresenter_ = std::make_unique<MidiEditorPresenter>(
            transport,
            session,
            deviceManager,
            recorder_,
            timelineViewport_,
            midiEditorWindow_,
            MidiEditorPresenter::Callbacks{
                [this](TrackId tid) { return getInstrumentControllerForTrack(tid); },
                [this](TrackId tid) { return getInstrumentHostForTrack(tid); },
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
                [this] { return buildSortedInstrumentMusicalUndoSnapshot(); },
                [this] { invokeUndoFromWindowShortcut(); },
                [this] { invokeRedoFromWindowShortcut(); },
                [this] { invokePlayPauseToggleFromWindowShortcut(); },
                [this] { stopOrSeekFromStopButton(); },
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
                        safeThis->onAddGrooveAgentInstrumentTrackFromMenu();
                    }
                });
        };
        projectIoCoordinator_ = std::make_unique<ProjectIoCoordinator>(
            transport,
            session,
            deviceManager,
            pluginHost_,
            ProjectIoCoordinator::Callbacks{
                [this](TrackId tid) { return getInstrumentControllerForTrack(tid); },
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
        playPauseButton.onClick = [this] { togglePlayPauseFromUi(); };
        // Stop: "playback off + playhead to start" when idle; if recording, finalize/commit first
        // so RecorderService is never left recording while transport is Stopped.
        stopButton.onClick = [this] { stopOrSeekFromStopButton(); };
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
        trackLanesView.setTrackHeaderPluginHost(
            { [this](const TrackId tid) {
                  vst3PluginPickerCoordinator_->showVst3PluginPickerForTrack(
                      tid, Vst3PluginPickerCoordinator::InsertPickerMode::AddPost, this);
              },
              [this](const TrackId tid) { pluginHost_.openNativeEditor(tid); },
              [this](const TrackId tid) { pluginHost_.openGenericParamsEditor(tid); },
              [this](const TrackId tid) { pluginHost_.removePlugin(tid); } });
        inspectorView_.setInspectorPluginHost({
            [this](const TrackId tid) { return pluginHost_.hasAnyInsertOnTrack(tid); },
            [this](const TrackId tid) {
                std::vector<InspectorInsertRow> rows;
                rows.reserve(8);
                for (const auto& rv : pluginHost_.getInsertRowsForTrack(tid))
                {
                    InspectorInsertRow ir;
                    ir.slotId = rv.slotId;
                    ir.stage = rv.stage;
                    ir.displayName = rv.displayName;
                    rows.push_back(std::move(ir));
                }
                return rows;
            },
            [this](const TrackId tid, const InsertStage st) {
                vst3PluginPickerCoordinator_->showVst3PluginPickerForTrack(
                    tid,
                    st == InsertStage::Pre ? Vst3PluginPickerCoordinator::InsertPickerMode::AddPre
                                           : Vst3PluginPickerCoordinator::InsertPickerMode::AddPost,
                    &inspectorView_);
            },
            [this](const TrackId tid, const InsertSlotId sid) { pluginHost_.openNativeEditor(tid, sid); },
            [this](const TrackId tid, const InsertSlotId sid) { pluginHost_.removeInsert(tid, sid); },
            [this](const TrackId tid, const InsertSlotId sid, const InsertStage st, const int gap) {
                pluginHost_.moveInsertToStageAtGap(tid, sid, st, gap);
            },
            [this](const TrackId tid, const InsertSlotId sid, const int gapIndex) {
                pluginHost_.reorderInsertWithinStage(tid, sid, gapIndex);
            } });
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
                [this](TrackId tid) { return getInstrumentHostForTrack(tid); },
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
        updatePlayPauseButtonFromTransport();
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
        if (recorder_.isRecording())
        {
            recordingCoordinator_->stopRecordingAndCommitFromUi("space");
            return;
        }
        togglePlayPauseTransportOnly();
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
        auto area = getLocalBounds().reduced(8);
        auto row = area.removeFromTop(32);
        if (shortcut_diagnostics::kShowKeyDiagnostic)
        {
            keyDiagLabel_.setBounds(row.removeFromRight(300).reduced(2, 0));
        }
        constexpr int kCountInLabelWidth = 140;
        countInStatusLabel_.setBounds(row.removeFromRight(kCountInLabelWidth).reduced(4, 0));
        const int buttonWidth = juce::jmax(48, row.getWidth() / 8);

        addClipButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        addTrackButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        saveProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        loadProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        playPauseButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        stopButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        audioSettingsButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        helpButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
        auto toolRow = area.removeFromTop(28);
        constexpr int kToolButtonW = 80;
        pointerToolButton_.setBounds(toolRow.removeFromLeft(kToolButtonW).reduced(2, 2));
        splitToolButton_.setBounds(toolRow.removeFromLeft(kToolButtonW).reduced(2, 2));
        if constexpr (shortcut_diagnostics::kShowShortcutDiagnostics)
        {
            if (shortcutDiagLabel_ != nullptr)
            {
                shortcutDiagLabel_->setBounds(area.removeFromTop(28));
            }
        }
        constexpr int kTimelineRulerHeight = 20;
        const int lanesBandTop = area.getY() + kTimelineRulerHeight;
        const int lanesBandHeight = juce::jmax(0, area.getHeight() - kTimelineRulerHeight);
        if (inspectorCurrentWidth_ > 0)
        {
            auto inspectorStrip = area.removeFromLeft(inspectorCurrentWidth_);
            const int splitW = juce::jmin(collapsible_side_strip::kSplitterWidth, inspectorStrip.getWidth());
            const int contentW = juce::jmax(0, inspectorStrip.getWidth() - splitW);
            inspectorView_.setBounds(
                inspectorStrip.getX(),
                inspectorStrip.getY(),
                contentW,
                inspectorStrip.getHeight());
            inspectorView_.setVisible(true);
            inspectorResizeSplitter_.setBounds(
                inspectorStrip.getX() + contentW,
                inspectorStrip.getY(),
                splitW,
                inspectorStrip.getHeight());
            inspectorResizeSplitter_.setVisible(true);
            inspectorCollapsedKnob_.setVisible(false);
        }
        else
        {
            if (area.getX() > 0)
            {
                area.setLeft(0);
            }
            inspectorView_.setBounds(area.getX(), lanesBandTop, 0, lanesBandHeight);
            inspectorView_.setVisible(true);
            inspectorResizeSplitter_.setBounds(0, 0, 0, 0);
            inspectorResizeSplitter_.setVisible(false);
        }
        auto timelineRow = area.removeFromTop(kTimelineRulerHeight);
        timelineRow.removeFromLeft(TrackLanesView::kTrackHeaderWidth);    
        rulerView.setBounds(timelineRow);
        trackLanesView.setBounds(area);
        if (lanePlayheadOverlay_ != nullptr)
        {
            const int tw = trackLanesView.getWidth();
            const int leftStrip = juce::jmin(TrackLanesView::kTrackHeaderWidth, tw);
            const int laneContentLeft = trackLanesView.getX() + leftStrip;
            const int laneW = juce::jmax(0, tw - leftStrip);
            static constexpr bool kLogTransportLaneLayout = false;
            if constexpr (kLogTransportLaneLayout)
            {
                juce::Logger::writeToLog(
                    "Transport layout: trackLanes=" + trackLanesView.getBounds().toString()
                    + " kTrackHeaderWidth=" + juce::String(TrackLanesView::kTrackHeaderWidth)
                    + " laneContentLeft=" + juce::String(laneContentLeft)
                    + " instrumentRowVisible="
                    + juce::String(trackLanesView.isInstrumentTimelineRowVisible() ? 1 : 0)
                    + " ruler=" + rulerView.getBounds().toString()
                    + " laneW=" + juce::String(laneW));
            }

            const int topY = trackLanesView.getY();
            const int bottomY = trackLanesView.getBottom();
            if (laneW > 0 && bottomY > topY)
            {
                lanePlayheadOverlay_->setBounds(laneContentLeft, topY, laneW, bottomY - topY);
                lanePlayheadOverlay_->setVisible(true);
                lanePlayheadOverlay_->toFront(false);
            }
            else
            {
                lanePlayheadOverlay_->setBounds(0, 0, 0, 0);
                lanePlayheadOverlay_->setVisible(false);
            }
        }
        if (inspectorCurrentWidth_ == 0)
        {
            const int knobX = trackLanesView.getBounds().getX();
            const int knobY = trackLanesView.getBounds().getCentreY()
                              - collapsible_side_strip::kCollapsedKnobHeight / 2;
            inspectorCollapsedKnob_.setBounds(
                knobX,
                knobY,
                collapsible_side_strip::kCollapsedKnobWidth,
                collapsible_side_strip::kCollapsedKnobHeight);
            inspectorCollapsedKnob_.setVisible(true);
            inspectorCollapsedKnob_.toFront(false);
        }
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
                                                      [this]() { updatePlayPauseButtonFromTransport(); },
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
        updatePlayPauseButtonFromTransport();
        inspectorView_.refreshFromSession();
    }

    // [Message thread] Transport intent: Playing -> Paused, else (Stopped or Paused) -> Playing.
    // If a take is in progress, the button stops/commits (never Paused+recording).
    void togglePlayPauseTransportOnly()
    {
        if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
        {
            transport.requestPlaybackIntent(PlaybackIntent::Paused);
        }
        else
        {
            transport.requestPlaybackIntent(PlaybackIntent::Playing);
        }
        updatePlayPauseButtonFromTransport();
    }

    void togglePlayPauseFromUi()
    {
        if (recordingCoordinator_->isCountInActive())
        {
            recordingCoordinator_->cancelCountIn();
            return;
        }
        if (recorder_.isRecording())
        {
            recordingCoordinator_->stopRecordingAndCommitFromUi("play_pause");
            return;
        }
        togglePlayPauseTransportOnly();
    }

    // [Message thread] Stop button: normal stop+seek, or end recording and commit (then seek 0).
    void stopOrSeekFromStopButton()
    {
        if (recordingCoordinator_->isCountInActive())
        {
            recordingCoordinator_->cancelCountIn();
            return;
        }
        if (recorder_.isRecording())
        {
            recordingCoordinator_->stopRecordingAndCommitFromUi("stop");
        }
        else
        {
            transport.requestPlaybackIntent(PlaybackIntent::Stopped);
        }
        transport.requestSeek(0);
        updatePlayPauseButtonFromTransport();
    }

    void updatePlayPauseButtonFromTransport()
    {
        const bool playing = transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
        const juce::String t = playing ? "Pause" : "Play";
        if (t != playPauseButton.getButtonText())
        {
            playPauseButton.setButtonText(t);
        }
    }

    // [Message thread] Undo-1: mutator must return false when no session mutation occurred
    // (e.g. paste `Result::fail`). `Session::removePlacedClip` / `removeTrack` always allocate a
    // new `SessionSnapshot` instance even on semantic no-op, so we only wrap paths that
    // pre-validate the target id (delete event / delete track) or use `Result::wasOk()` (paste).
    // Steps are recorded by `UndoRedoCoordinator::executeUndoableSessionEdit` (unchanged rules).
    template <typename F>
    void executeUndoableSessionEdit(const juce::String& label, F&& mutator)
    {
        static_assert(std::is_invocable_r_v<bool, F>);
        if (undoRedoCoordinator_ == nullptr)
        {
            return;
        }
        undoRedoCoordinator_->executeUndoableSessionEdit(
            label,
            [m = std::forward<F>(mutator)]() mutable -> bool { return m(); });
    }

    template <typename F>
    void executeUndoableInstrumentEdit(const juce::String& label, F&& mutator)
    {
        static_assert(std::is_invocable_r_v<bool, F>);
        if (undoRedoCoordinator_ == nullptr)
        {
            return;
        }
        undoRedoCoordinator_->executeUndoableInstrumentEdit(
            label,
            [m = std::forward<F>(mutator)]() mutable -> bool { return m(); });
    }

    void commitInstrumentMusicalUndoPair(const juce::String& label,
                                        std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical)
    {
        if (undoRedoCoordinator_ == nullptr)
        {
            return;
        }
        undoRedoCoordinator_->commitInstrumentMusicalUndoPair(label, std::move(beforeMusical));
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

    void onAddGrooveAgentInstrumentTrackFromMenu()
    {
        if (!anyHeldExperimentalHostShowsGrooveAgentLoaded())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Instrument track",
                "Load Groove Agent SE from cached OOP description first.");
            return;
        }
        const juce::String displayName = proposeNextGrooveAgentInstrumentTrackDisplayName();
        const std::optional<TrackId> newIdOpt = session.appendExperimentalInstrumentShellTrack(displayName);
        if (!newIdOpt.has_value())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Instrument track",
                "Could not add instrument track shell.");
            return;
        }

        const TrackId tid = *newIdOpt;
        const bool registryWasEmpty = instrumentRuntimeCoordinator_->isKeyedRuntimeRegistryEmpty();
        ExperimentalInstrumentHost* mh = nullptr;
        InstrumentTrackController* ctl = nullptr;

        if (registryWasEmpty && instrumentRuntimeCoordinator_->stagingInstrumentHostUnchecked() != nullptr
            && instrumentRuntimeCoordinator_->stagingInstrumentControllerUnchecked() != nullptr
            && instrumentRuntimeCoordinator_->stagingInstrumentHostUnchecked()->hasInstrument()
            && instrumentRuntimeCoordinator_->stagingInstrumentHostUnchecked()->getInstrumentNameForUi().containsIgnoreCase(
                   "Groove Agent"))
        {
            mh = instrumentRuntimeCoordinator_->stagingInstrumentHostUnchecked();
            ctl = instrumentRuntimeCoordinator_->stagingInstrumentControllerUnchecked();
        }
        else
        {
            const auto pr = instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid);
            mh = pr.first;
            ctl = pr.second;
        }

        if (mh == nullptr || ctl == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                    "Instrument track",
                                                    "Could not allocate Groove Agent instrument runtime.");
            session.removeTrack(tid);
            if (!registryWasEmpty)
            {
                instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
            }
            refreshInstrumentUi();
            return;
        }

        if (!mh->hasInstrument() || !mh->getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
        {
            if (!tryCloneGrooveAgentFromAnyExistingInto(*mh))
            {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                        "Instrument track",
                                                        "Could not clone Groove Agent into this instrument track.");
                session.removeTrack(tid);
                if (!registryWasEmpty)
                {
                    instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
                }
                refreshInstrumentUi();
                return;
            }
        }

        if (!ctl->bootstrapGrooveAgentShellForSessionTrack(tid))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                    "Instrument track",
                                                    "Could not bind Groove Agent to the new timeline row.");
            session.removeTrack(tid);
            if (!registryWasEmpty)
            {
                instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
            }
            refreshInstrumentUi();
            return;
        }

        if (registryWasEmpty && mh == instrumentRuntimeCoordinator_->stagingInstrumentHostUnchecked()
            && ctl == instrumentRuntimeCoordinator_->stagingInstrumentControllerUnchecked())
        {
            instrumentRuntimeCoordinator_->promoteInstrumentStagingIntoRegistryBoundTo(tid);
        }

        ctl->syncShellWithHostState();
        instrumentRuntimeCoordinator_->updateExperimentalPlaybackBridgeAfterRegistryChange();
        if (midiEditorPresenter_ != nullptr)
        {
            midiEditorPresenter_->syncInstrumentClipTimelineFromDevice();
        }
        refreshInstrumentUi();
        resized();
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

    /// When Audio Settings is open; auto-clears when the dialog-owned view is destroyed.
    juce::Component::SafePointer<LatencySettingsView> audioLatencySettingsWeak_;
    /// Count-in / recording line (no always-visible audio device debug; use Audio...).
    juce::Label countInStatusLabel_;
    std::unique_ptr<RecordingCoordinator> recordingCoordinator_;
    std::unique_ptr<UndoRedoCoordinator> undoRedoCoordinator_;
    std::unique_ptr<ClipPasteboardController> clipPasteboardController_;
    std::unique_ptr<AudioClipImportCoordinator> audioClipImportCoordinator_;
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

TrackId mini_daw_app_transport::TransportControlsContent::canonicalInstrumentLaneTrackIdFromSession() const noexcept
{
    return instrumentRuntimeCoordinator_->canonicalInstrumentLaneTrackIdFromSession();
}

bool mini_daw_app_transport::TransportControlsContent::anyHeldExperimentalHostShowsGrooveAgentLoaded() const noexcept
{
    return instrumentRuntimeCoordinator_->anyHeldHostShowsGrooveAgentLoaded();
}

ExperimentalInstrumentHost* mini_daw_app_transport::TransportControlsContent::getInstrumentHostForTrack(const TrackId tid) const noexcept
{
    return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
}

InstrumentTrackController* mini_daw_app_transport::TransportControlsContent::getInstrumentControllerForTrack(const TrackId tid) const noexcept
{
    return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
}

std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
mini_daw_app_transport::TransportControlsContent::getOrCreateInstrumentRuntimeForTrack(const TrackId tid)
{
    return instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid);
}

std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
mini_daw_app_transport::TransportControlsContent::getExperimentRuntimePairForGrooveAdds()
{
    return instrumentRuntimeCoordinator_->getExperimentRuntimePairForGrooveAdds();
}

void mini_daw_app_transport::TransportControlsContent::promoteInstrumentStagingIntoRegistryBoundTo(const TrackId tid)
{
    instrumentRuntimeCoordinator_->promoteInstrumentStagingIntoRegistryBoundTo(tid);
}

void mini_daw_app_transport::TransportControlsContent::removeInstrumentRuntimeForTrack(const TrackId tid) noexcept
{
    instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
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

void mini_daw_app_transport::TransportControlsContent::experimentalBeginAudioBlockAllHosts(const std::int64_t numSamples) noexcept
{
    instrumentRuntimeCoordinator_->experimentalBeginAudioBlockAllHosts(numSamples);
}

void mini_daw_app_transport::TransportControlsContent::prepareExperimentalInstrumentHostsForDevice(const double sampleRate,
                                                                          const int blockSamples) noexcept
{
    instrumentRuntimeCoordinator_->prepareExperimentalInstrumentHostsForDevice(sampleRate, blockSamples);
}

void mini_daw_app_transport::TransportControlsContent::releaseExperimentalInstrumentHostsDeviceResources() noexcept
{
    instrumentRuntimeCoordinator_->releaseExperimentalInstrumentHostsDeviceResources();
}

void mini_daw_app_transport::TransportControlsContent::updateExperimentalPlaybackBridgeAfterRegistryChange()
{
    instrumentRuntimeCoordinator_->updateExperimentalPlaybackBridgeAfterRegistryChange();
}

juce::String mini_daw_app_transport::TransportControlsContent::proposeNextGrooveAgentInstrumentTrackDisplayName()
    const
{
    int shells = 0;
    const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
    if (snap != nullptr)
    {
        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            if (snap->getTrack(ti).getKind() == TrackKind::Instrument)
            {
                ++shells;
            }
        }
    }
    if (shells == 0)
    {
        return juce::String("Groove Agent");
    }
    return juce::String("Groove Agent ") + juce::String(shells + 1);
}

bool mini_daw_app_transport::TransportControlsContent::tryCloneGrooveAgentFromAnyExistingInto(
    ExperimentalInstrumentHost& dest) noexcept
{
    ExperimentalInstrumentHost* src = instrumentRuntimeCoordinator_->findGrooveAgentTemplateHostPreferKeyed(&dest);
    if (src == nullptr || src == &dest)
    {
        return false;
    }
    juce::PluginDescription d{};
    if (!src->getLastLoadedPluginDescription(d))
    {
        return false;
    }
    const juce::File original(src->getLastLoadedVst3OriginalPath());
    if (!original.exists())
    {
        return false;
    }
    juce::MemoryBlock restored;
    {
        const juce::String b64 = src->getCurrentInstrumentStateBase64();
        if (b64.isNotEmpty())
        {
            juce::MemoryOutputStream mos;
            if (juce::Base64::convertFromBase64(mos, b64))
            {
                restored.append(mos.getData(), mos.getDataSize());
            }
        }
    }
    juce::String warnIgnored;
    const juce::MemoryBlock* mb = restored.getSize() > 0 ? &restored : nullptr;
    return dest
        .loadInstrumentFromDescription(
            d,
            original,
            "groove-agent-sibling-clone",
            mb,
            &warnIgnored)
        .wasOk();
}

std::vector<ProjectFileExperimentalInstrumentTrackV1>
mini_daw_app_transport::TransportControlsContent::buildSortedInstrumentMusicalUndoSnapshot() const
{
    std::vector<ProjectFileExperimentalInstrumentTrackV1> out;
    const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return out;
    }
    for (int ti = 0; ti < snap->getNumTracks(); ++ti)
    {
        const Track& tr = snap->getTrack(ti);
        if (tr.getKind() != TrackKind::Instrument)
        {
            continue;
        }
        InstrumentTrackController* const ctl = const_cast<mini_daw_app_transport::TransportControlsContent*>(this)->getInstrumentControllerForTrack(
            tr.getId());
        if (ctl == nullptr || !ctl->hasInstrumentTrack()
            || ctl->getExperimentalInstrumentDomainTrackId() != tr.getId())
        {
            continue;
        }
        std::vector<ProjectFileExperimentalInstrumentTrackV1> one = ctl->buildExperimentalInstrumentMusicalUndoBlock();
        out.insert(out.end(), std::make_move_iterator(one.begin()), std::make_move_iterator(one.end()));
    }
    stableSortInstrumentMusicalUndoVector(out);
    return out;
}

void mini_daw_app_transport::TransportControlsContent::stableSortInstrumentMusicalUndoVector(
    std::vector<ProjectFileExperimentalInstrumentTrackV1>& v)
{
    std::stable_sort(
        v.begin(),
        v.end(),
        [](const ProjectFileExperimentalInstrumentTrackV1& a,
           const ProjectFileExperimentalInstrumentTrackV1& b) noexcept -> bool { return a.trackId < b.trackId; });
}

void mini_daw_app_transport::TransportControlsContent::applyInstrumentMusicalUndoVectorToAllKeyedControllers(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept
{
    instrumentRuntimeCoordinator_->applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(tracks);
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
