#pragma once

#include <JuceHeader.h>

#include <functional>

class Session;
class Transport;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;

/// Owns the Add clip file-picker flow (project Audio/ import + placement at playhead).
class AudioClipImportCoordinator
{
public:
    struct Callbacks
    {
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableSessionEdit;
        std::function<void()> syncViewportFromSession;
    };

    AudioClipImportCoordinator(Session& session,
                               Transport& transport,
                               juce::AudioDeviceManager& deviceManager,
                               TrackLanesView& trackLanesView,
                               TimelineRulerView& rulerView,
                               InspectorView& inspectorView,
                               Callbacks callbacks);

    void addClipAtPlayheadClicked();

private:
    Session& session_;
    Transport& transport_;
    juce::AudioDeviceManager& deviceManager_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
    bool importInFlight_ = false;
};
