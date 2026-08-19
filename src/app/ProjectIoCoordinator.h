#pragma once

#include <JuceHeader.h>

#include <functional>
#include <optional>
#include <utility>

#include "domain/Track.h"
#include "io/ProjectFile.h"
#include "ui/SnapSettings.h"

class Transport;
class Session;
class PlaybackEngine;
class PluginInsertHost;
class InstrumentTrackController;
class ExperimentalInstrumentHost;

class ProjectIoCoordinator
{
public:
    struct Callbacks
    {
        std::function<InstrumentTrackController*(TrackId)> instrumentCtlByTrackId;
        std::function<void()> snapshotOpenClipViewportFromMidiEditor;

        std::function<void()> clearExperimentalInstrumentRuntimesPreserveBridgeOnly;
        std::function<std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>(TrackId)> getOrCreateInstrumentRuntimeForTrack;
        std::function<void()> syncMidiEditorInstrumentStateFromHost;
        std::function<void()> clearSessionHistory;
        std::function<void()> refreshAllUiAfterLoadedProject;

        std::function<SnapProjectRootFields()> getSnapProjectRootFieldsForSave;
        std::function<void(const SnapProjectRootFields&)> restoreSnapProjectRootFieldsToUi;

        std::function<std::optional<ProjectFileMainWindowBoundsV1>()> getMainWindowBoundsForProjectSave;
        std::function<void(const ProjectFileV1&)> applyMainWindowBoundsFromLoadedProject;

        /// Optional: transient "Saving project" indicator, invoked right before the project file write.
        std::function<void()> showSavingProjectIndicator;
    };

    ProjectIoCoordinator(Transport& transport,
                           Session& session,
                           juce::AudioDeviceManager& deviceManager,
                           PluginInsertHost& pluginHost,
                           PlaybackEngine& playbackEngine,
                           Callbacks callbacks);

    void saveProject();
    void loadProject();

    /// [Message thread] Load `projectFile` directly (no chooser) — same pipeline as `loadProject`,
    /// so project-relative paths resolve identically. Used by the command-line ".dalproj" open path.
    /// Shows a non-fatal alert when the file is missing or unreadable.
    void loadProjectFromFile(const juce::File& projectFile);

private:
    Transport& transport_;
    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    PluginInsertHost& pluginHost_;
    PlaybackEngine& playbackEngine_;
    Callbacks callbacks_;
};
