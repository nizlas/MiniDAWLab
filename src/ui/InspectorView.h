#pragma once

#include "domain/Track.h"
#include "plugins/InsertSlotId.h"

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
    void setVolumeEditorTextFromLinearGain(float linearGain);
    void updateElidedTrackTitleDisplay();
    void syncInsertsWhenInspectorDisabled();
    void syncInsertsNoActiveTrack();
    void syncInsertsForActiveTrack(TrackId active);

    void clearInsertRowStrips();
    void rebuildInsertRowStrips(TrackId active, const std::vector<InspectorInsertRow>& rows);

    Session& session_;
    InspectorPluginHost pluginHost_;
    juce::Label sectionTitleLabel_;
    juce::Label activeTrackTitleLabel_;
    juce::Label channelVolumeCaptionLabel_;
    juce::TextEditor channelVolumeDbEditor_;
    juce::Label channelVolumeDbUnitLabel_;
    juce::Label insertsSectionLabel_;
    juce::Label preSectionLabel_;
    juce::Label preEmptyLabel_;
    juce::TextButton addPreInsertButton_;
    juce::Label postSectionLabel_;
    juce::Label postEmptyLabel_;
    juce::TextButton addPostInsertButton_;

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

    TrackId lastShownInsertRowsTrackId_ = kInvalidTrackId;
    TrackId lastShownTrackId_ = kInvalidTrackId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorView)
};
