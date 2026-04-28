#pragma once

#include "domain/Track.h"

#include <juce_gui_basics/juce_gui_basics.h>

class Session;

/// Active-track-only controls (Cubase-style Inspector), not repeated in every track header.

class InspectorView final : public juce::Component,
                            private juce::TextEditor::Listener
{
public:
    explicit InspectorView(Session& session);

    /// [Message thread] Sync from current snapshot / `getActiveTrackId` (safe to poll).

    void refreshFromSession();

    void resized() override;

private:
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor& editor) override;

    void commitVolumeField();
    void setVolumeEditorTextFromLinearGain(float linearGain);
    void updateElidedTrackTitleDisplay();

    Session& session_;
    juce::Label sectionTitleLabel_;
    juce::Label activeTrackTitleLabel_;
    juce::Label channelVolumeCaptionLabel_;
    juce::TextEditor channelVolumeDbEditor_;
    juce::Label channelVolumeDbUnitLabel_;

    /// Full active-track name (used for tooltip and eliding to narrow label width).
    juce::String activeTrackPlainName_;

    TrackId lastShownTrackId_ = kInvalidTrackId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorView)
};
