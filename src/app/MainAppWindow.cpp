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
#include "diagnostics/ProjectLoadDiagnosticLog.h"

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
#include "instruments/InstrumentTrackController.h"
#include "plugins/InsertSlotId.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/PlayheadOverlay.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackLanesView.h"
#include "ui/EditToolIconStrip.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/InspectorView.h"
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
                [this]() {
                    applyArrangementSnapUiFromSettings(session.getArrangementSnapSettings(), true);
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
            return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
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
            rulerView.repaint();
            trackLanesView.repaint();
            instrumentTimelineRowCoordinator_->repaintInstrumentTrackRow();
        });
        addTrackCornerPlusButton_.onClick = [this] {
            juce::PopupMenu menu;
            menu.addItem(1, "Add Audio Track");
            menu.addItem(3, "Add Group Track");
            juce::PopupMenu instrMenu;
            instrMenu.addItem(99, "Rescan instrument plugins...");
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
                    if (result == 99)
                    {
                        if (safeThis->addInstrumentTrackCoordinator_ != nullptr)
                        {
                            safeThis->addInstrumentTrackCoordinator_->rescanInstrumentPluginsFromMenu();
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
                        return captureProjectMainWindowBoundsForProjectSave(*dw);
                    }
                    return std::nullopt;
                },
                [this](const ProjectFileV1& loaded) {
                    if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                    {
                        applyLoadedProjectMainWindowBounds(*dw, loaded);
                    }
                },
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
                [this](const TrackId tid) {
                    if (recorder_.getArmedTrackId() == tid)
                    {
                        recorder_.disarm();
                    }
                },
            });
        trackLanesEditCoordinator_->install();

        if (instrumentTimelineRowCoordinator_ != nullptr)
        {
            instrumentTimelineRowCoordinator_->rewireInstrumentTrackRenameHandlers();
        }

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

    void showAudioMixdownDialog()
    {
        mini_daw_app_dialogs::showAudioMixdownDialog(*this,
                                                     transport,
                                                     session,
                                                     playbackEngine_,
                                                     deviceManager,
                                                     [this]() {
                                                         transportPlayPauseStopController_->updatePlayPauseButtonFromTransport();
                                                     });
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
        mini_daw_app_dialogs::showHelpMenuPopup(*menuBar_, this, []() {
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
        playbackEngine_.rebuildRoutingPlanFromSession();
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
    /// Count-in / recording line (no always-visible audio device debug; use Audio menu).
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
    TimelineRulerView rulerView;
    TrackLanesView trackLanesView;
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
    arrangementBpmEditor_.onReturnKey = [this] { commitArrangementBpmFromEditorIfNeeded(); };
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
