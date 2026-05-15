#pragma once

#include <JuceHeader.h>

#include <memory>

#include "app/TransportControlsShortcutTarget.h"

class Transport;
class Session;
class PluginInsertHost;
class RecorderService;
class CountInClickOutput;
class LatencySettingsStore;
class PlaybackEngine;

struct CreatedTransportUiForMainWindow
{
    std::unique_ptr<juce::Component> component;
    TransportControlsShortcutTarget* shortcutTarget = nullptr;
};

CreatedTransportUiForMainWindow createTransportUiForMainWindow(
    Transport& transport,
    Session& session,
    PluginInsertHost& pluginInsertHost,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorderService,
    CountInClickOutput& countInClicks,
    LatencySettingsStore& latencyStore,
    PlaybackEngine& playbackEngine);
