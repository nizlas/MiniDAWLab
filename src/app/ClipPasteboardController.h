#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "domain/AudioClip.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"

class Session;
class Transport;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;

/// Message-thread clip clipboard for window shortcuts (delete/copy/paste). Delegates undo session
/// edits and instrument musical edits to the owner via callbacks.
class ClipPasteboardController
{
public:
    enum class PayloadKind : std::uint8_t
    {
        None = 0,
        Audio,
        InstrumentMidi,
    };

    struct Callbacks
    {
        std::function<bool()> isRecording;
        std::function<bool()> isCountInActive;
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableSessionEdit;
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableInstrumentEdit;
        std::function<InstrumentTrackController*(TrackId)> getInstrumentControllerForTrack;
        std::function<void()> syncViewportFromSession;
        std::function<void()> refreshInstrumentArrangementUi;
        std::function<void(TrackId timelineInstrumentTrackId, InstrumentMidiClipId clipId)> openMidiEditorForInstrumentClip;
        std::function<std::int64_t(std::int64_t timelineSample)> snapArrangementTimelineSample;
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

    struct InternalInstrumentMidiPasteboard
    {
        TrackId sourceTrackId = kInvalidTrackId;
        std::int64_t groupEarliestStartSamples = 0;
        /// Sorted by original `startSamples`. `InstrumentMidiClip::id` fields are stale and ignored on paste.
        std::vector<InstrumentMidiClip> clipsSortedByOriginalStart;
    };

    [[nodiscard]] TrackId resolveInstrumentMidiPasteTargetTrack(TrackId sourceTrackFromPasteboard) const noexcept;

    Session& session_;
    Transport& transport_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;

    PayloadKind payloadKind_ = PayloadKind::None;
    std::optional<InternalClipPasteboard> audioPasteboard_;
    std::optional<InternalInstrumentMidiPasteboard> instrumentMidiPasteboard_;
};
