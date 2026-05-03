// =============================================================================
// PluginEditorWindows.cpp — native + generic plugin editor shells (message thread)
// =============================================================================

#include "plugins/PluginEditorWindows.h"

#include "plugins/PluginInsertHost.h"

PluginEditorWindow::PluginEditorWindow(PluginInsertHost& host,
                                       const TrackId trackId,
                                       juce::AudioProcessor& proc,
                                       std::unique_ptr<juce::AudioProcessorEditor> editor)
    : DocumentWindow(proc.getName().isNotEmpty() ? proc.getName() : "Plugin",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
    , host_(host)
    , trackId_(trackId)
{
    juce::ignoreUnused(proc);
    setUsingNativeTitleBar(true);
    setContentOwned(editor.release(), true);
    if (auto* c = getContentComponent())
    {
        const int tw = juce::jmax(200, c->getWidth());
        const int th = juce::jmax(120, c->getHeight());
        centreWithSize(tw + 20, th + 20);
    }
    setVisible(true);
}

void PluginEditorWindow::closeButtonPressed()
{
    host_.editorWindowClosing(trackId_, false);
}

PluginParamsWindow::PluginParamsWindow(PluginInsertHost& host,
                                       const TrackId trackId,
                                       juce::AudioProcessor& proc,
                                       std::unique_ptr<juce::AudioProcessorEditor> editor)
    : DocumentWindow((proc.getName().isNotEmpty() ? proc.getName() : "Plugin") + " — parameters",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
    , host_(host)
    , trackId_(trackId)
{
    juce::ignoreUnused(proc);
    setUsingNativeTitleBar(true);
    setContentOwned(editor.release(), true);
    if (auto* c = getContentComponent())
    {
        const int tw = juce::jmax(320, c->getWidth());
        const int th = juce::jmax(200, c->getHeight());
        centreWithSize(tw + 20, th + 40);
    }
    setVisible(true);
}

void PluginParamsWindow::closeButtonPressed()
{
    host_.editorWindowClosing(trackId_, true);
}
