#pragma once

#include <JuceHeader.h>

#include <functional>
#include <optional>

#include "domain/Track.h"
#include "util/AsyncLifetimeToken.h"

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

    /// Same file-picker + import as `addClipAtPlayheadClicked`, but ensures the clip is placed on
    /// `trackId` by activating it immediately before the session mutation (message thread).
    void addClipAtPlayheadForAudioTrack(TrackId trackId);

private:
    void launchAddClipAtPlayheadPicker(std::optional<TrackId> activateTrackBeforeImport);

    Session& session_;
    Transport& transport_;
    juce::AudioDeviceManager& deviceManager_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
    bool importInFlight_ = false;
    /// Stability Slice 4: FileChooser completions check this before touching the coordinator.
    mini_daw::AsyncLifetimeOwnerToken asyncLifetime_;
};
