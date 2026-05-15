#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>

#include "domain/AudioClip.h"

class Session;
class Transport;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;

/// Message-thread clip clipboard for window shortcuts (delete/copy/paste). Delegates undo session
/// edits to the owner via callbacks.
class ClipPasteboardController
{
public:
    struct Callbacks
    {
        std::function<bool()> isRecording;
        std::function<bool()> isCountInActive;
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableSessionEdit;
        std::function<void()> syncViewportFromSession;
    };

    ClipPasteboardController(Session& session,
                             Transport& transport,
                             TrackLanesView& trackLanesView,
                             TimelineRulerView& rulerView,
                             InspectorView& inspectorView,
                             Callbacks callbacks);

    void invokeDeleteSelectedPlacedClipFromWindowShortcut();
    void invokeCopySelectedClipFromWindowShortcut();
    void invokePasteClipFromWindowShortcut();

private:
    struct InternalClipPasteboard
    {
        std::shared_ptr<const AudioClip> material;
        std::int64_t leftTrimSamples = 0;
        std::int64_t visibleLengthSamples = 0;
        std::int64_t materialWindowStartSamples = 0;
        std::int64_t materialWindowEndExclusiveSamples = 0;
    };

    Session& session_;
    Transport& transport_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
    std::optional<InternalClipPasteboard> clipPasteboard_;
};
