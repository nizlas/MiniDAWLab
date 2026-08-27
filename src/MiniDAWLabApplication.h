#pragma once

#include <memory>

#include <JuceHeader.h>

#include <juce_audio_devices/juce_audio_devices.h>

class MainWindow;
class Transport;
class Session;
class RecorderService;
class CountInClickOutput;
class PluginInsertHost;
class PlaybackEngine;
class LatencySettingsStore;

class MiniDAWLabApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Danielssons Audio Lab"; }

    const juce::String getApplicationVersion() override
    {
        return { ProjectInfo::versionString };
    }

    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;

    /// Stability Slice 5: quits immediately when the project is clean; otherwise shows the
    /// save-before-quit prompt (quit resumes from the prompt). Implemented in Main.cpp.
    void systemRequestedQuit() override;

    ~MiniDAWLabApplication() override;

private:
    bool vst3OopWorkerMode_ = false;

    std::unique_ptr<Transport> transport;
    std::unique_ptr<Session> session;
    // Phase 4: recording capture (not user-wired in this file yet); engine holds non-owning `get()`.
    std::unique_ptr<RecorderService> recorderService;
    /// Count-in metronome clicks to device only; coordinator state lives in `TransportControlsContent`.
    std::unique_ptr<CountInClickOutput> countInOutput_;
    std::unique_ptr<PluginInsertHost> pluginInsertHost_;
    std::unique_ptr<PlaybackEngine> playbackEngine;
    std::unique_ptr<LatencySettingsStore> latencySettingsStore;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<MainWindow> mainWindow;
};
