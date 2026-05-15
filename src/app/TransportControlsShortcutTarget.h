#pragma once

#include <JuceHeader.h>

/// Narrow surface used by MainWindow shortcut routing (`routeShortcut`).
class TransportControlsShortcutTarget
{
public:
    virtual ~TransportControlsShortcutTarget() = default;

    virtual void setKeyDiagnosticLine(const juce::String& line) = 0;

    virtual void setShortcutDiagVisibleCaption(const juce::String& line) = 0;

    virtual void invokeDeleteSelectedPlacedClipFromWindowShortcut() = 0;

    virtual void invokeCopySelectedClipFromWindowShortcut() = 0;

    virtual void invokePasteClipFromWindowShortcut() = 0;

    virtual void invokeUndoFromWindowShortcut() = 0;

    virtual void invokeRedoFromWindowShortcut() = 0;

    virtual void invokeRecordToggleFromWindowShortcut() = 0;

    virtual void invokeJumpToLeftLocatorFromWindowShortcut() = 0;

    virtual void invokePlayPauseToggleFromWindowShortcut() = 0;
};
