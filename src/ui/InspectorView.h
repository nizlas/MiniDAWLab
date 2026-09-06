#pragma once

#include "domain/Track.h"
#include "instruments/ProxyStatusModel.h" // P1I proxy status view (pure model)
#include "plugins/InsertSlotId.h"

#include "ui/InspectorPanControl.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

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

/// [Message thread] P1I instrument-proxy section seams (wired from Main). The Inspector
/// never touches the scheduler/policy services directly: it displays the precomputed
/// ProxyStatusView and invokes these narrow actions. All optional (null = hidden section).
struct InspectorProxyHost
{
    /// True only for instrument destinations with a proxy-capable runtime.
    std::function<bool(TrackId)> isProxyDestination;
    /// Complete precomputed status view (labels, tooltip, control availability).
    std::function<proxy_status::ProxyStatusView(TrackId)> getStatusView;
    /// Persisted per-destination update mode (0 Auto / 1 On Save / 2 Manual / 3 Off).
    /// Dirty semantics live behind this seam (§18.3), never in the view.
    std::function<void(TrackId, int modeComboIndex)> setUpdateMode;
    std::function<void(TrackId)> renderNow;
    std::function<void(TrackId)> cancelRender;
    std::function<void(TrackId)> retryRender;
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

    void setInspectorProxyHost(InspectorProxyHost host) noexcept { proxyHost_ = std::move(host); }

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

    void commitVolumeField();
    void commitActiveTrackNameField();
    void setVolumeEditorTextFromLinearGain(float linearGain);
    void syncActiveTrackNameEditorDisplay();
    void syncInsertsWhenInspectorDisabled();
    void syncInsertsNoActiveTrack();
    void syncInsertsForActiveTrack(TrackId active);

    void syncSendsWhenInspectorDisabled();
    void syncSendsNoActiveTrack();
    void syncSendsForActiveTrack(TrackId active, const Track& track);

    /// P1I: show/refresh or hide the instrument-proxy section for the active row.
    void syncProxySectionForActiveTrack(TrackId active, const Track& track);
    void setProxySectionVisible(bool visible);

    void commitSendAmountField(int sendRowIndex);
    void setSendAmountEditorText(int sendRowIndex, float amountLinear);
    void populateSendDestCombo(int sendRowIndex, TrackId activeTrackId, const Track& track);

    void clearInsertRowStrips();
    void rebuildInsertRowStrips(TrackId active, const std::vector<InspectorInsertRow>& rows);

    Session& session_;
    InspectorPluginHost pluginHost_;
    juce::Label sectionTitleLabel_;
    juce::TextEditor activeTrackNameEditor_;
    juce::Label channelVolumeCaptionLabel_;
    juce::TextEditor channelVolumeDbEditor_;
    juce::Label channelVolumeDbUnitLabel_;
    juce::Label panCaptionLabel_;
    InspectorPanControl panField_;
    juce::Label outputCaptionLabel_;
    juce::ComboBox outputComboBox_;
    /// MIDI output channel (instrument rows only). Deliberately captioned "MIDI Channel" next to
    /// "Audio Output" so the two routing concepts are never both just called "Output".
    juce::Label midiChannelCaptionLabel_;
    juce::ComboBox midiChannelComboBox_;
    /// MIDI destination ("MIDI To") — `TrackKind::Midi` rows only.
    juce::Label midiDestCaptionLabel_;
    juce::ComboBox midiDestComboBox_;
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

    // ------------------------------------------------------------------ P1I
    // Instrument proxy status + controls (visible only for proxy destinations).
    // No Secondary control exists (P1); no extra track/mixer channel is created.
    juce::Label proxySectionLabel_;
    juce::Label proxySourceCaptionLabel_;
    juce::Label proxySourceValueLabel_; ///< playback source (compact, PI-021)
    juce::Label proxyCacheCaptionLabel_;
    juce::Label proxyCacheValueLabel_;  ///< cache/maintenance state (distinct axis)
    juce::Label proxyProgressLabel_;    ///< unobtrusive "12.3 s rendered" (no modal)
    juce::Label proxyModeCaptionLabel_;
    juce::ComboBox proxyModeComboBox_;  ///< Auto after idle / On Save / Manual / Off
    juce::TextButton proxyRenderNowButton_;
    juce::TextButton proxyCancelButton_;
    juce::TextButton proxyRetryButton_;
    bool proxyModeComboGuard_ = false;
    InspectorProxyHost proxyHost_;

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
    std::function<void(TrackId, TrackId)> routedOutputHandler_;
    std::function<void(TrackId, int)> midiOutputChannelHandler_;
    std::function<void(TrackId, TrackId)> midiDestinationHandler_;
    std::function<void(TrackId, int, TrackId)> trackSendDestinationHandler_;
    std::function<void(TrackId, int, float)> trackSendAmountHandler_;
    std::function<void(TrackId, int, bool)> trackSendEnabledHandler_;
    bool inspectorNameEditorGuard_ = false;
    bool outputComboGuard_ = false;
    std::vector<TrackId> outputComboDestIds_;
    bool midiChannelComboGuard_ = false;
    /// Parallel to the combo's item ids (1-based): `kTrackMidiOutputChannelAny` then 1 … 16.
    std::vector<int> midiChannelComboValues_;
    bool midiDestComboGuard_ = false;
    /// Parallel to the combo's item ids (1-based): `kInvalidTrackId` first ("No destination").
    std::vector<TrackId> midiDestComboValues_;

    TrackId lastShownInsertRowsTrackId_ = kInvalidTrackId;
    TrackId lastShownTrackId_ = kInvalidTrackId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorView)
};
