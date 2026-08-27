#pragma once

#include <JuceHeader.h>

#include <memory>

class Transport;
class Session;
class PluginInsertHost;
class RecorderService;
class CountInClickOutput;
class LatencySettingsStore;
class PlaybackEngine;

class TransportControlsShortcutTarget;
struct StabilityScenarioRequest;

class MainWindow final : public juce::DocumentWindow, public juce::KeyListener
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

    /// [Message thread] Load a ".dalproj" passed on the command line through the normal load pipeline.
    void openProjectFileFromCommandLine(const juce::File& projectFile);

    /// [Message thread] Stability Slice 5: returns true when a save-before-quit prompt was shown
    /// (quit resumes or is cancelled from the prompt); false = no unsaved changes, quit now.
    [[nodiscard]] bool tryInterceptQuitForUnsavedChanges();

    /// [Message thread] Stability Slice 5: offer autosave recovery on startup.
    void offerAutosaveRecoveryOnStartup(bool commandLineProjectOpenQueued);

    /// [Message thread] Stability C2: start an in-process `--stability-*` scenario.
    void startStabilityScenario(const StabilityScenarioRequest& request);

private:
    [[nodiscard]] bool routeShortcut(const juce::KeyPress& key);

    TransportControlsShortcutTarget* shortcutTargetFromContent_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

[[nodiscard]] std::unique_ptr<MainWindow> createMainWindow(
    const juce::String& name,
    Transport& transport,
    Session& session,
    PluginInsertHost& pluginInsertHost,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorderService,
    CountInClickOutput& countInClicks,
    LatencySettingsStore& latencyStore,
    PlaybackEngine& playbackEngine);
