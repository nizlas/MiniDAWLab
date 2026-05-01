#pragma once


#include <juce_audio_devices/juce_audio_devices.h>
#include <map>

/// Message-thread only. App-level persistence for recording placement and playback audible-alignment offsets.
/// Per-device keyed by `{typeName}::{deviceName}`; see `%APPDATA%\\MiniDAWLab\\audio-latency.xml`.
class LatencySettingsStore final
{
public:
    LatencySettingsStore(juce::AudioDeviceManager& deviceManager, juce::File persistenceFile);

    void loadFromFile();
    /// Re-read the active device latencies/buffer/rate and re-apply overrides or defaults for the current device key.
    void refreshFromCurrentDevice();

    [[nodiscard]] std::int64_t getCurrentRecordingOffsetSamples() const noexcept { return recordingOffsetSamples_; }

    [[nodiscard]] std::int64_t getCurrentPlaybackOffsetSamples() const noexcept { return playbackOffsetSamples_; }

    [[nodiscard]] int getReportedInputLatencySamples() const noexcept { return reportedInputLatencySamples_; }

    [[nodiscard]] int getReportedOutputLatencySamples() const noexcept { return reportedOutputLatencySamples_; }

    [[nodiscard]] double getCurrentSampleRate() const noexcept { return currentSampleRate_; }

    [[nodiscard]] int getCurrentBufferSizeSamples() const noexcept { return currentBufferSamples_; }

    [[nodiscard]] juce::String getCurrentDeviceTypeName() const { return deviceTypeName_; }

    [[nodiscard]] juce::String getCurrentDeviceName() const { return deviceIoName_; }

    void setCurrentRecordingOffsetSamples(std::int64_t v);
    void setCurrentPlaybackOffsetSamples(std::int64_t v);
    void resetRecordingToMinusReportedInput();
    void resetPlaybackToZero();
    void setPlaybackToReportedOutputLatency();

    void save();

private:
    struct Offsets final
    {
        std::int64_t recording{ 0 };
        std::int64_t playback{ 0 };
    };

    [[nodiscard]] juce::String makeDeviceKey(const juce::String& typeName, const juce::String& deviceName) const;
    [[nodiscard]] std::int64_t defaultRecordingOffsetFromReportedInput() const noexcept;
    void applyStoredOrDefaultsForCurrentKey();
    void persistAllKnownDevicesToFile();

    juce::AudioDeviceManager& deviceManager_;
    juce::File persistenceFile_;

    std::map<juce::String, Offsets> perDeviceStored_;

    juce::String currentKey_;
    juce::String deviceTypeName_;
    juce::String deviceIoName_;
    double currentSampleRate_{ 0.0 };
    int currentBufferSamples_{ 0 };
    int reportedInputLatencySamples_{ 0 };
    int reportedOutputLatencySamples_{ 0 };

    std::int64_t recordingOffsetSamples_{ 0 };
    std::int64_t playbackOffsetSamples_{ 0 };

    LatencySettingsStore(const LatencySettingsStore&) = delete;
    LatencySettingsStore& operator=(const LatencySettingsStore&) = delete;
    LatencySettingsStore(LatencySettingsStore&&) = delete;
    LatencySettingsStore& operator=(LatencySettingsStore&&) = delete;
};
