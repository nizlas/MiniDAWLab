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

        /// Ctrl+S from the MIDI editor: same save flow as File -> Save Project (`ProjectIoCoordinator`).
        std::function<void()> invokeSaveProject;
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

    /// Project load: remember (or clear) saved MIDI editor window bounds and internal view state
    /// for this project. Never opens the editor here (see `tryRestoreMidiEditorWorkspaceAfterProjectLoad`).
    void setMidiEditorWindowBoundsFromLoadedProject(const ProjectFileV1& projectFile) noexcept;

    /// Project save: live window bounds if the editor window exists (even hidden), else the last
    /// remembered bounds (from load or a previously open editor). nullopt = omit from project file.
    [[nodiscard]] std::optional<ProjectFileMainWindowBoundsV1>
    getMidiEditorWindowBoundsForProjectSave() noexcept;

    /// Project save (Conny 1B): open state + bound track/clip + internal view. Only returned while
    /// the editor window is actually visible; a closed editor omits the workspace (no auto-reopen).
    [[nodiscard]] std::optional<ProjectFileMidiEditorWorkspaceV1>
    getMidiEditorWorkspaceForProjectSave() noexcept;

    /// After a completed project load: reopen the MIDI editor when the project saved it as open.
    /// Missing track/clip is skipped safely with a project-load diagnostic line. Never opens plugin
    /// editor windows.
    void tryRestoreMidiEditorWorkspaceAfterProjectLoad(const ProjectFileV1& projectFile) noexcept;

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
    /// Last known MIDI editor window bounds for the current project (loaded or user-moved).
    std::optional<ProjectFileMainWindowBoundsV1> midiEditorWindowBoundsMemo_;
    /// Last known internal view state (vertical pitch scroll, velocity lane, rows mode) and the
    /// track it belongs to. Track-specific fields are only re-applied when reopening the same track.
    MidiEditorWorkspaceUiState midiEditorUiStateMemo_;
    std::optional<TrackId> midiEditorUiStateMemoTrackId_;

    void rememberMidiEditorWindowBoundsIfWindowExists() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiEditorPresenter)
};
