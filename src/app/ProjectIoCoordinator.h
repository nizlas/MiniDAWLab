#pragma once

#include <functional>
#include <utility>

#include <juce_audio_devices/juce_audio_devices.h>

#include "domain/Track.h"

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
