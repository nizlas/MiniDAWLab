#pragma once

#include <JuceHeader.h>

struct StabilityScenarioRequest;

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

    /// Ctrl+S: same flow as File -> Save Project (known path saves directly, else Save As chooser).
    virtual void invokeSaveProjectFromWindowShortcut() = 0;

    /// Command-line ".dalproj" open: load `projectFile` through the normal project-load pipeline.
    virtual void invokeLoadProjectFileFromStartup(const juce::File& projectFile) = 0;

    /// Stability Slice 5: quit interception. Returns false when the app may quit immediately
    /// (no unsaved changes); returns true when a save prompt was shown and quit resumes (or is
    /// cancelled) from that prompt.
    [[nodiscard]] virtual bool invokeQuitUnsavedGuardFromWindow() = 0;

    /// Stability Slice 5: startup autosave recovery offer (skipped when a command-line ".dalproj"
    /// open is already queued).
    virtual void invokeAutosaveRecoveryCheckFromStartup(bool commandLineProjectOpenQueued) = 0;

    /// Stability C2: start an in-process `--stability-*` scenario (see StabilityScenarioRunner.h).
    virtual void invokeStartStabilityScenarioFromStartup(const StabilityScenarioRequest& request) = 0;
};
