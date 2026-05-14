#pragma once

#include <functional>

#include <juce_audio_devices/juce_audio_devices.h>

#include "domain/Track.h"

class Transport;
class Session;
class PluginInsertHost;
class InstrumentTrackController;

class ProjectIoCoordinator
{
public:
    struct Callbacks
    {
        std::function<InstrumentTrackController*(TrackId)> instrumentCtlByTrackId;
        std::function<void()> snapshotOpenClipViewportFromMidiEditor;
    };

    ProjectIoCoordinator(Transport& transport,
                           Session& session,
                           juce::AudioDeviceManager& deviceManager,
                           PluginInsertHost& pluginHost,
                           Callbacks callbacks);

    void saveProject();

private:
    Transport& transport_;
    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    PluginInsertHost& pluginHost_;
    Callbacks callbacks_;
};
