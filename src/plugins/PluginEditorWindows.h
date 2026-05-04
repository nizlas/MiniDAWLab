#pragma once

// =============================================================================
// PluginEditorWindows — host-owned floating windows for VST3 UIs (Phase 8)
// =============================================================================

#include "domain/Track.h"
#include "plugins/InsertSlotId.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class PluginInsertHost;

/// Optional callbacks so plugin editor windows forward undo/redo to the same handlers as MainWindow.
struct PluginEditorWindowHostShortcuts
{
    std::function<void()> requestUndo;
    std::function<void()> requestRedo;
};

/// Wraps the plugin’s native `AudioProcessorEditor`.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow(PluginInsertHost& host,
                       TrackId trackId,
                       InsertSlotId insertSlotId,
                       juce::AudioProcessor& proc,
                       std::unique_ptr<juce::AudioProcessorEditor> editor,
                       PluginEditorWindowHostShortcuts hostShortcuts);

    void closeButtonPressed() override;

    bool keyPressed(const juce::KeyPress& key) override;

private:
    PluginInsertHost& host_;
    TrackId trackId_;
    InsertSlotId insertSlotId_;
    PluginEditorWindowHostShortcuts hostShortcuts_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};

/// Generic sliders / parameters (`GenericAudioProcessorEditor`) in a second window.
class PluginParamsWindow : public juce::DocumentWindow
{
public:
    PluginParamsWindow(PluginInsertHost& host,
                       TrackId trackId,
                       InsertSlotId insertSlotId,
                       juce::AudioProcessor& proc,
                       std::unique_ptr<juce::AudioProcessorEditor> editor,
                       PluginEditorWindowHostShortcuts hostShortcuts);

    void closeButtonPressed() override;

    bool keyPressed(const juce::KeyPress& key) override;

private:
    PluginInsertHost& host_;
    TrackId trackId_;
    InsertSlotId insertSlotId_;
    PluginEditorWindowHostShortcuts hostShortcuts_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginParamsWindow)
};
