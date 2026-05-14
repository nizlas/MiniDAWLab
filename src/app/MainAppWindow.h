#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <juce_audio_devices/juce_audio_devices.h>

class Transport;
class Session;
class PluginInsertHost;
class RecorderService;
class CountInClickOutput;
class LatencySettingsStore;
class PlaybackEngine;

class MainWindow : public juce::DocumentWindow, public juce::KeyListener
{
public:
    MainWindow(const juce::String& name,
               Transport& transport,
               Session& session,
               PluginInsertHost& pluginInsertHost,
               juce::AudioDeviceManager& deviceManager,
               RecorderService& recorderService,
               CountInClickOutput& countInClicks,
               LatencySettingsStore& latencyStore,
               PlaybackEngine& playbackEngine);

    ~MainWindow() override;

    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;
    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override;

private:
    [[nodiscard]] bool routeShortcut(const juce::KeyPress& key);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};
