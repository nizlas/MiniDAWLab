#include "app/MainAppWindow.h"
#include "app/MidiEditorPresenter.h"
#include "app/InstrumentRuntimeCoordinator.h"
#include "app/InstrumentTimelineRowCoordinator.h"
#include <JuceHeader.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

#include "domain/Session.h"
#include "domain/Track.h"
#include "domain/SessionHistory.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/InsertSlotId.h"
#include "io/AudioFileLoader.h"
#include "io/MonoWavFileWriter.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/PlayheadOverlay.h"
#include "ui/TimelineViewportModel.h"
#include "ui/ClipWaveformView.h"
#include "ui/TrackLanesView.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/InspectorView.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/LatencySettingsStore.h"
#include "ui/LatencySettingsView.h"
#include "ui/experimental/ExperimentalMidiEditorWindow.h"
#include "ui/TransportShortcutKeys.h"

#include "io/ProjectAudioImport.h"
#include "io/AudioWaveformCache.h"
#include "io/ProjectFile.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "diagnostics/ExperimentalPlaybackRoutingLog.h"

#include "app/ProjectIoCoordinator.h"
#include "app/RecordingCoordinator.h"
#include "app/Vst3PluginPickerCoordinator.h"

#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "domain/SessionSnapshot.h"
namespace
{
    // Temporary: show last key in a small local label (transport row). Leave `false` in normal use.
    constexpr bool kShowKeyDiagnostic = false;

    // Temporary: log + on-screen line for keys reaching MainWindow::routeShortcut. Leave `false` in
    // normal use (no extra layout row; `if constexpr` strips the UI).
    constexpr bool kShowShortcutDiagnostics = false;

    [[nodiscard]] juce::String hex8(const juce::uint32 x)
    {
        return juce::String::toHexString(x).toUpperCase();
    }

    // [ShortcutDiag] Lines are parseable tokens for correlating WM/JUCE conversions with matchers.
    void logShortcutRouterKey(const juce::KeyPress& key)
    {
        if (!kShowShortcutDiagnostics)
        {
            return;
        }
        const int kc = key.getKeyCode();
        const int lowWord = kc & 0xffff;
        const juce_wchar tc = key.getTextCharacter();
        const juce::uint32 kcU = static_cast<juce::uint32>(kc);
        const juce::uint32 tcU = static_cast<juce::uint32>(static_cast<juce::uint16>(tc));
        const juce::uint32 lowU = static_cast<juce::uint32>(lowWord) & 0xffffu;

        const juce::ModifierKeys mods = key.getModifiers();
        const juce::uint32 modRaw = static_cast<juce::uint32>(mods.getRawFlags());

        const int canonNp1 = juce::KeyPress::numberPad1;
        const int canonMul = juce::KeyPress::numberPadMultiply;
        const juce::uint32 canonNp1U = static_cast<juce::uint32>(canonNp1);
        const juce::uint32 canonMulU = static_cast<juce::uint32>(canonMul);

        juce::String msg;
        msg += "[ShortcutDiag] ";
        msg += "keyCode=";
        msg += juce::String(kc);
        msg += " (0x";
        msg += hex8(kcU);
        msg += ") lowWord=";
        msg += juce::String(lowWord);
        msg += " (0x";
        msg += hex8(lowU);
        msg += ") textChar=";
        msg += juce::String(static_cast<int>(tcU & 0xffffu));
        msg += " (0x";
        msg += hex8(tcU);
        msg += ") np1Canon=";
        msg += juce::String(canonNp1);
        msg += " (0x";
        msg += hex8(canonNp1U);
        msg += ") mulCanon=";
        msg += juce::String(canonMul);
        msg += " (0x";
        msg += hex8(canonMulU);
        msg += ") modShift=";
        msg += mods.isShiftDown() ? juce::String("Y") : juce::String("n");
        msg += " modCtrl=";
        msg += mods.isCtrlDown() ? juce::String("Y") : juce::String("n");
        msg += " modAlt=";
        msg += mods.isAltDown() ? juce::String("Y") : juce::String("n");
        msg += " modCmd=";
        msg += mods.isCommandDown() ? juce::String("Y") : juce::String("n");
        msg += " modRaw=0x";
        msg += hex8(modRaw);
        msg += " desc=\"";
        msg += key.getTextDescription();
        msg += "\"";
        juce::Logger::writeToLog(msg);
    }

    [[nodiscard]] juce::String undoDiagSnapPtr(const SessionSnapshot* p)
    {
        if (p == nullptr)
        {
            return "null";
        }
        return "0x" + juce::String::toHexString(reinterpret_cast<juce::pointer_sized_int>(p));
    }

    // Single-line caption for temporary on-screen shortcut diagnostic (transport area).
    [[nodiscard]] juce::String makeShortcutDiagVisibleCaption(const juce::KeyPress& key)
    {
        const int kc = key.getKeyCode();
        const int lowWord = kc & 0xffff;
        const juce_wchar tc = key.getTextCharacter();
        const auto kcU = static_cast<juce::uint32>(kc);
        const auto tcU = static_cast<juce::uint32>(static_cast<juce::uint16>(tc));
        const auto lowU = static_cast<juce::uint32>(lowWord) & 0xffffu;

        juce::String cap;
        cap << "[ShortcutDiag ui] ";
        cap << "keyCode=" << juce::String(kc) << " (0x" << hex8(kcU) << ") ";
        cap << "lowWord=" << juce::String(lowWord) << " (0x" << hex8(lowU) << ") ";
        cap << "textChar=" << juce::String(static_cast<int>(tcU & 0xffffu)) << " (0x"
            << hex8(tcU) << ") ";
        cap << "desc=\"" << key.getTextDescription() << "\"";
        return cap;
    }

} // namespace

namespace
{
class AudioSettingsDialogContent final : public juce::Component
{
public:
    AudioSettingsDialogContent(juce::AudioDeviceManager& dm,
                               LatencySettingsStore& latencyStore,
                               PlaybackEngine& playbackEngine)
        : selector_(dm, 0, 2, 2, 2, false, false, false, false)
        , latencyView_(latencyStore, playbackEngine)
    {
        addAndMakeVisible(selector_);
        addAndMakeVisible(latencyView_);
        setSize(640, 680);
    }

    void resized() override
    {
        constexpr int kGapBelowSelectorPx = 10;
        auto area = getLocalBounds();
        const int w = area.getWidth();
        const int topY = area.getY();

        // AudioDeviceSelectorComponent ends resized() by setSize(w, intrinsicHeight). Lay it out
        // with enough vertical slack first so internal controls measure correctly; then tighten
        // its bounds to that height so we do not leave a tall empty band above the latency panel.
        const int provisionalH = juce::jmax(1, area.getHeight() - kGapBelowSelectorPx);
        selector_.setBounds(area.getX(), topY, w, provisionalH);
        const int selectorH = juce::jmax(1, selector_.getHeight());
        selector_.setBounds(area.getX(), topY, w, selectorH);

        const int latencyY = topY + selectorH + kGapBelowSelectorPx;
        const int latencyH = juce::jmax(1, area.getBottom() - latencyY);
        latencyView_.setBounds(area.getX(), latencyY, w, latencyH);
    }

    [[nodiscard]] LatencySettingsView& getLatencyPane() noexcept { return latencyView_; }

private:
    juce::AudioDeviceSelectorComponent selector_;
    LatencySettingsView latencyView_;
};
} // namespace

namespace
{
class TransportControlsContent : public juce::Component,
                                 public juce::ChangeListener,
                                 private juce::Timer,
                                 public collapsible_side_strip::Host
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
        , sessionHistory_{}
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
                    executeUndoableInstrumentEdit(lab, std::move(m));
                },
                [this](const juce::String& lab,
                      std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical) {
                    commitInstrumentMusicalUndoPair(lab, std::move(beforeMusical));
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
            juce::Component::SafePointer<TransportControlsContent> safeThis(this);
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
                [this] { sessionHistory_.clear(); },
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
        if (kShowKeyDiagnostic)
        {
            addAndMakeVisible(keyDiagLabel_);
            keyDiagLabel_.setFont(juce::FontOptions(11.0f));
            keyDiagLabel_.setJustificationType(juce::Justification::centredLeft);
            keyDiagLabel_.setText("key: —", juce::dontSendNotification);
        }
        if constexpr (kShowShortcutDiagnostics)
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
        pluginHost_.setUndoRecorder(
            this,
            [](void* ctx, const juce::String& label, const PluginUndoStepSides& sides) {
                auto* const self = static_cast<TransportControlsContent*>(ctx);
                const std::shared_ptr<const SessionSnapshot> snap
                    = self->session.loadSessionSnapshotForAudioThread();
                if (snap == nullptr)
                {
                    return;
                }
                PluginUndoStepSides copy = sides;
                self->sessionHistory_.record(label, snap, snap, std::move(copy), std::nullopt);
            });
        pluginHost_.setEditorShortcutCallbacks({
            [this] { invokeUndoFromWindowShortcut(); },
            [this] { invokeRedoFromWindowShortcut(); } });
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
        trackLanesView.setOnDeleteTrackRequested([this](const TrackId tid) {
            if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
            {
                return;
            }
            if (tid == kInvalidTrackId)
            {
                return;
            }
            executeUndoableSessionEdit(
                "Delete track",
                [this, tid]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> snap
                        = session.loadSessionSnapshotForAudioThread();
                    if (snap == nullptr || snap->findTrackIndexById(tid) < 0)
                    {
                        return false;
                    }
                    const int ix = snap->findTrackIndexById(tid);
                    if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Instrument)
                    {
                        if (ExperimentalInstrumentHost* mh = getInstrumentHostForTrack(tid))
                        {
                            mh->closeNativeEditor();
                        }
                        if (midiEditorPresenter_ != nullptr)
                        {
                            midiEditorPresenter_->resetWindowAndBookingIfOpenOnTrack(tid);
                        }
                        instrumentTimelineRowCoordinator_->tearDownExperimentalInstrumentTimelineUiForTrack(tid);
                        pluginHost_.evictPluginForTrackNoUndo(tid);
                        session.removeTrack(tid);
                        instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
                        refreshInstrumentUi();
                    }
                    else
                    {
                        pluginHost_.evictPluginForTrackNoUndo(tid);
                        session.removeTrack(tid);
                    }
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    rulerView.repaint();
                    trackLanesView.repaint();
                    inspectorView_.refreshFromSession();
                    return true;
                });
        });
        trackLanesView.setCommittedHeaderDragTrackReorder([this](const TrackId movedId,
                                                                 const int destSessionIndex) {
            if (movedId == kInvalidTrackId || destSessionIndex < 0)
            {
                return;
            }
            executeUndoableSessionEdit(
                "Reorder track",
                [this, movedId, destSessionIndex]() -> bool {
                    const std::shared_ptr<const SessionSnapshot> before
                        = session.loadSessionSnapshotForAudioThread();
                    if (before == nullptr)
                    {
                        return false;
                    }
                    session.moveTrack(movedId, destSessionIndex);
                    const std::shared_ptr<const SessionSnapshot> after
                        = session.loadSessionSnapshotForAudioThread();
                    if (after == nullptr || after == before)
                    {
                        return false;
                    }
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    rulerView.repaint();
                    trackLanesView.repaint();
                    inspectorView_.refreshFromSession();
                    return true;
                });
        });
        trackLanesView.setOnUndoableClipMoveRequested(
            [this](const PlacedClipId clipId,
                   const std::int64_t newStart,
                   const std::optional<TrackId> destTrack) -> bool {
                if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
                {
                    return false;
                }
                if (clipId == kInvalidPlacedClipId)
                {
                    return false;
                }
                bool committed = false;
                executeUndoableSessionEdit(
                    "Move clip",
                    [this, clipId, newStart, destTrack, &committed]() -> bool {
                        const std::shared_ptr<const SessionSnapshot> snapBefore
                            = session.loadSessionSnapshotForAudioThread();
                        if (snapBefore == nullptr)
                        {
                            return false;
                        }
                        bool found = false;
                        for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                        {
                            const Track& tr = snapBefore->getTrack(ti);
                            for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                            {
                                if (tr.getPlacedClip(ci).getId() == clipId)
                                {
                                    found = true;
                                    break;
                                }
                            }
                            if (found)
                            {
                                break;
                            }
                        }
                        if (!found)
                        {
                            return false;
                        }
                        if (destTrack.has_value())
                        {
                            if (*destTrack == kInvalidTrackId
                                || snapBefore->findTrackIndexById(*destTrack) < 0)
                            {
                                return false;
                            }
                            session.moveClipToTrack(clipId, newStart, *destTrack);
                        }
                        else
                        {
                            session.moveClip(clipId, newStart);
                        }
                        const std::shared_ptr<const SessionSnapshot> snapAfter
                            = session.loadSessionSnapshotForAudioThread();
                        if (snapAfter == snapBefore)
                        {
                            return false;
                        }
                        syncViewportFromSession();
                        trackLanesView.syncTracksFromSession();
                        rulerView.repaint();
                        trackLanesView.repaint();
                        inspectorView_.refreshFromSession();
                        committed = true;
                        return true;
                    });
                return committed;
            });
        trackLanesView.setOnUndoableClipTrimRequested(
            [this](const PlacedClipId clipId, const ClipTrimEdge edge, const std::int64_t newVal) -> bool {
                if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
                {
                    return false;
                }
                if (clipId == kInvalidPlacedClipId)
                {
                    return false;
                }
                bool committed = false;
                executeUndoableSessionEdit(
                    "Trim clip",
                    [this, clipId, edge, newVal, &committed]() -> bool {
                        const std::shared_ptr<const SessionSnapshot> snapBefore
                            = session.loadSessionSnapshotForAudioThread();
                        if (snapBefore == nullptr)
                        {
                            return false;
                        }
                        bool found = false;
                        for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                        {
                            const Track& tr = snapBefore->getTrack(ti);
                            for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                            {
                                if (tr.getPlacedClip(ci).getId() == clipId)
                                {
                                    found = true;
                                    break;
                                }
                            }
                            if (found)
                            {
                                break;
                            }
                        }
                        if (!found)
                        {
                            return false;
                        }
                        if (edge == ClipTrimEdge::Left)
                        {
                            session.setClipLeftEdgeTrim(clipId, newVal);
                        }
                        else
                        {
                            session.setClipRightEdgeVisibleLength(clipId, newVal);
                        }
                        const std::shared_ptr<const SessionSnapshot> snapAfter
                            = session.loadSessionSnapshotForAudioThread();
                        if (snapAfter == snapBefore)
                        {
                            return false;
                        }
                        syncViewportFromSession();
                        trackLanesView.syncTracksFromSession();
                        rulerView.repaint();
                        trackLanesView.repaint();
                        inspectorView_.refreshFromSession();
                        committed = true;
                        return true;
                    });
                return committed;
            });
        trackLanesView.setActiveEditToolProvider([this]() { return currentEditTool_; });
        trackLanesView.setOnUndoableClipSplitRequested(
            [this](const PlacedClipId clipId,
                   const std::int64_t splitSample,
                   const bool clipWasSelected) {
                if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
                {
                    return;
                }
                if (clipId == kInvalidPlacedClipId)
                {
                    return;
                }
                std::optional<std::pair<PlacedClipId, PlacedClipId>> splitIds;
                executeUndoableSessionEdit(
                    "Split clip",
                    [this, clipId, splitSample, &splitIds]() -> bool {
                        const std::shared_ptr<const SessionSnapshot> snapBefore
                            = session.loadSessionSnapshotForAudioThread();
                        if (snapBefore == nullptr)
                        {
                            return false;
                        }
                        bool found = false;
                        for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                        {
                            const Track& tr = snapBefore->getTrack(ti);
                            for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                            {
                                if (tr.getPlacedClip(ci).getId() == clipId)
                                {
                                    found = true;
                                    break;
                                }
                            }
                            if (found)
                            {
                                break;
                            }
                        }
                        if (!found)
                        {
                            return false;
                        }
                        const auto maybe = session.splitClip(clipId, splitSample);
                        if (!maybe.has_value())
                        {
                            return false;
                        }
                        const std::shared_ptr<const SessionSnapshot> snapAfter
                            = session.loadSessionSnapshotForAudioThread();
                        if (snapAfter == snapBefore)
                        {
                            return false;
                        }
                        splitIds = *maybe;
                        syncViewportFromSession();
                        trackLanesView.syncTracksFromSession();
                        rulerView.repaint();
                        trackLanesView.repaint();
                        inspectorView_.refreshFromSession();
                        return true;
                    });
                if (!clipWasSelected || !splitIds.has_value())
                {
                    return;
                }
                const PlacedClipId rightId = splitIds->second;
                const std::shared_ptr<const SessionSnapshot> snap
                    = session.loadSessionSnapshotForAudioThread();
                if (snap == nullptr)
                {
                    return;
                }
                for (int ti = 0; ti < snap->getNumTracks(); ++ti)
                {
                    const Track& tr = snap->getTrack(ti);
                    for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                    {
                        if (tr.getPlacedClip(ci).getId() == rightId)
                        {
                            trackLanesView.selectPlacedClipOnTrack(tr.getId(), rightId);
                            return;
                        }
                    }
                }
            });
        // UI-only mutex with the instrument timeline header row. Audio headers paint inactive
        // when the instrument row is the UI-active row; clicking any audio header clears it.
        // No `Session` change — `Session::activeTrackId_` semantics for Add Clip etc. unchanged.
        trackLanesView.setHeaderActiveSuppressProvider(
            [this] { return instrumentRuntimeCoordinator_->hasAnyKeyedInstrumentControllerActive(); });
        trackLanesView.setOnAudioHeaderActivated(
            [this] { instrumentRuntimeCoordinator_->deactivateKeyedInstrumentControllersOnly(); });
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
    void invokeRecordToggleFromWindowShortcut() { recordingCoordinator_->numpadRecordToggled(); }
    // [Message thread] Space: when recording, commit (source tag `space`); else same as Play/Pause.
    void invokePlayPauseToggleFromWindowShortcut()
    {
        if (recorder_.isRecording())
        {
            recordingCoordinator_->stopRecordingAndCommitFromUi("space");
            return;
        }
        togglePlayPauseTransportOnly();
    }

    void invokeJumpToLeftLocatorFromWindowShortcut()
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

    void invokeDeleteSelectedPlacedClipFromWindowShortcut()
    {
        if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
        {
            return;
        }
        const std::optional<std::pair<TrackId, PlacedClipId>> sel
            = trackLanesView.getAggregatedSelectedClip();
        if (!sel.has_value())
        {
            return;
        }
        const TrackId tid = sel->first;
        const PlacedClipId pid = sel->second;
        const std::shared_ptr<const SessionSnapshot> snap
            = session.loadSessionSnapshotForAudioThread();
        const int ti = (snap != nullptr) ? snap->findTrackIndexById(tid) : -1;
        if (ti < 0)
        {
            return;
        }
        const Track& tr = snap->getTrack(ti);
        bool found = false;
        for (int i = 0; i < tr.getNumPlacedClips(); ++i)
        {
            if (tr.getPlacedClip(i).getId() == pid)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return;
        }
        executeUndoableSessionEdit(
            "Delete event",
            [this, tid, pid]() -> bool {
                session.removePlacedClip(tid, pid);
                trackLanesView.notifyPlacedClipRemoved(tid, pid);
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
                rulerView.repaint();
                trackLanesView.repaint();
                inspectorView_.refreshFromSession();
                return true;
            });
    }

    void invokeCopySelectedClipFromWindowShortcut()
    {
        const std::optional<std::pair<TrackId, PlacedClipId>> sel
            = trackLanesView.getAggregatedSelectedClip();
        if (!sel.has_value())
        {
            return;
        }
        const std::shared_ptr<const SessionSnapshot> snap
            = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return;
        }
        const int tIdx = snap->findTrackIndexById(sel->first);
        if (tIdx < 0)
        {
            return;
        }
        const Track& tr = snap->getTrack(tIdx);
        for (int i = 0; i < tr.getNumPlacedClips(); ++i)
        {
            const PlacedClip& p = tr.getPlacedClip(i);
            if (p.getId() != sel->second)
            {
                continue;
            }
            InternalClipPasteboard pb;
            pb.material = p.getMaterial();
            pb.leftTrimSamples = p.getLeftTrimSamples();
            pb.visibleLengthSamples = p.getEffectiveLengthSamples();
            pb.materialWindowStartSamples = p.getMaterialWindowStartSamples();
            pb.materialWindowEndExclusiveSamples = p.getMaterialWindowEndExclusiveSamples();
            clipPasteboard_ = std::move(pb);
            return;
        }
    }

    void invokePasteClipFromWindowShortcut()
    {
        if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
        {
            return;
        }
        if (!clipPasteboard_.has_value())
        {
            return;
        }
        const TrackId target = session.getActiveTrackId();
        if (target == kInvalidTrackId)
        {
            return;
        }
        const std::shared_ptr<const SessionSnapshot> snap
            = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr || snap->findTrackIndexById(target) < 0)
        {
            return;
        }
        const InternalClipPasteboard pb = *clipPasteboard_;
        if (pb.material == nullptr || pb.visibleLengthSamples <= 0)
        {
            return;
        }
        executeUndoableSessionEdit(
            "Paste clip",
            [this, target, pb]() -> bool {
                const juce::Result r = session.addPlacedClipFromExistingMaterial(
                    pb.material,
                    transport.readPlayheadSamplesForUi(),
                    pb.leftTrimSamples,
                    pb.visibleLengthSamples,
                    target,
                    pb.materialWindowStartSamples,
                    pb.materialWindowEndExclusiveSamples);
                if (!r.wasOk())
                {
                    return false;
                }
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
                trackLanesView.selectFrontPlacedClipOnTrack(target);
                rulerView.repaint();
                trackLanesView.repaint();
                inspectorView_.refreshFromSession();
                return true;
            });
    }

    void invokeUndoFromWindowShortcut()
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeUndoFromWindowShortcut entered undoSize="
                + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                + juce::String(sessionHistory_.redoStackSize()));
        }
        if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeUndo bail: recordingOrCountIn recording="
                    + juce::String(recorder_.isRecording() ? "Y" : "n")
                    + " countIn=" + juce::String(recordingCoordinator_->isCountInActive() ? "Y" : "n"));
            }
            return;
        }
        if (trackLanesView.isClipEditGestureInProgress())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] invokeUndo bail: gestureInProgress");
            }
            return;
        }
        pluginHost_.flushOpenEditorParameterUndoSteps();
        const std::optional<SessionHistoryRestoreBundle> bundle = sessionHistory_.popUndo();
        if (!bundle.has_value() || bundle->timelineSnapshot == nullptr)
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeUndo bail: emptyOrNullBundle hasValue="
                    + juce::String(bundle.has_value() ? "Y" : "n") + " timelineNull="
                    + juce::String(
                        (bundle.has_value() && bundle->timelineSnapshot == nullptr) ? "Y" : "n"));
            }
            return;
        }
        {
            const std::shared_ptr<const SessionSnapshot> live
                = session.loadSessionSnapshotForAudioThread();
            const std::int64_t curL = live ? live->getLeftLocatorSamples() : 0;
            const std::int64_t curR = live ? live->getRightLocatorSamples() : 0;
            const std::shared_ptr<const SessionSnapshot> restoredWithLocators
                = SessionSnapshot::withLocators(*bundle->timelineSnapshot, curL, curR);
            session.restoreSessionSnapshotForUndo(restoredWithLocators);
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeUndo restored timeline="
                    + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " liveBeforeRestore="
                    + undoDiagSnapPtr(live.get()));
            }
        }
        if (bundle->pluginSides.has_value())
        {
            pluginHost_.importChain(bundle->pluginSides->trackId, bundle->pluginSides->before);
        }
        if (bundle->instrumentSides.has_value())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                ExperimentalMidiEditorWindow* midiEditorWnd = nullptr;
                if (midiEditorPresenter_ != nullptr)
                    midiEditorWnd = midiEditorPresenter_->midiEditorWindow();
                if (midiEditorWnd != nullptr)
                {
                    const auto preId = midiEditorWnd->getBoundInstrumentClipId();
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeUndo pre instrument apply storedEditorClipId="
                        + (preId.has_value() ? juce::String(static_cast<juce::int64>(*preId))
                                             : juce::String("none")));
                }
            }
            const std::vector<ProjectFileExperimentalInstrumentTrackV1>& mus
                = bundle->isRedo ? bundle->instrumentSides->after : bundle->instrumentSides->before;
            applyInstrumentMusicalUndoVectorToAllKeyedControllers(mus);
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->rebindAfterInstrumentMusicalUndo();
            }
        }
        refreshAfterSessionSnapshotRestore();
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            const auto liveNow = session.loadSessionSnapshotForAudioThread();
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeUndo complete liveNow=" + undoDiagSnapPtr(liveNow.get()) + " undoSize="
                + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                + juce::String(sessionHistory_.redoStackSize()));
        }
    }

    void invokeRedoFromWindowShortcut()
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeRedoFromWindowShortcut entered undoSize="
                + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                + juce::String(sessionHistory_.redoStackSize()));
        }
        if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeRedo bail: recordingOrCountIn recording="
                    + juce::String(recorder_.isRecording() ? "Y" : "n")
                    + " countIn=" + juce::String(recordingCoordinator_->isCountInActive() ? "Y" : "n"));
            }
            return;
        }
        if (trackLanesView.isClipEditGestureInProgress())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] invokeRedo bail: gestureInProgress");
            }
            return;
        }
        const std::optional<SessionHistoryRestoreBundle> bundle = sessionHistory_.popRedo();
        if (!bundle.has_value() || bundle->timelineSnapshot == nullptr)
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeRedo bail: emptyOrNullBundle hasValue="
                    + juce::String(bundle.has_value() ? "Y" : "n") + " timelineNull="
                    + juce::String(
                        (bundle.has_value() && bundle->timelineSnapshot == nullptr) ? "Y" : "n"));
            }
            return;
        }
        {
            const std::shared_ptr<const SessionSnapshot> live
                = session.loadSessionSnapshotForAudioThread();
            const std::int64_t curL = live ? live->getLeftLocatorSamples() : 0;
            const std::int64_t curR = live ? live->getRightLocatorSamples() : 0;
            const std::shared_ptr<const SessionSnapshot> restoredWithLocators
                = SessionSnapshot::withLocators(*bundle->timelineSnapshot, curL, curR);
            session.restoreSessionSnapshotForUndo(restoredWithLocators);
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeRedo restored timeline="
                    + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " liveBeforeRestore="
                    + undoDiagSnapPtr(live.get()));
            }
        }
        if (bundle->pluginSides.has_value())
        {
            pluginHost_.importChain(bundle->pluginSides->trackId, bundle->pluginSides->after);
        }
        if (bundle->instrumentSides.has_value())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                ExperimentalMidiEditorWindow* midiEditorWnd = nullptr;
                if (midiEditorPresenter_ != nullptr)
                    midiEditorWnd = midiEditorPresenter_->midiEditorWindow();
                if (midiEditorWnd != nullptr)
                {
                    const auto preId = midiEditorWnd->getBoundInstrumentClipId();
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeRedo pre instrument apply storedEditorClipId="
                        + (preId.has_value() ? juce::String(static_cast<juce::int64>(*preId))
                                             : juce::String("none")));
                }
            }
            const std::vector<ProjectFileExperimentalInstrumentTrackV1>& mus
                = bundle->isRedo ? bundle->instrumentSides->after : bundle->instrumentSides->before;
            applyInstrumentMusicalUndoVectorToAllKeyedControllers(mus);
            if (midiEditorPresenter_ != nullptr)
            {
                midiEditorPresenter_->rebindAfterInstrumentMusicalUndo();
            }
        }
        refreshAfterSessionSnapshotRestore();
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            const auto liveNow = session.loadSessionSnapshotForAudioThread();
            writeUndoDiagnosticLogLine(
                "[UndoDiag] invokeRedo complete liveNow=" + undoDiagSnapPtr(liveNow.get()) + " undoSize="
                + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                + juce::String(sessionHistory_.redoStackSize()));
        }
    }

    void setKeyDiagnosticLine(const juce::String& line)
    {
        if (kShowKeyDiagnostic)
        {
            keyDiagLabel_.setText(line, juce::dontSendNotification);
        }
    }

    void setShortcutDiagVisibleCaption(const juce::String& line)
    {
        if constexpr (kShowShortcutDiagnostics)
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
        if (kShowKeyDiagnostic)
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
        if constexpr (kShowShortcutDiagnostics)
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
    struct InternalClipPasteboard
    {
        std::shared_ptr<const AudioClip> material;
        std::int64_t leftTrimSamples = 0;
        std::int64_t visibleLengthSamples = 0;
        std::int64_t materialWindowStartSamples = 0;
        std::int64_t materialWindowEndExclusiveSamples = 0;
    };

    std::optional<InternalClipPasteboard> clipPasteboard_;

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
        if (recorder_.isRecording() || recordingCoordinator_->isCountInActive())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Audio Settings",
                "Audio settings cannot be changed while recording or count-in is active.");
            return;
        }
        if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
        {
            transport.requestPlaybackIntent(PlaybackIntent::Stopped);
            updatePlayPauseButtonFromTransport();
        }
        auto* body = new AudioSettingsDialogContent(deviceManager, latencyStore_, playbackEngine_);
        audioLatencySettingsWeak_ = &body->getLatencyPane();
        body->getLatencyPane().syncFromStore();
        juce::DialogWindow::LaunchOptions opt;
        opt.content.setOwned(body);
        opt.dialogTitle = "Audio Settings";
        opt.dialogBackgroundColour
            = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
        opt.componentToCentreAround = this;
        opt.escapeKeyTriggersCloseButton = true;
        opt.useNativeTitleBar = true;
        opt.resizable = true;
        opt.launchAsync();
    }

    void showHelpMenuPopup()
    {
        juce::PopupMenu menu;
        constexpr int kUndoBehaviorMenuItemId = 1;
        menu.addItem(kUndoBehaviorMenuItemId, "Undo Behavior...");
        juce::Component::SafePointer<TransportControlsContent> safeThis(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&helpButton),
            [safeThis, kUndoBehaviorMenuItemId](const int result) {
                if (safeThis == nullptr)
                {
                    return;
                }
                if (result != kUndoBehaviorMenuItemId)
                {
                    return;
                }
                safeThis->showUndoBehaviorDialog();
            });
    }

    void showUndoBehaviorDialog()
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Undo Behavior",
            undoBehaviorHelpBodyText());
    }

    [[nodiscard]] static juce::String undoBehaviorHelpBodyText()
    {
        return juce::String(
            "Undo will restore previous session/timeline states, such as:\n"
            "  - clip moves\n"
            "  - clip trims\n"
            "  - split clips\n"
            "  - pasted clips\n"
            "  - deleted events\n"
            "  - deleted tracks\n"
            "  - track mute / off / fader changes\n"
            "  - locator / range edits\n"
            "\n"
            "Undo will NOT automatically delete or restore external files on disk.\n"
            "\n"
            "For safety:\n"
            "  - recorded audio files in the project Audio/ folder remain on disk\n"
            "  - imported audio files copied into Audio/ remain on disk\n"
            "  - undoing a recording or import placement may remove the timeline event,\n"
            "    but not the underlying audio file\n"
            "  - cleanup of unused files will be a separate future command\n"
            "    (e.g. \"Clean Unused Media\")\n"
            "\n"
            "Note: undo/redo is not implemented yet. This dialog explains the planned behavior.");
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

    // [Message thread] After `Session::restoreSessionSnapshotForUndo` / redo: playhead, cycle
    // toggle, and L/R locator samples are not session-undo state for this pass (`withLocators`
    // reapplies live locators on purpose). Clear clip UI gestures / caches so stale drag ghosts
    // and raster fingerprints cannot survive restore.
    void refreshAfterSessionSnapshotRestore()
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] refreshAfterSessionSnapshotRestore: cancel UI for snapshot restore");
        }
        trackLanesView.cancelAllClipGesturesAndTransientUiState();
        recordingCoordinator_->reconcileCycleBookingAfterUndoSnapshotRestore();
        syncViewportFromSession();
        trackLanesView.syncTracksFromSession();
        rulerView.repaint();
        trackLanesView.repaint();
        refreshInstrumentUi();
        inspectorView_.refreshFromSession();
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] refreshAfterSessionSnapshotRestore complete");
        }
    }

    // [Message thread] Undo-1: mutator must return false when no session mutation occurred
    // (e.g. paste `Result::fail`). `Session::removePlacedClip` / `removeTrack` always allocate a
    // new `SessionSnapshot` instance even on semantic no-op, so we only wrap paths that
    // pre-validate the target id (delete event / delete track) or use `Result::wasOk()` (paste).
    template <typename F>
    void executeUndoableSessionEdit(const juce::String& label, F&& mutator)
    {
        static_assert(std::is_invocable_r_v<bool, F>);
        std::shared_ptr<const SessionSnapshot> before = session.loadSessionSnapshotForAudioThread();
        if (before == nullptr)
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit skip: null before label=\""
                                         + label + "\"");
            }
            return;
        }
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit mutator run label=\"" + label
                                       + "\" before=" + undoDiagSnapPtr(before.get()) + " undoSize="
                                       + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                                       + juce::String(sessionHistory_.redoStackSize()));
        }
        if (!mutator())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] executeUndoableSessionEdit mutator=false label=\"" + label + "\"");
            }
            return;
        }
        std::shared_ptr<const SessionSnapshot> after = session.loadSessionSnapshotForAudioThread();
        if (after == nullptr)
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit skip: null after label=\""
                                           + label + "\"");
            }
            return;
        }
        const SessionSnapshot* const afterPtrForDiag = after.get();
        sessionHistory_.record(label, std::move(before), std::move(after));
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit after record label=\"" + label
                                       + "\" after=" + undoDiagSnapPtr(afterPtrForDiag) + " undoSize="
                                       + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                                       + juce::String(sessionHistory_.redoStackSize()));
        }
    }

    template <typename F>
    void executeUndoableInstrumentEdit(const juce::String& label, F&& mutator)
    {
        static_assert(std::is_invocable_r_v<bool, F>);
        std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] executeUndoableInstrumentEdit skip: null snap label=\"" + label + "\"");
            }
            return;
        }
        std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical = buildSortedInstrumentMusicalUndoSnapshot();
        stableSortInstrumentMusicalUndoVector(beforeMusical);
        if (beforeMusical.empty())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] executeUndoableInstrumentEdit skip: empty before label=\"" + label + "\"");
            }
            return;
        }
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit mutator run label=\"" + label
                                       + "\"");
        }
        if (!mutator())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] executeUndoableInstrumentEdit mutator=false label=\"" + label + "\"");
            }
            return;
        }
        std::vector<ProjectFileExperimentalInstrumentTrackV1> afterMusical = buildSortedInstrumentMusicalUndoSnapshot();
        stableSortInstrumentMusicalUndoVector(afterMusical);
        if (afterMusical.empty())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit skip: empty after label=\""
                                           + label + "\"");
            }
            return;
        }
        if (experimentalInstrumentTracksMusicalUndoEqual(beforeMusical, afterMusical))
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] executeUndoableInstrumentEdit skip: musicalEqual label=\"" + label + "\"");
            }
            return;
        }
        sessionHistory_.record(label,
                               snap,
                               snap,
                               std::nullopt,
                               InstrumentUndoStepSides{ std::move(beforeMusical), std::move(afterMusical) });
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit recorded label=\"" + label
                                       + "\" undoSize=" + juce::String(sessionHistory_.undoStackSize())
                                       + " redoSize=" + juce::String(sessionHistory_.redoStackSize()));
        }
    }

    void commitInstrumentMusicalUndoPair(
        const juce::String& label,
        std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical)
    {
        stableSortInstrumentMusicalUndoVector(beforeMusical);
        if (beforeMusical.empty())
        {
            return;
        }
        std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return;
        }
        std::vector<ProjectFileExperimentalInstrumentTrackV1> afterMusical = buildSortedInstrumentMusicalUndoSnapshot();
        stableSortInstrumentMusicalUndoVector(afterMusical);
        if (afterMusical.empty() || experimentalInstrumentTracksMusicalUndoEqual(beforeMusical, afterMusical))
        {
            return;
        }
        sessionHistory_.record(label,
                               snap,
                               snap,
                               std::nullopt,
                               InstrumentUndoStepSides{ std::move(beforeMusical), std::move(afterMusical) });
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

    // [Message thread] Presents a native file dialog; on success, new clip is placed on the
    // **session** timeline at the current `Transport` playhead (read once, here, not on audio).
    void addClipAtPlayheadClicked()
    {
        if (!session.hasKnownProjectFile())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Add clip",
                "Save the project before importing audio.");
            return;
        }
        if (importInFlight_)
        {
            return;
        }
        importInFlight_ = true;

        const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles;

        auto chooser = std::make_shared<juce::FileChooser>(
            "Add audio at playhead",
            juce::File{},
            "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

        // JUCE: async dialog; the lambda runs on the *message* thread when the user dismisses
        // the picker. We record playhead and decode in this callback — the agreed “at add
        // time” read for placement (not the audio thread).
        chooser->launchAsync(fileChooserFlags, [this, chooser](const juce::FileChooser& fc) {
            juce::ignoreUnused(chooser);
            struct ClearImportInFlight
            {
                bool& b;
                explicit ClearImportInFlight(bool& ref) noexcept
                    : b(ref)
                {
                }
                ~ClearImportInFlight() { b = false; }
            } clearImport{importInFlight_};

            const juce::File file = fc.getResult();
            if (!file.existsAsFile())
            {
                // Cancel or empty selection — not an error, keep the current session.
                return;
            }

            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Audio",
                    "No active audio device. Cannot validate sample rate for load.");
                return;
            }

            // Snapshot once: this value becomes `PlacedClip::startSampleOnTimeline` for the
            // new row (see Session / `PHASE_PLAN` add-at-playhead).
            const std::int64_t startSampleOnTimeline = transport.readPlayheadSamplesForUi();

            // Loader must match the *running* device rate (Phase 1 contract).
            const double sampleRate = device->getCurrentSampleRate();

            const juce::File audioDir = mini_daw::getProjectAudioDir(session.getCurrentProjectFolder());
            juce::File pathToUse;
            const juce::Result importRes
                = mini_daw::importAudioIntoProjectAudioDir(file, audioDir, pathToUse);
            if (!importRes.wasOk())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Could not import audio",
                    importRes.getErrorMessage());
                return;
            }
            executeUndoableSessionEdit("Import clip", [&]() -> bool {
                const juce::Result loadResult = session.addClipFromFileAtPlayhead(
                    pathToUse, sampleRate, startSampleOnTimeline);
                if (!loadResult.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Could not open file",
                        loadResult.getErrorMessage());
                    return false;
                }
                // New **front** clip is on the active track; playhead/transport are unchanged.
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
                rulerView.repaint();
                trackLanesView.repaint();
                inspectorView_.refreshFromSession();
                return true;
            });
        });
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

    SessionHistory sessionHistory_;

    /// When Audio Settings is open; auto-clears when the dialog-owned view is destroyed.
    juce::Component::SafePointer<LatencySettingsView> audioLatencySettingsWeak_;
    /// Count-in / recording line (no always-visible audio device debug; use Audio...).
    juce::Label countInStatusLabel_;
    std::unique_ptr<RecordingCoordinator> recordingCoordinator_;
    std::unique_ptr<Vst3PluginPickerCoordinator> vst3PluginPickerCoordinator_;

    /// Set while a file chooser for Add clip is in flight; blocks overlapping Add clip clicks.
    bool importInFlight_ = false;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportControlsContent)
};

TrackId TransportControlsContent::canonicalInstrumentLaneTrackIdFromSession() const noexcept
{
    return instrumentRuntimeCoordinator_->canonicalInstrumentLaneTrackIdFromSession();
}

bool TransportControlsContent::anyHeldExperimentalHostShowsGrooveAgentLoaded() const noexcept
{
    return instrumentRuntimeCoordinator_->anyHeldHostShowsGrooveAgentLoaded();
}

ExperimentalInstrumentHost* TransportControlsContent::getInstrumentHostForTrack(const TrackId tid) const noexcept
{
    return instrumentRuntimeCoordinator_->getInstrumentHostForTrack(tid);
}

InstrumentTrackController* TransportControlsContent::getInstrumentControllerForTrack(const TrackId tid) const noexcept
{
    return instrumentRuntimeCoordinator_->getInstrumentControllerForTrack(tid);
}

std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
TransportControlsContent::getOrCreateInstrumentRuntimeForTrack(const TrackId tid)
{
    return instrumentRuntimeCoordinator_->getOrCreateInstrumentRuntimeForTrack(tid);
}

std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
TransportControlsContent::getExperimentRuntimePairForGrooveAdds()
{
    return instrumentRuntimeCoordinator_->getExperimentRuntimePairForGrooveAdds();
}

void TransportControlsContent::promoteInstrumentStagingIntoRegistryBoundTo(const TrackId tid)
{
    instrumentRuntimeCoordinator_->promoteInstrumentStagingIntoRegistryBoundTo(tid);
}

void TransportControlsContent::removeInstrumentRuntimeForTrack(const TrackId tid) noexcept
{
    instrumentRuntimeCoordinator_->removeInstrumentRuntimeForTrack(tid);
}

void TransportControlsContent::clearExperimentalInstrumentRuntimesPreserveBridgeOnly() noexcept
{
    if (midiEditorPresenter_ != nullptr)
    {
        midiEditorPresenter_->resetWindowAndBooking();
    }
    trackLanesView.syncInstrumentTimelineAttachments({});
    instrumentTimelineRowCoordinator_->clearInstrumentTimelineLanesAndHeaders();
    instrumentRuntimeCoordinator_->clearRuntimesPreserveBridgeOnly();
}

void TransportControlsContent::experimentalBeginAudioBlockAllHosts(const std::int64_t numSamples) noexcept
{
    instrumentRuntimeCoordinator_->experimentalBeginAudioBlockAllHosts(numSamples);
}

void TransportControlsContent::prepareExperimentalInstrumentHostsForDevice(const double sampleRate,
                                                                          const int blockSamples) noexcept
{
    instrumentRuntimeCoordinator_->prepareExperimentalInstrumentHostsForDevice(sampleRate, blockSamples);
}

void TransportControlsContent::releaseExperimentalInstrumentHostsDeviceResources() noexcept
{
    instrumentRuntimeCoordinator_->releaseExperimentalInstrumentHostsDeviceResources();
}

void TransportControlsContent::updateExperimentalPlaybackBridgeAfterRegistryChange()
{
    instrumentRuntimeCoordinator_->updateExperimentalPlaybackBridgeAfterRegistryChange();
}

juce::String TransportControlsContent::proposeNextGrooveAgentInstrumentTrackDisplayName()
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

bool TransportControlsContent::tryCloneGrooveAgentFromAnyExistingInto(
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
TransportControlsContent::buildSortedInstrumentMusicalUndoSnapshot() const
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
        InstrumentTrackController* const ctl = const_cast<TransportControlsContent*>(this)->getInstrumentControllerForTrack(
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

void TransportControlsContent::stableSortInstrumentMusicalUndoVector(
    std::vector<ProjectFileExperimentalInstrumentTrackV1>& v)
{
    std::stable_sort(
        v.begin(),
        v.end(),
        [](const ProjectFileExperimentalInstrumentTrackV1& a,
           const ProjectFileExperimentalInstrumentTrackV1& b) noexcept -> bool { return a.trackId < b.trackId; });
}

void TransportControlsContent::applyInstrumentMusicalUndoVectorToAllKeyedControllers(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept
{
    instrumentRuntimeCoordinator_->applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(tracks);
}

} // namespace

MainWindow::MainWindow(const juce::String& name,
           Transport& transport,
           Session& session,
           PluginInsertHost& pluginInsertHost,
           juce::AudioDeviceManager& deviceManager,
           RecorderService& recorderService,
           CountInClickOutput& countInClicks,
           LatencySettingsStore& latencyStore,
           PlaybackEngine& playbackEngine)
    : DocumentWindow(
          name,
          juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
              juce::ResizableWindow::backgroundColourId),
          DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(
        new TransportControlsContent(transport,
                                     session,
                                     pluginInsertHost,
                                     deviceManager,
                                     recorderService,
                                     countInClicks,
                                     latencyStore,
                                     playbackEngine),
        true);
    setResizable(true, true);
    setResizeLimits(320, 240, 10000, 10000);
    centreWithSize(640, 400);
    addKeyListener(this);
    if (juce::Component* c = getContentComponent())
    {
        c->setWantsKeyboardFocus(true);
    }
    juce::MessageManager::callAsync([this] {
        if (juce::Component* c = getContentComponent())
        {
            c->grabKeyboardFocus();
        }
    });
    setVisible(true);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] enabled");
    }
}

MainWindow::~MainWindow()
{
    removeKeyListener(this);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainWindow::activeWindowStatusChanged()
{
    juce::DocumentWindow::activeWindowStatusChanged();
    if (isActiveWindow())
    {
        juce::MessageManager::callAsync([this] {
            if (juce::Component* c = getContentComponent())
            {
                c->grabKeyboardFocus();
            }
        });
    }
}

bool MainWindow::keyPressed(const juce::KeyPress& key, juce::Component* originating)
{
    juce::ignoreUnused(originating);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] MainWindow::keyPressed desc=\""
                                   + key.getTextDescription() + "\"");
    }
    if (kShowKeyDiagnostic)
    {
        if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
        {
            tcc->setKeyDiagnosticLine(
                juce::String{"0x" } + juce::String::toHexString((juce::uint32)key.getKeyCode())
                + " ch=0x" + juce::String::toHexString((juce::uint32)key.getTextCharacter()) + " "
                + key.getTextDescription());
        }
    }
    return routeShortcut(key);
}

bool MainWindow::routeShortcut(const juce::KeyPress& key)
{
    logShortcutRouterKey(key);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        const bool cmd = key.getModifiers().isCommandDown();
        const bool z = (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
        const bool y = (key.getKeyCode() == 'y' || key.getKeyCode() == 'Y');
        const bool undoCombo = cmd && !key.getModifiers().isShiftDown() && z;
        const bool redoCombo = cmd && (y || (key.getModifiers().isShiftDown() && z));
        if (undoCombo || redoCombo)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] routeShortcut entered undoRelated desc=\"" + key.getTextDescription()
                + "\" undoCombo=" + juce::String(undoCombo ? "Y" : "n") + " redoCombo="
                + juce::String(redoCombo ? "Y" : "n") + ")");
        }
    }
    if constexpr (kShowShortcutDiagnostics)
    {
        if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
        {
            tcc->setShortcutDiagVisibleCaption(makeShortcutDiagVisibleCaption(key));
        }
    }

    const bool editorHasFocus
        = (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())
           != nullptr);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        const bool undoShortcut = key.getModifiers().isCommandDown()
                                  && !key.getModifiers().isShiftDown()
                                  && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
        const bool redoShortcut
            = key.getModifiers().isCommandDown()
              && ((key.getKeyCode() == 'y' || key.getKeyCode() == 'Y')
                  || ((key.getKeyCode() == 'z' || key.getKeyCode() == 'Z')
                      && key.getModifiers().isShiftDown()));
        if (editorHasFocus && (undoShortcut || redoShortcut))
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] routeShortcut blocked: TextEditor focus undoShortcut="
                + juce::String(undoShortcut ? "Y" : "n") + " redoShortcut="
                + juce::String(redoShortcut ? "Y" : "n") + " desc=\""
                + key.getTextDescription() + "\"");
        }
    }
    if (!editorHasFocus)
    {
        if (key.isKeyCode(juce::KeyPress::deleteKey))
        {
            if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
            {
                tcc->invokeDeleteSelectedPlacedClipFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'c' || key.getKeyCode() == 'C'))
        {
            if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
            {
                tcc->invokeCopySelectedClipFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'v' || key.getKeyCode() == 'V'))
        {
            if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
            {
                tcc->invokePasteClipFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
            && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z'))
        {
            if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] routeShortcut matched=undo desc=\""
                                               + key.getTextDescription() + "\"");
                }
                tcc->invokeUndoFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown()
            && ((key.getKeyCode() == 'y' || key.getKeyCode() == 'Y')
                || ((key.getKeyCode() == 'z' || key.getKeyCode() == 'Z')
                    && key.getModifiers().isShiftDown())))
        {
            if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] routeShortcut matched=redo desc=\""
                                               + key.getTextDescription() + "\"");
                }
                tcc->invokeRedoFromWindowShortcut();
                return true;
            }
        }
    }

    if (midi_transport_shortcuts::isRecordToggleShortcut(key))
    {
        if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
        {
            tcc->invokeRecordToggleFromWindowShortcut();
            juce::Logger::writeToLog(
                juce::String{"[Shortcut] record toggle: "} + key.getTextDescription());
            return true;
        }
        return false;
    }
    if (midi_transport_shortcuts::isJumpToLeftLocatorShortcut(key))
    {
        if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
        {
            tcc->invokeJumpToLeftLocatorFromWindowShortcut();
            juce::Logger::writeToLog(juce::String{"[Shortcut] jump to left locator (numpad1 / top-row "
                                                  "1 / VK): "}
                                     + key.getTextDescription());
            return true;
        }
        return false;
    }
    if (midi_transport_shortcuts::isSpacePlayPauseShortcut(key))
    {
        if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
        {
            tcc->invokePlayPauseToggleFromWindowShortcut();
            juce::Logger::writeToLog(
                juce::String{"[Shortcut] play/pause: "} + key.getTextDescription());
            return true;
        }
        return false;
    }
    return false;
}

[[nodiscard]] std::unique_ptr<MainWindow> createMainWindow(
    const juce::String& name,
    Transport& transport,
    Session& session,
    PluginInsertHost& pluginInsertHost,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorderService,
    CountInClickOutput& countInClicks,
    LatencySettingsStore& latencyStore,
    PlaybackEngine& playbackEngine)
{
    return std::make_unique<MainWindow>(
        name, transport, session, pluginInsertHost, deviceManager, recorderService, countInClicks,
        latencyStore, playbackEngine);
}
