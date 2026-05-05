// =============================================================================
// PluginEditorWindows.cpp — native + generic plugin editor shells (message thread)
// =============================================================================

#include "plugins/PluginEditorWindows.h"

#include "plugins/PluginInsertHost.h"

namespace
{
    [[nodiscard]] bool dispatchHostUndoRedoFromPluginWindowKey(const juce::KeyPress& key,
                                                               const PluginEditorWindowHostShortcuts& sh)
    {
        if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        {
            return false;
        }

        if (!key.getModifiers().isCommandDown())
        {
            return false;
        }

        const int kc = key.getKeyCode();
        if (!key.getModifiers().isShiftDown() && (kc == 'z' || kc == 'Z'))
        {
            if (sh.requestUndo)
            {
                sh.requestUndo();
                return true;
            }
            return false;
        }
        if ((kc == 'y' || kc == 'Y')
            || (key.getModifiers().isShiftDown() && (kc == 'z' || kc == 'Z')))
        {
            if (sh.requestRedo)
            {
                sh.requestRedo();
                return true;
            }
            return false;
        }
        return false;
    }
} // namespace

PluginEditorWindow::PluginEditorWindow(PluginInsertHost& host,
                                       const TrackId trackId,
                                       const InsertSlotId insertSlotId,
                                       juce::AudioProcessor& proc,
                                       std::unique_ptr<juce::AudioProcessorEditor> editor,
                                       PluginEditorWindowHostShortcuts hostShortcuts)
    : DocumentWindow(proc.getName().isNotEmpty() ? proc.getName() : "Plugin",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
    , host_(host)
    , trackId_(trackId)
    , insertSlotId_(insertSlotId)
    , hostShortcuts_(std::move(hostShortcuts))
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

bool PluginEditorWindow::keyPressed(const juce::KeyPress& key)
{
    if (dispatchHostUndoRedoFromPluginWindowKey(key, hostShortcuts_))
    {
        return true;
    }
    return DocumentWindow::keyPressed(key);
}

void PluginEditorWindow::closeButtonPressed()
{
    setVisible(false);
    PluginInsertHost* const host = &host_;
    const TrackId trackId = trackId_;
    const InsertSlotId slotId = insertSlotId_;
    juce::MessageManager::callAsync([host, trackId, slotId] {
        if (host != nullptr)
        {
            host->editorWindowClosing(trackId, slotId, false);
        }
    });
}

PluginParamsWindow::PluginParamsWindow(PluginInsertHost& host,
                                       const TrackId trackId,
                                       const InsertSlotId insertSlotId,
                                       juce::AudioProcessor& proc,
                                       std::unique_ptr<juce::AudioProcessorEditor> editor,
                                       PluginEditorWindowHostShortcuts hostShortcuts)
    : DocumentWindow((proc.getName().isNotEmpty() ? proc.getName() : "Plugin") + " — parameters",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
    , host_(host)
    , trackId_(trackId)
    , insertSlotId_(insertSlotId)
    , hostShortcuts_(std::move(hostShortcuts))
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

bool PluginParamsWindow::keyPressed(const juce::KeyPress& key)
{
    if (dispatchHostUndoRedoFromPluginWindowKey(key, hostShortcuts_))
    {
        return true;
    }
    return DocumentWindow::keyPressed(key);
}

void PluginParamsWindow::closeButtonPressed()
{
    setVisible(false);
    PluginInsertHost* const host = &host_;
    const TrackId trackId = trackId_;
    const InsertSlotId slotId = insertSlotId_;
    juce::MessageManager::callAsync([host, trackId, slotId] {
        if (host != nullptr)
        {
            host->editorWindowClosing(trackId, slotId, true);
        }
    });
}
