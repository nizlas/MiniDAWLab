#pragma once

#include <JuceHeader.h>

#include <functional>

class LatencySettingsStore;
class LatencySettingsView;
class PlaybackEngine;
class RecorderService;
class RecordingCoordinator;
class Session;
class Transport;

namespace mini_daw_app_dialogs
{

void showAudioSettingsDialog(juce::Component& parent,
                             Transport& transport,
                             std::function<void()> updatePlayPauseButtonFromTransport,
                             RecorderService& recorder,
                             RecordingCoordinator& recordingCoordinator,
                             juce::AudioDeviceManager& deviceManager,
                             LatencySettingsStore& latencyStore,
                             PlaybackEngine& playbackEngine,
                             juce::Component::SafePointer<LatencySettingsView>& audioLatencySettingsWeakSlot);

void showAudioMixdownDialog(juce::Component& parent,
                            Transport& transport,
                            Session& session,
                            PlaybackEngine& playbackEngine,
                            juce::AudioDeviceManager& deviceManager,
                            std::function<void()> updatePlayPauseButtonFromTransport,
                            /// Slice 5 unsaved-changes guard; invoked with the export continuation
                            /// when the user clicks Export. Null: export proceeds directly.
                            std::function<void(std::function<void()>)> confirmSaveBeforeExport = nullptr);

void showHelpMenuPopup(juce::Component& helpButtonAnchor,
                       juce::Component::SafePointer<juce::Component> menuOwnerLifetime,
                       std::function<void()> showUndoBehaviorDialog);

void showUndoBehaviorDialog();

} // namespace mini_daw_app_dialogs
