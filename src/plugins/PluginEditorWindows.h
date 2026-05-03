#pragma once

// =============================================================================
// PluginEditorWindows — host-owned floating windows for VST3 UIs (Phase 8)
// =============================================================================

#include "domain/Track.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class PluginInsertHost;

/// Wraps the plugin’s native `AudioProcessorEditor`.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow(PluginInsertHost& host,
                       TrackId trackId,
                       juce::AudioProcessor& proc,
                       std::unique_ptr<juce::AudioProcessorEditor> editor);

    void closeButtonPressed() override;

private:
    PluginInsertHost& host_;
    TrackId trackId_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};

/// Generic sliders / parameters (`GenericAudioProcessorEditor`) in a second window.
class PluginParamsWindow : public juce::DocumentWindow
{
public:
    PluginParamsWindow(PluginInsertHost& host,
                       TrackId trackId,
                       juce::AudioProcessor& proc,
                       std::unique_ptr<juce::AudioProcessorEditor> editor);

    void closeButtonPressed() override;

private:
    PluginInsertHost& host_;
    TrackId trackId_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginParamsWindow)
};
