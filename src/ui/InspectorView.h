#pragma once

#include "domain/Track.h"
#include "plugins/InsertSlotId.h"

#include "ui/InspectorPanControl.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h> // juce::BubbleMessageComponent

class Session;

/// One occupied insert row for Inspector (mirrors host row data; keeps Inspector independent of PluginInsertHost).
struct InspectorInsertRow
{
    InsertSlotId slotId = kInvalidInsertSlotId;
    InsertStage stage = InsertStage::Post;
    juce::String displayName;
};

/// [Message thread] Optional plugin-insert actions for the active track (wired from Main).
struct InspectorPluginHost
{
    std::function<bool(TrackId)> hasAnyInsert;
    std::function<std::vector<InspectorInsertRow>(TrackId)> getInsertRows;
    std::function<void(TrackId, InsertStage)> requestAdd;
    std::function<void(TrackId, InsertSlotId)> requestEdit;
    std::function<void(TrackId, InsertSlotId)> requestRemove;
    /// `gapIndexInTargetStage` in [0, targetStageCount] (see PluginInsertHost::moveInsertToStageAtGap).
    std::function<void(TrackId, InsertSlotId, InsertStage, int)> requestMoveToStageAtGap;
    /// `gapIndexInStage` is the visual gap in [0, stageCount] before removal (see PluginInsertHost::reorderInsertWithinStage).
    std::function<void(TrackId, InsertSlotId, int)> requestReorderInStage;
};

// P2: the former InspectorProxyHost / InspectorSecondaryHost sections moved to the track-header
// "Instrument alternatives" popup (see ui/InstrumentAlternativesPopup.h).

/// [Message thread] Snapshot of the ACTIVE audio device's input channels for the "Audio Input"
/// selector (wired from Main; the Inspector never touches the device manager directly).
/// `physicalInputNames` is indexed by PHYSICAL channel; `activeInputChannels` marks which of them
/// are enabled in the current device configuration (only those are selectable — a disabled
/// channel cannot be delivered by the audio callback).
struct InspectorAudioInputDeviceSnapshot
{
    juce::StringArray physicalInputNames;
    juce::BigInteger activeInputChannels;
    bool deviceAvailable = false;
};

/// [Message thread] One MIDI input device for the "MIDI Input" selector (wired from Main via the
/// live-MIDI coordinator). `present == false` marks the row's saved device that is not connected
/// right now — listed so the assignment stays visible, never silently replaced.
struct InspectorMidiInputDeviceOption
{
    juce::String identifier;
    juce::String name;
    bool present = true;
};

/// [Message thread] Live-MIDI snapshot for the selected row: devices to list and a status line.
struct InspectorMidiInputSnapshot
{
    std::vector<InspectorMidiInputDeviceOption> devices;
    /// Empty when nominal; otherwise e.g. "MIDI device missing: …", "No playable instrument …".
    juce::String statusLine;
};

/// Active-track-only controls (Cubase-style Inspector), not repeated in every track header.

class InspectorView final : public juce::Component,
                            public juce::DragAndDropContainer,
                            public juce::DragAndDropTarget,
                            private juce::TextEditor::Listener
{
    class InsertSlotButton;
    class StageDropTarget;

public:
    explicit InspectorView(Session& session);
    ~InspectorView() override;

    /// [Message thread] Sync from current snapshot / `getActiveTrackId` (safe to poll).

    void refreshFromSession();

    void setInspectorPluginHost(InspectorPluginHost host) noexcept { pluginHost_ = std::move(host); }

    /// [Message thread] Undoable rename (`TrackLanesEditCoordinator`). Empty default = inspector name field commits as no-op.
    void setRenameTrackHandler(std::function<bool(TrackId, juce::String)> fn) noexcept
    {
        renameTrackHandler_ = std::move(fn);
    }

    /// [Message thread] Undoable **audio** output routing (`TrackLanesEditCoordinator`).
    void setRoutedOutputHandler(std::function<void(TrackId, TrackId)> fn) noexcept
    {
        routedOutputHandler_ = std::move(fn);
    }

    /// [Message thread] Undoable pre-gain edit in dB (`TrackLanesEditCoordinator`): applied before
    /// the track's Pre inserts and fader — audio rows only (the control is hidden elsewhere).
    void setPreGainHandler(std::function<void(TrackId, float)> fn) noexcept
    {
        preGainHandler_ = std::move(fn);
    }

    /// [Message thread] Stability-scenario surface for the real user flow: feeds `text` to the
    /// pre-gain field through `TextEditor::keyPressed` (Ctrl+A, the characters, Return) exactly as
    /// typed keys arrive from the peer. Return is delivered by the editor as an asynchronous
    /// command message, so the commit (listener → handler → undoable Session edit) lands on a later
    /// message-loop turn; read `getPreGainFieldTextForStabilityTest()` after a settle.
    void typePreGainTextLikeKeyboardForStabilityTest(const juce::String& text);
    [[nodiscard]] juce::String getPreGainFieldTextForStabilityTest() const { return preGainDbEditor_.getText(); }
    [[nodiscard]] bool isPreGainFieldVisibleForStabilityTest() const { return preGainDbEditor_.isVisible(); }

    /// [Message thread] Undoable **audio input** assignment (`TrackLanesEditCoordinator`):
    /// recording + monitoring source for audio rows only (the control is hidden elsewhere).
    void setAudioInputHandler(std::function<void(TrackId, TrackInputAssignment)> fn) noexcept
    {
        audioInputHandler_ = std::move(fn);
    }

    /// [Message thread] Provider for the active device's input channels (wired from Main).
    void setAudioInputDeviceSnapshotProvider(
        std::function<InspectorAudioInputDeviceSnapshot()> fn) noexcept
    {
        audioInputDeviceSnapshotProvider_ = std::move(fn);
    }

    /// [Message thread] Undoable live **MIDI input** assignment (`TrackLanesEditCoordinator`):
    /// device + input-channel filter for Instrument / Midi rows (the control is hidden elsewhere).
    void setMidiInputHandler(std::function<void(TrackId, TrackMidiInputAssignment)> fn) noexcept
    {
        midiInputHandler_ = std::move(fn);
    }
    /// [Message thread] Provider for the MIDI devices + status of a row (wired from Main).
    void setMidiInputSnapshotProvider(std::function<InspectorMidiInputSnapshot(TrackId)> fn) noexcept
    {
        midiInputSnapshotProvider_ = std::move(fn);
    }
    /// [Stability] Current MIDI Input / channel combo texts and status (what the user sees).
    [[nodiscard]] juce::String getMidiInputComboTextForStabilityTest() const { return midiInputComboBox_.getText(); }
    [[nodiscard]] juce::String getMidiInputChannelComboTextForStabilityTest() const
    {
        return midiInputChannelComboBox_.getText();
    }
    [[nodiscard]] juce::String getMidiInputStatusTextForStabilityTest() const { return midiInputStatusLabel_.getText(); }
    [[nodiscard]] bool isMidiInputComboVisibleForStabilityTest() const { return midiInputComboBox_.isVisible(); }

    /// [Message thread] Undoable **MIDI** output channel (`kTrackMidiOutputChannelAny` or 1 … 16).
    void setMidiOutputChannelHandler(std::function<void(TrackId, int)> fn) noexcept
    {
        midiOutputChannelHandler_ = std::move(fn);
    }

    /// [Message thread] Undoable MIDI destination for `TrackKind::Midi` rows
    /// (`kInvalidTrackId` = no destination / silent).
    void setMidiDestinationHandler(std::function<void(TrackId, TrackId)> fn) noexcept
    {
        midiDestinationHandler_ = std::move(fn);
    }

    /// [Message thread] Undoable send edits (`TrackLanesEditCoordinator`). `sendUiSlotIndex` is 0..3; `destTrackId` = `kInvalidTrackId` clears slot.
    void setTrackSendHandlers(
        std::function<void(TrackId, int sendUiSlotIndex, TrackId destTrackId)> destination,
        std::function<void(TrackId, int sendUiSlotIndex, float amountLinear)> amount,
        std::function<void(TrackId, int sendUiSlotIndex, bool enabled)> enabled) noexcept;

    /// Height the stacked sections currently need (`InspectorPanel` sizes this view to it inside
    /// its scroll viewport). Measured by the last layout pass; sections never stretch with height.
    [[nodiscard]] int getPreferredContentHeight(int widthPx) const noexcept;
    /// Fired (asynchronously, after a layout pass) whenever the preferred height changed.
    void setOnPreferredHeightChanged(std::function<void()> fn) noexcept;

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;
    void dragOperationEnded(const juce::DragAndDropTarget::SourceDetails& details) override;

    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragEnter(const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragMove(const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragExit(const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDropped(const juce::DragAndDropTarget::SourceDetails& details) override;

private:
    friend class InsertSlotButton;
    friend class StageDropTarget;

    void requestEditForSlot(InsertSlotId slotId);
    void requestRemoveForSlot(InsertSlotId slotId);

    void onInsertSlotDragStarted(InsertStage sourceStage);
    void clearInsertSlotDragSession() noexcept;

    [[nodiscard]] bool isInsertRowDragPayloadAcceptedForActiveTrack(const juce::var& desc) const noexcept;
    [[nodiscard]] std::optional<InsertStage> stageForLocalPoint(juce::Point<int> p) const noexcept;

    void handleInsertDropped(TrackId tid,
                            InsertSlotId sid,
                            InsertStage sourceStage,
                            InsertStage targetStage,
                            juce::Point<int> localPoint);

    void updateInsertDragHoverFromInspectorPoint(juce::Point<int> p) noexcept;
    void notifyInsertDropHover(InsertStage stage, juce::Point<int> p) noexcept;
    void clearInsertDropHover() noexcept;
    [[nodiscard]] int gapIndexForStageAtLocalPoint(InsertStage stage, juce::Point<int> p) const noexcept;
    [[nodiscard]] int gapIndexForCrossStageDrop(InsertStage targetStage, juce::Point<int> p) const noexcept;
    [[nodiscard]] bool isSameStageAddButtonArea(InsertStage st, juce::Point<int> p) const noexcept;

    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor& editor) override;

    /// Ctrl/Cmd+click on the pre-gain field = reset to 0.0 dB (the established reset gesture,
    /// same as the pan control). Received via `addMouseListener` on the editor.
    void mouseDown(const juce::MouseEvent& e) override;

    void commitPreGainField();
    void setPreGainEditorTextFromDb(float preGainDb);

    void commitActiveTrackNameField();
    void syncActiveTrackNameEditorDisplay();
    void syncInsertsWhenInspectorDisabled();
    void syncInsertsNoActiveTrack();
    void syncInsertsForActiveTrack(TrackId active);

    void syncSendsWhenInspectorDisabled();
    void syncSendsNoActiveTrack();
    void syncSendsForActiveTrack(TrackId active, const Track& track);

    void commitSendAmountField(int sendRowIndex);
    void setSendAmountEditorText(int sendRowIndex, float amountLinear);
    void populateSendDestCombo(int sendRowIndex, TrackId activeTrackId, const Track& track);
    /// Rebuild the "Audio Input" combo from the active device snapshot and the track's stored
    /// assignment (unresolved assignments appear as an explicit "(unavailable)" entry).
    void populateAudioInputCombo(const Track& track);
    /// Rebuild the "MIDI Input" + "Input Channel" combos and the status line for the row.
    void populateMidiInputControls(const Track& track);

    void clearInsertRowStrips();
    void rebuildInsertRowStrips(TrackId active, const std::vector<InspectorInsertRow>& rows);

    Session& session_;
    InspectorPluginHost pluginHost_;
    juce::Label sectionTitleLabel_;
    juce::TextEditor activeTrackNameEditor_;
    /// Pre-gain (audio rows only): dB before Pre inserts and fader; sits above Channel volume.
    juce::Label preGainCaptionLabel_;
    juce::TextEditor preGainDbEditor_;
    juce::Label preGainDbUnitLabel_;
    /// Transient "why did nothing happen" bubble for a refused pre-gain commit (see
    /// `commitPreGainField`): undoable session edits are refused while recording / count-in.
    juce::BubbleMessageComponent preGainRefusedBubble_;
    juce::Label panCaptionLabel_;
    InspectorPanControl panField_;
    /// Audio Input (audio rows only): which device input the track records/monitors.
    juce::Label inputCaptionLabel_;
    juce::ComboBox inputComboBox_;
    juce::Label outputCaptionLabel_;
    juce::ComboBox outputComboBox_;
    /// MIDI output channel (instrument rows only). Deliberately captioned "MIDI Channel" next to
    /// "Audio Output" so the two routing concepts are never both just called "Output".
    juce::Label midiChannelCaptionLabel_;
    juce::ComboBox midiChannelComboBox_;
    /// MIDI destination ("MIDI To") — `TrackKind::Midi` rows only.
    juce::Label midiDestCaptionLabel_;
    juce::ComboBox midiDestComboBox_;
    /// Live MIDI input (Instrument / Midi rows): device selector, input-channel FILTER (distinct
    /// from the output "MIDI Channel" above) and a one-line status.
    juce::Label midiInputCaptionLabel_;
    juce::ComboBox midiInputComboBox_;
    juce::Label midiInputChannelCaptionLabel_;
    juce::ComboBox midiInputChannelComboBox_;
    juce::Label midiInputStatusLabel_;
    juce::Label insertsSectionLabel_;
    juce::Label preSectionLabel_;
    juce::Label preEmptyLabel_;
    juce::TextButton addPreInsertButton_;
    juce::Label postSectionLabel_;
    juce::Label postEmptyLabel_;
    juce::TextButton addPostInsertButton_;

    static constexpr int kVisibleSendRows = 4;

    struct SendRowUi
    {
        juce::ComboBox destCombo;
        juce::TextEditor amountEditor;
        juce::Label amountDbUnitLabel;
        juce::ToggleButton enableToggle;
        std::vector<TrackId> destIds;
        bool comboGuard = false;
        bool amountGuard = false;
        bool enableGuard = false;
    };

    juce::Label sendsSectionLabel_;
    juce::Label sendsExtraLabel_;
    SendRowUi sendRows_[kVisibleSendRows];

    std::unique_ptr<StageDropTarget> preStageDrop_;
    std::unique_ptr<StageDropTarget> postStageDrop_;
    std::optional<InsertStage> insertDragSourceStage_;
    juce::Rectangle<int> preInsertBlockBounds_;
    juce::Rectangle<int> postInsertBlockBounds_;

    bool insertDropHoverActive_ = false;
    InsertStage insertDropHoverStage_ = InsertStage::Pre;
    int insertDropHoverGapIndex_ = 0;
    juce::Rectangle<int> insertDropLineBounds_;

    std::vector<std::unique_ptr<InsertSlotButton>> preRowStrips_;
    std::vector<std::unique_ptr<InsertSlotButton>> postRowStrips_;

    /// Last insert-row model shown in the UI (avoids rebuilding strips on every timer tick).
    std::vector<InspectorInsertRow> lastShownInsertRows_;
    juce::String activeTrackPlainName_;

    std::function<bool(TrackId, juce::String)> renameTrackHandler_;
    std::function<void(TrackId, float)> preGainHandler_;
    std::function<void(TrackId, TrackInputAssignment)> audioInputHandler_;
    std::function<InspectorAudioInputDeviceSnapshot()> audioInputDeviceSnapshotProvider_;
    std::function<void(TrackId, TrackId)> routedOutputHandler_;
    std::function<void(TrackId, int)> midiOutputChannelHandler_;
    std::function<void(TrackId, TrackId)> midiDestinationHandler_;
    std::function<void(TrackId, TrackMidiInputAssignment)> midiInputHandler_;
    std::function<InspectorMidiInputSnapshot(TrackId)> midiInputSnapshotProvider_;
    bool midiInputComboGuard_ = false;
    /// Parallel to the MIDI Input combo's item ids (1-based); channel filter is applied on top.
    std::vector<TrackMidiInputAssignment> midiInputComboValues_;
    bool midiInputChannelComboGuard_ = false;
    std::function<void(TrackId, int, TrackId)> trackSendDestinationHandler_;
    std::function<void(TrackId, int, float)> trackSendAmountHandler_;
    std::function<void(TrackId, int, bool)> trackSendEnabledHandler_;
    bool inspectorNameEditorGuard_ = false;
    bool outputComboGuard_ = false;
    std::vector<TrackId> outputComboDestIds_;
    bool inputComboGuard_ = false;
    /// Parallel to the input combo's item ids (1-based).
    std::vector<TrackInputAssignment> inputComboValues_;
    bool midiChannelComboGuard_ = false;
    /// Parallel to the combo's item ids (1-based): `kTrackMidiOutputChannelAny` then 1 … 16.
    std::vector<int> midiChannelComboValues_;
    bool midiDestComboGuard_ = false;
    /// Parallel to the combo's item ids (1-based): `kInvalidTrackId` first ("No destination").
    std::vector<TrackId> midiDestComboValues_;

    TrackId lastShownInsertRowsTrackId_ = kInvalidTrackId;
    TrackId lastShownTrackId_ = kInvalidTrackId;

    /// Preferred content height measured by the last `resized()` (see `getPreferredContentHeight`).
    int lastLayoutUsedHeight_ = 0;
    std::function<void()> onPreferredHeightChanged_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorView)
};
