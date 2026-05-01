#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class LatencySettingsStore;
class PlaybackEngine;

/// Lives under Audio Settings dialog; edits `LatencySettingsStore` on the message thread.
class LatencySettingsView final : public juce::Component,
                                   private juce::TextEditor::Listener,
                                   private juce::Button::Listener
{
public:
    LatencySettingsView(LatencySettingsStore& store, PlaybackEngine& playbackEngine);

    void syncFromStore();

    void resized() override;

private:
    void buttonClicked(juce::Button*) override;

    void textEditorTextChanged(juce::TextEditor&) override {}
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor& editor) override;

    void commitRecordingFromSamplesEditor(bool focusNext);
    void commitRecordingFromMsEditor(bool focusNext);
    void commitPlaybackFromSamplesEditor(bool focusNext);
    void commitPlaybackFromMsEditor(bool focusNext);

    [[nodiscard]] bool sampleRateKnown() const noexcept;
    void showLatencyInfoBox();

    LatencySettingsStore& store_;
    PlaybackEngine& playbackEngine_;

    bool suppressCommits_{ false };

    juce::GroupComponent latencyGroup_;

    std::unique_ptr<juce::Label> reportedRateLabel_;
    std::unique_ptr<juce::Label> reportedBufLabel_;
    std::unique_ptr<juce::Label> reportedInLabel_;
    std::unique_ptr<juce::Label> reportedOutLabel_;
    std::unique_ptr<juce::Label> recExplainerLabel_;

    std::unique_ptr<juce::Label> recSamplesTitle_;
    std::unique_ptr<juce::TextEditor> recSamples_;
    std::unique_ptr<juce::Label> recMsTitle_;
    std::unique_ptr<juce::TextEditor> recMs_;

    std::unique_ptr<juce::TextButton> recResetMinusReportedIn_;

    std::unique_ptr<juce::Label> playSamplesTitle_;
    std::unique_ptr<juce::TextEditor> playSamples_;
    std::unique_ptr<juce::Label> playMsTitle_;
    std::unique_ptr<juce::TextEditor> playMs_;

    std::unique_ptr<juce::TextButton> playResetZero_;
    std::unique_ptr<juce::TextButton> playSetReportedOut_;

    std::unique_ptr<juce::ToggleButton> lowLatencyPlaceholder_;

    std::unique_ptr<juce::TextButton> infoButton_;

    LatencySettingsView(const LatencySettingsView&) = delete;
    LatencySettingsView& operator=(const LatencySettingsView&) = delete;
    LatencySettingsView(LatencySettingsView&&) = delete;
    LatencySettingsView& operator=(LatencySettingsView&&) = delete;
};
