#pragma once

#include "domain/Track.h"

#include <functional>
#include <utility>
#include <juce_gui_basics/juce_gui_basics.h>

class Session;

/// [Message thread] Optional plugin-insert actions for the active track (wired from Main).
struct InspectorPluginHost
{
    std::function<bool(TrackId)> hasPlugin;
    std::function<juce::String(TrackId)> displayName;
    std::function<void(TrackId)> requestAdd;
    std::function<void(TrackId)> requestEdit;
    std::function<void(TrackId)> requestRemove;
};

/// Active-track-only controls (Cubase-style Inspector), not repeated in every track header.

class InspectorView final : public juce::Component,
                            private juce::TextEditor::Listener
{
public:
    explicit InspectorView(Session& session);

    /// [Message thread] Sync from current snapshot / `getActiveTrackId` (safe to poll).

    void refreshFromSession();

    void setInspectorPluginHost(InspectorPluginHost host) noexcept { pluginHost_ = std::move(host); }

    void resized() override;

private:
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor& editor) override;

    void commitVolumeField();
    void setVolumeEditorTextFromLinearGain(float linearGain);
    void updateElidedTrackTitleDisplay();
    void syncInsertsWhenInspectorDisabled();
    void syncInsertsNoActiveTrack();
    void syncInsertsForActiveTrack(TrackId active);

    Session& session_;
    InspectorPluginHost pluginHost_;
    juce::Label sectionTitleLabel_;
    juce::Label activeTrackTitleLabel_;
    juce::Label channelVolumeCaptionLabel_;
    juce::TextEditor channelVolumeDbEditor_;
    juce::Label channelVolumeDbUnitLabel_;
    juce::Label insertsSectionLabel_;
    juce::TextButton addInsertButton_;
    juce::Label insertSlotNameLabel_;
    juce::TextButton editPluginButton_;
    juce::TextButton removePluginButton_;

    /// Full active-track name (used for tooltip and eliding to narrow label width).
    juce::String activeTrackPlainName_;

    TrackId lastShownTrackId_ = kInvalidTrackId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorView)
};
