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
    };

    ProjectIoCoordinator(Transport& transport,
                           Session& session,
                           juce::AudioDeviceManager& deviceManager,
                           PluginInsertHost& pluginHost,
                           Callbacks callbacks);

    void saveProject();
    void loadProject();

private:
    Transport& transport_;
    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    PluginInsertHost& pluginHost_;
    Callbacks callbacks_;
};
