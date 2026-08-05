#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "io/ProjectFile.h"

#include "ui/experimental/ExperimentalMidiEditorWindow.h"

class Transport;
class Session;
class RecorderService;
class ExperimentalInstrumentHost;
class TimelineViewportModel;

using InstrumentMusicalUndoSnapshot = ProjectFileExperimentalInstrumentTrackV1;

/// Owns MIDI editor clip binding, external transport shortcuts, undo UI hooks, and device timeline sample-rate sync for instrument clips.
/// `midiEditorWindow` remains allocated on `TransportControlsContent`; presenter holds it by reference.
class MidiEditorPresenter final
{
public:
    struct Callbacks
    {
        std::function<InstrumentTrackController*(TrackId)> getInstrumentControllerForTrack;
        std::function<ExperimentalInstrumentHost*(TrackId)> getInstrumentHostForTrack;

        std::function<void(const juce::String&, std::function<bool()>)> executeUndoableInstrumentEdit;

        std::function<void()> invokeUndo;
        std::function<void()> invokeRedo;
        std::function<void()> invokePlayPauseToggle;
        std::function<void()> invokeStopOrSeek;
        std::function<void()> invokeRecordToggle;
        std::function<void()> invokeJumpToLeftLocator;

        std::function<bool()> isCountInActive;
        std::function<bool()> isUiInputBlockedByRecording;

        std::function<void()> repaintInstrumentTrackRows;
        std::function<void()> refreshInstrumentUi;

        /// Same as legacy `TransportControlsContent::syncInstrumentClipTimelineFromDevice` (staging + all keyed controllers receive device sample-rate).
        std::function<void(double sampleRate)> applyInstrumentClipTimelineSampleRate;

        /// After MIDI editor changes shared `Session` arrangement snap resolution: refresh main toolbar combo (no extra snap math).
        std::function<void()> syncArrangementSnapToolbarFromSession;
    };

    MidiEditorPresenter(Transport& transport,
                        Session& session,
                        juce::AudioDeviceManager& deviceManager,
                        RecorderService& recorder,
                        TimelineViewportModel& timelineViewport,
                        std::unique_ptr<ExperimentalMidiEditorWindow>& midiEditorWindow,
                        Callbacks callbacks);

    void openMidiEditorForInstrumentClip(TrackId timelineInstrumentTrackId,
                                         InstrumentMidiClipId clipId);
    void detachToScratchAfterMissingInstrumentClip(const juce::String& reasonForUser);
    void rebindAfterInstrumentMusicalUndo();
    void refreshInstrumentUiIfOpen();
    void syncInstrumentClipTimelineFromDevice();

    /// When `Session` timeline ruler format (BBT / seconds) changes; updates MIDI editor toolbar if open.
    void syncTimelineRulerFormatUiIfEditorOpen();

    /// Main-window snap changed or project loaded; refresh MIDI editor snap toggle + resolution if open.
    void refreshArrangementSnapMirrorFromSession() noexcept;

    /// After seeking from shortcuts that already moved the session playhead (`invokeJumpToLeftLocatorFromWindowShortcut`).
    void notifyMidiEditorExternalTransportSeekIfOpen(std::int64_t targetSample) noexcept;

    [[nodiscard]] std::optional<TrackId> openedTrackId() const noexcept;

    [[nodiscard]] ExperimentalMidiEditorWindow* midiEditorWindow() const noexcept;

    void snapshotOpenClipViewportFromRollIfOpen() noexcept;
    void resetWindowAndBooking() noexcept;

    /// When deleting an instrument lane / track that had the MIDI editor targeted at it.
    void resetWindowAndBookingIfOpenOnTrack(TrackId tid) noexcept;

private:
    [[nodiscard]] ExperimentalMidiTransportCommands makeMidiEditorTransportCommands();
    void wireMidiEditorForOpenClip(TrackId timelineInstrumentTrackId, InstrumentMidiClip* clip);

    Transport& transport_;
    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    TimelineViewportModel& timelineViewport_;
    std::unique_ptr<ExperimentalMidiEditorWindow>& midiEditorWindow_;
    Callbacks callbacks_;
    std::optional<TrackId> midiEditorOpenedForInstrumentTrackId_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiEditorPresenter)
};
